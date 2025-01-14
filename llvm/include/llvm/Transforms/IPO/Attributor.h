//===- Attributor.h --- Module-wide attribute deduction ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Attributor: An inter procedural (abstract) "attribute" deduction framework.
//
// The Attributor framework is an inter procedural abstract analysis (fixpoint
// iteration analysis). The goal is to allow easy deduction of new attributes as
// well as information exchange between abstract attributes in-flight.
//
// The Attributor class is the driver and the link between the various abstract
// attributes. The Attributor will iterate until a fixpoint state is reached by
// all abstract attributes in-flight, or until it will enforce a pessimistic fix
// point because an iteration limit is reached.
//
// Abstract attributes, derived from the AbstractAttribute class, actually
// describe properties of the code. They can correspond to actual LLVM-IR
// attributes, or they can be more general, ultimately unrelated to LLVM-IR
// attributes. The latter is useful when an abstract attributes provides
// information to other abstract attributes in-flight but we might not want to
// manifest the information. The Attributor allows to query in-flight abstract
// attributes through the `Attributor::getAAFor` method (see the method
// description for an example). If the method is used by an abstract attribute
// P, and it results in an abstract attribute Q, the Attributor will
// automatically capture a potential dependence from Q to P. This dependence
// will cause P to be reevaluated whenever Q changes in the future.
//
// The Attributor will only reevaluated abstract attributes that might have
// changed since the last iteration. That means that the Attribute will not
// revisit all instructions/blocks/functions in the module but only query
// an update from a subset of the abstract attributes.
//
// The update method `AbstractAttribute::updateImpl` is implemented by the
// specific "abstract attribute" subclasses. The method is invoked whenever the
// currently assumed state (see the AbstractState class) might not be valid
// anymore. This can, for example, happen if the state was dependent on another
// abstract attribute that changed. In every invocation, the update method has
// to adjust the internal state of an abstract attribute to a point that is
// justifiable by the underlying IR and the current state of abstract attributes
// in-flight. Since the IR is given and assumed to be valid, the information
// derived from it can be assumed to hold. However, information derived from
// other abstract attributes is conditional on various things. If the justifying
// state changed, the `updateImpl` has to revisit the situation and potentially
// find another justification or limit the optimistic assumes made.
//
// Change is the key in this framework. Until a state of no-change, thus a
// fixpoint, is reached, the Attributor will query the abstract attributes
// in-flight to re-evaluate their state. If the (current) state is too
// optimistic, hence it cannot be justified anymore through other abstract
// attributes or the state of the IR, the state of the abstract attribute will
// have to change. Generally, we assume abstract attribute state to be a finite
// height lattice and the update function to be monotone. However, these
// conditions are not enforced because the iteration limit will guarantee
// termination. If an optimistic fixpoint is reached, or a pessimistic fix
// point is enforced after a timeout, the abstract attributes are tasked to
// manifest their result in the IR for passes to come.
//
// Attribute manifestation is not mandatory. If desired, there is support to
// generate a single or multiple LLVM-IR attributes already in the helper struct
// IRAttribute. In the simplest case, a subclass inherits from IRAttribute with
// a proper Attribute::AttrKind as template parameter. The Attributor
// manifestation framework will then create and place a new attribute if it is
// allowed to do so (based on the abstract state). Other use cases can be
// achieved by overloading AbstractAttribute or IRAttribute methods.
//
//
// The "mechanics" of adding a new "abstract attribute":
// - Define a class (transitively) inheriting from AbstractAttribute and one
//   (which could be the same) that (transitively) inherits from AbstractState.
//   For the latter, consider the already available BooleanState and
//   {Inc,Dec,Bit}IntegerState if they fit your needs, e.g., you require only a
//   number tracking or bit-encoding.
// - Implement all pure methods. Also use overloading if the attribute is not
//   conforming with the "default" behavior: A (set of) LLVM-IR attribute(s) for
//   an argument, call site argument, function return value, or function. See
//   the class and method descriptions for more information on the two
//   "Abstract" classes and their respective methods.
// - Register opportunities for the new abstract attribute in the
//   `Attributor::identifyDefaultAbstractAttributes` method if it should be
//   counted as a 'default' attribute.
// - Add sufficient tests.
// - Add a Statistics object for bookkeeping. If it is a simple (set of)
//   attribute(s) manifested through the Attributor manifestation framework, see
//   the bookkeeping function in Attributor.cpp.
// - If instructions with a certain opcode are interesting to the attribute, add
//   that opcode to the switch in `Attributor::identifyAbstractAttributes`. This
//   will make it possible to query all those instructions through the
//   `InformationCache::getOpcodeInstMapForFunction` interface and eliminate the
//   need to traverse the IR repeatedly.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_IPO_ATTRIBUTOR_H
#define LLVM_TRANSFORMS_IPO_ATTRIBUTOR_H

#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/MustExecute.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/PassManager.h"

namespace llvm {

struct AbstractAttribute;
struct InformationCache;
struct AAIsDead;

class Function;

/// Simple enum class that forces the status to be spelled out explicitly.
///
///{
enum class ChangeStatus {
  CHANGED,
  UNCHANGED,
};

ChangeStatus operator|(ChangeStatus l, ChangeStatus r);
ChangeStatus operator&(ChangeStatus l, ChangeStatus r);
///}

/// Helper to describe and deal with positions in the LLVM-IR.
///
/// A position in the IR is described by an anchor value and an "offset" that
/// could be the argument number, for call sites and arguments, or an indicator
/// of the "position kind". The kinds, specified in the Kind enum below, include
/// the locations in the attribute list, i.a., function scope and return value,
/// as well as a distinction between call sites and functions. Finally, there
/// are floating values that do not have a corresponding attribute list
/// position.
struct IRPosition {
  virtual ~IRPosition() {}

  /// The positions we distinguish in the IR.
  ///
  /// The values are chosen such that the KindOrArgNo member has a value >= 1
  /// if it is an argument or call site argument while a value < 1 indicates the
  /// respective kind of that value.
  enum Kind : int {
    IRP_INVALID = -6, ///< An invalid position.
    IRP_FLOAT = -5, ///< A position that is not associated with a spot suitable
                    ///< for attributes. This could be any value or instruction.
    IRP_RETURNED = -4, ///< An attribute for the function return value.
    IRP_CALL_SITE_RETURNED = -3, ///< An attribute for a call site return value.
    IRP_FUNCTION = -2,           ///< An attribute for a function (scope).
    IRP_CALL_SITE = -1, ///< An attribute for a call site (function scope).
    IRP_ARGUMENT = 0,   ///< An attribute for a function argument.
    IRP_CALL_SITE_ARGUMENT = 1, ///< An attribute for a call site argument.
  };

  /// Default constructor available to create invalid positions implicitly. All
  /// other positions need to be created explicitly through the appropriate
  /// static member function.
  IRPosition() : AnchorVal(nullptr), KindOrArgNo(IRP_INVALID) { verify(); }

  /// Create a position describing the value of \p V.
  static const IRPosition value(const Value &V) {
    if (auto *Arg = dyn_cast<Argument>(&V))
      return IRPosition::argument(*Arg);
    if (auto *CB = dyn_cast<CallBase>(&V))
      return IRPosition::callsite_returned(*CB);
    return IRPosition(const_cast<Value &>(V), IRP_FLOAT);
  }

  /// Create a position describing the function scope of \p F.
  static const IRPosition function(const Function &F) {
    return IRPosition(const_cast<Function &>(F), IRP_FUNCTION);
  }

  /// Create a position describing the returned value of \p F.
  static const IRPosition returned(const Function &F) {
    return IRPosition(const_cast<Function &>(F), IRP_RETURNED);
  }

  /// Create a position describing the argument \p Arg.
  static const IRPosition argument(const Argument &Arg) {
    return IRPosition(const_cast<Argument &>(Arg), Kind(Arg.getArgNo()));
  }

  /// Create a position describing the function scope of \p CB.
  static const IRPosition callsite_function(const CallBase &CB) {
    return IRPosition(const_cast<CallBase &>(CB), IRP_CALL_SITE);
  }

  /// Create a position describing the returned value of \p CB.
  static const IRPosition callsite_returned(const CallBase &CB) {
    return IRPosition(const_cast<CallBase &>(CB), IRP_CALL_SITE_RETURNED);
  }

  /// Create a position describing the argument of \p CB at position \p ArgNo.
  static const IRPosition callsite_argument(const CallBase &CB,
                                            unsigned ArgNo) {
    return IRPosition(const_cast<CallBase &>(CB), Kind(ArgNo));
  }

  /// Create a position describing the function scope of \p ICS.
  static const IRPosition callsite_function(ImmutableCallSite ICS) {
    return IRPosition::callsite_function(cast<CallBase>(*ICS.getInstruction()));
  }

  /// Create a position describing the returned value of \p ICS.
  static const IRPosition callsite_returned(ImmutableCallSite ICS) {
    return IRPosition::callsite_returned(cast<CallBase>(*ICS.getInstruction()));
  }

  /// Create a position describing the argument of \p ICS at position \p ArgNo.
  static const IRPosition callsite_argument(ImmutableCallSite ICS,
                                            unsigned ArgNo) {
    return IRPosition::callsite_argument(cast<CallBase>(*ICS.getInstruction()),
                                         ArgNo);
  }

  /// Create a position describing the argument of \p ACS at position \p ArgNo.
  static const IRPosition callsite_argument(AbstractCallSite ACS,
                                            unsigned ArgNo) {
    int CSArgNo = ACS.getCallArgOperandNo(ArgNo);
    if (CSArgNo >= 0)
      return IRPosition::callsite_argument(
          cast<CallBase>(*ACS.getInstruction()), CSArgNo);
    return IRPosition();
  }

  /// Create a position with function scope matching the "context" of \p IRP.
  /// If \p IRP is a call site (see isAnyCallSitePosition()) then the result
  /// will be a call site position, otherwise the function position of the
  /// associated function.
  static const IRPosition function_scope(const IRPosition &IRP) {
    if (IRP.isAnyCallSitePosition()) {
      return IRPosition::callsite_function(
          cast<CallBase>(IRP.getAnchorValue()));
    }
    assert(IRP.getAssociatedFunction());
    return IRPosition::function(*IRP.getAssociatedFunction());
  }

  bool operator==(const IRPosition &RHS) const {
    return (AnchorVal == RHS.AnchorVal) && (KindOrArgNo == RHS.KindOrArgNo);
  }
  bool operator!=(const IRPosition &RHS) const { return !(*this == RHS); }

  /// Return the value this abstract attribute is anchored with.
  ///
  /// The anchor value might not be the associated value if the latter is not
  /// sufficient to determine where arguments will be manifested. This is, so
  /// far, only the case for call site arguments as the value is not sufficient
  /// to pinpoint them. Instead, we can use the call site as an anchor.
  Value &getAnchorValue() const {
    assert(KindOrArgNo != IRP_INVALID &&
           "Invalid position does not have an anchor value!");
    return *AnchorVal;
  }

  /// Return the associated function, if any.
  Function *getAssociatedFunction() const {
    if (auto *CB = dyn_cast<CallBase>(AnchorVal))
      return CB->getCalledFunction();
    assert(KindOrArgNo != IRP_INVALID &&
           "Invalid position does not have an anchor scope!");
    Value &V = getAnchorValue();
    if (isa<Function>(V))
      return &cast<Function>(V);
    if (isa<Argument>(V))
      return cast<Argument>(V).getParent();
    if (isa<Instruction>(V))
      return cast<Instruction>(V).getFunction();
    return nullptr;
  }

  /// Return the associated argument, if any.
  Argument *getAssociatedArgument() const {
    if (auto *Arg = dyn_cast<Argument>(&getAnchorValue()))
      return Arg;
    int ArgNo = getArgNo();
    if (ArgNo < 0)
      return nullptr;
    Function *AssociatedFn = getAssociatedFunction();
    if (!AssociatedFn || AssociatedFn->arg_size() <= unsigned(ArgNo))
      return nullptr;
    return AssociatedFn->arg_begin() + ArgNo;
  }

  /// Return true if the position refers to a function interface, that is the
  /// function scope, the function return, or an argumnt.
  bool isFnInterfaceKind() const {
    switch (getPositionKind()) {
    case IRPosition::IRP_FUNCTION:
    case IRPosition::IRP_RETURNED:
    case IRPosition::IRP_ARGUMENT:
      return true;
    default:
      return false;
    }
  }

  /// Return the Function surrounding the anchor value.
  Function *getAnchorScope() const {
    Value &V = getAnchorValue();
    if (isa<Function>(V))
      return &cast<Function>(V);
    if (isa<Argument>(V))
      return cast<Argument>(V).getParent();
    if (isa<Instruction>(V))
      return cast<Instruction>(V).getFunction();
    return nullptr;
  }

  /// Return the context instruction, if any.
  Instruction *getCtxI() const {
    Value &V = getAnchorValue();
    if (auto *I = dyn_cast<Instruction>(&V))
      return I;
    if (auto *Arg = dyn_cast<Argument>(&V))
      if (!Arg->getParent()->isDeclaration())
        return &Arg->getParent()->getEntryBlock().front();
    if (auto *F = dyn_cast<Function>(&V))
      if (!F->isDeclaration())
        return &(F->getEntryBlock().front());
    return nullptr;
  }

  /// Return the value this abstract attribute is associated with.
  Value &getAssociatedValue() const {
    assert(KindOrArgNo != IRP_INVALID &&
           "Invalid position does not have an associated value!");
    if (getArgNo() < 0 || isa<Argument>(AnchorVal))
      return *AnchorVal;
    assert(isa<CallBase>(AnchorVal) && "Expected a call base!");
    return *cast<CallBase>(AnchorVal)->getArgOperand(getArgNo());
  }

  /// Return the argument number of the associated value if it is an argument or
  /// call site argument, otherwise a negative value.
  int getArgNo() const { return KindOrArgNo; }

  /// Return the index in the attribute list for this position.
  unsigned getAttrIdx() const {
    switch (getPositionKind()) {
    case IRPosition::IRP_INVALID:
    case IRPosition::IRP_FLOAT:
      break;
    case IRPosition::IRP_FUNCTION:
    case IRPosition::IRP_CALL_SITE:
      return AttributeList::FunctionIndex;
    case IRPosition::IRP_RETURNED:
    case IRPosition::IRP_CALL_SITE_RETURNED:
      return AttributeList::ReturnIndex;
    case IRPosition::IRP_ARGUMENT:
    case IRPosition::IRP_CALL_SITE_ARGUMENT:
      return KindOrArgNo + AttributeList::FirstArgIndex;
    }
    llvm_unreachable(
        "There is no attribute index for a floating or invalid position!");
  }

  /// Return the associated position kind.
  Kind getPositionKind() const {
    if (getArgNo() >= 0) {
      assert(((isa<Argument>(getAnchorValue()) &&
               isa<Argument>(getAssociatedValue())) ||
              isa<CallBase>(getAnchorValue())) &&
             "Expected argument or call base due to argument number!");
      if (isa<CallBase>(getAnchorValue()))
        return IRP_CALL_SITE_ARGUMENT;
      return IRP_ARGUMENT;
    }

    assert(KindOrArgNo < 0 &&
           "Expected (call site) arguments to never reach this point!");
    return Kind(KindOrArgNo);
  }

  /// TODO: Figure out if the attribute related helper functions should live
  ///       here or somewhere else.

  /// Return true if any kind in \p AKs existing in the IR at a position that
  /// will affect this one. See also getAttrs(...).
  /// \param IgnoreSubsumingPositions Flag to determine if subsuming positions,
  ///                                 e.g., the function position if this is an
  ///                                 argument position, should be ignored.
  bool hasAttr(ArrayRef<Attribute::AttrKind> AKs,
               bool IgnoreSubsumingPositions = false) const;

  /// Return the attributes of any kind in \p AKs existing in the IR at a
  /// position that will affect this one. While each position can only have a
  /// single attribute of any kind in \p AKs, there are "subsuming" positions
  /// that could have an attribute as well. This method returns all attributes
  /// found in \p Attrs.
  void getAttrs(ArrayRef<Attribute::AttrKind> AKs,
                SmallVectorImpl<Attribute> &Attrs) const;

  /// Return the attribute of kind \p AK existing in the IR at this position.
  Attribute getAttr(Attribute::AttrKind AK) const {
    if (getPositionKind() == IRP_INVALID || getPositionKind() == IRP_FLOAT)
      return Attribute();

    AttributeList AttrList;
    if (ImmutableCallSite ICS = ImmutableCallSite(&getAnchorValue()))
      AttrList = ICS.getAttributes();
    else
      AttrList = getAssociatedFunction()->getAttributes();

    if (AttrList.hasAttribute(getAttrIdx(), AK))
      return AttrList.getAttribute(getAttrIdx(), AK);
    return Attribute();
  }

  /// Remove the attribute of kind \p AKs existing in the IR at this position.
  void removeAttrs(ArrayRef<Attribute::AttrKind> AKs) const {
    if (getPositionKind() == IRP_INVALID || getPositionKind() == IRP_FLOAT)
      return;

    AttributeList AttrList;
    CallSite CS = CallSite(&getAnchorValue());
    if (CS)
      AttrList = CS.getAttributes();
    else
      AttrList = getAssociatedFunction()->getAttributes();

    LLVMContext &Ctx = getAnchorValue().getContext();
    for (Attribute::AttrKind AK : AKs)
      AttrList = AttrList.removeAttribute(Ctx, getAttrIdx(), AK);

    if (CS)
      CS.setAttributes(AttrList);
    else
      getAssociatedFunction()->setAttributes(AttrList);
  }

  bool isAnyCallSitePosition() const {
    switch (getPositionKind()) {
    case IRPosition::IRP_CALL_SITE:
    case IRPosition::IRP_CALL_SITE_RETURNED:
    case IRPosition::IRP_CALL_SITE_ARGUMENT:
      return true;
    default:
      return false;
    }
  }

  /// Special DenseMap key values.
  ///
  ///{
  static const IRPosition EmptyKey;
  static const IRPosition TombstoneKey;
  ///}

private:
  /// Private constructor for special values only!
  explicit IRPosition(int KindOrArgNo)
      : AnchorVal(0), KindOrArgNo(KindOrArgNo) {}

  /// IRPosition anchored at \p AnchorVal with kind/argument numbet \p PK.
  explicit IRPosition(Value &AnchorVal, Kind PK)
      : AnchorVal(&AnchorVal), KindOrArgNo(PK) {
    verify();
  }

  /// Verify internal invariants.
  void verify();

protected:
  /// The value this position is anchored at.
  Value *AnchorVal;

  /// The argument number, if non-negative, or the position "kind".
  int KindOrArgNo;
};

/// Helper that allows IRPosition as a key in a DenseMap.
template <> struct DenseMapInfo<IRPosition> {
  static inline IRPosition getEmptyKey() { return IRPosition::EmptyKey; }
  static inline IRPosition getTombstoneKey() {
    return IRPosition::TombstoneKey;
  }
  static unsigned getHashValue(const IRPosition &IRP) {
    return (DenseMapInfo<Value *>::getHashValue(&IRP.getAnchorValue()) << 4) ^
           (unsigned(IRP.getArgNo()));
  }
  static bool isEqual(const IRPosition &LHS, const IRPosition &RHS) {
    return LHS == RHS;
  }
};

/// A visitor class for IR positions.
///
/// Given a position P, the SubsumingPositionIterator allows to visit "subsuming
/// positions" wrt. attributes/information. Thus, if a piece of information
/// holds for a subsuming position, it also holds for the position P.
///
/// The subsuming positions always include the initial position and then,
/// depending on the position kind, additionally the following ones:
/// - for IRP_RETURNED:
///   - the function (IRP_FUNCTION)
/// - for IRP_ARGUMENT:
///   - the function (IRP_FUNCTION)
/// - for IRP_CALL_SITE:
///   - the callee (IRP_FUNCTION), if known
/// - for IRP_CALL_SITE_RETURNED:
///   - the callee (IRP_RETURNED), if known
///   - the call site (IRP_FUNCTION)
///   - the callee (IRP_FUNCTION), if known
/// - for IRP_CALL_SITE_ARGUMENT:
///   - the argument of the callee (IRP_ARGUMENT), if known
///   - the callee (IRP_FUNCTION), if known
///   - the position the call site argument is associated with if it is not
///     anchored to the call site, e.g., if it is an arugment then the argument
///     (IRP_ARGUMENT)
class SubsumingPositionIterator {
  SmallVector<IRPosition, 4> IRPositions;
  using iterator = decltype(IRPositions)::iterator;

public:
  SubsumingPositionIterator(const IRPosition &IRP);
  iterator begin() { return IRPositions.begin(); }
  iterator end() { return IRPositions.end(); }
};

/// Wrapper for FunctoinAnalysisManager.
struct AnalysisGetter {
  template <typename Analysis>
  typename Analysis::Result *getAnalysis(const Function &F) {
    if (!MAM || !F.getParent())
      return nullptr;
    auto &FAM = MAM->getResult<FunctionAnalysisManagerModuleProxy>(
                       const_cast<Module &>(*F.getParent()))
                    .getManager();
    return &FAM.getResult<Analysis>(const_cast<Function &>(F));
  }

  template <typename Analysis>
  typename Analysis::Result *getAnalysis(const Module &M) {
    if (!MAM)
      return nullptr;
    return &MAM->getResult<Analysis>(const_cast<Module &>(M));
  }
  AnalysisGetter(ModuleAnalysisManager &MAM) : MAM(&MAM) {}
  AnalysisGetter() {}

private:
  ModuleAnalysisManager *MAM = nullptr;
};

/// Data structure to hold cached (LLVM-IR) information.
///
/// All attributes are given an InformationCache object at creation time to
/// avoid inspection of the IR by all of them individually. This default
/// InformationCache will hold information required by 'default' attributes,
/// thus the ones deduced when Attributor::identifyDefaultAbstractAttributes(..)
/// is called.
///
/// If custom abstract attributes, registered manually through
/// Attributor::registerAA(...), need more information, especially if it is not
/// reusable, it is advised to inherit from the InformationCache and cast the
/// instance down in the abstract attributes.
struct InformationCache {
  InformationCache(const Module &M, AnalysisGetter &AG)
      : DL(M.getDataLayout()), Explorer(/* ExploreInterBlock */ true), AG(AG) {

    CallGraph *CG = AG.getAnalysis<CallGraphAnalysis>(M);
    if (!CG)
      return;

    DenseMap<const Function *, unsigned> SccSize;
    for (scc_iterator<CallGraph *> I = scc_begin(CG); !I.isAtEnd(); ++I) {
      for (CallGraphNode *Node : *I)
        SccSize[Node->getFunction()] = I->size();
    }
    SccSizeOpt = std::move(SccSize);
  }

  /// A map type from opcodes to instructions with this opcode.
  using OpcodeInstMapTy = DenseMap<unsigned, SmallVector<Instruction *, 32>>;

  /// Return the map that relates "interesting" opcodes with all instructions
  /// with that opcode in \p F.
  OpcodeInstMapTy &getOpcodeInstMapForFunction(const Function &F) {
    return FuncInstOpcodeMap[&F];
  }

  /// A vector type to hold instructions.
  using InstructionVectorTy = std::vector<Instruction *>;

  /// Return the instructions in \p F that may read or write memory.
  InstructionVectorTy &getReadOrWriteInstsForFunction(const Function &F) {
    return FuncRWInstsMap[&F];
  }

  /// Return MustBeExecutedContextExplorer
  MustBeExecutedContextExplorer &getMustBeExecutedContextExplorer() {
    return Explorer;
  }

  /// Return TargetLibraryInfo for function \p F.
  TargetLibraryInfo *getTargetLibraryInfoForFunction(const Function &F) {
    return AG.getAnalysis<TargetLibraryAnalysis>(F);
  }

  /// Return AliasAnalysis Result for function \p F.
  AAResults *getAAResultsForFunction(const Function &F) {
    return AG.getAnalysis<AAManager>(F);
  }

  /// Return SCC size on call graph for function \p F.
  unsigned getSccSize(const Function &F) {
    if (!SccSizeOpt.hasValue())
      return 0;
    return (SccSizeOpt.getValue())[&F];
  }

  /// Return datalayout used in the module.
  const DataLayout &getDL() { return DL; }

private:
  /// A map type from functions to opcode to instruction maps.
  using FuncInstOpcodeMapTy = DenseMap<const Function *, OpcodeInstMapTy>;

  /// A map type from functions to their read or write instructions.
  using FuncRWInstsMapTy = DenseMap<const Function *, InstructionVectorTy>;

  /// A nested map that remembers all instructions in a function with a certain
  /// instruction opcode (Instruction::getOpcode()).
  FuncInstOpcodeMapTy FuncInstOpcodeMap;

  /// A map from functions to their instructions that may read or write memory.
  FuncRWInstsMapTy FuncRWInstsMap;

  /// The datalayout used in the module.
  const DataLayout &DL;

  /// MustBeExecutedContextExplorer
  MustBeExecutedContextExplorer Explorer;

  /// Getters for analysis.
  AnalysisGetter &AG;

  /// Cache result for scc size in the call graph
  Optional<DenseMap<const Function *, unsigned>> SccSizeOpt;

  /// Give the Attributor access to the members so
  /// Attributor::identifyDefaultAbstractAttributes(...) can initialize them.
  friend struct Attributor;
};

/// The fixpoint analysis framework that orchestrates the attribute deduction.
///
/// The Attributor provides a general abstract analysis framework (guided
/// fixpoint iteration) as well as helper functions for the deduction of
/// (LLVM-IR) attributes. However, also other code properties can be deduced,
/// propagated, and ultimately manifested through the Attributor framework. This
/// is particularly useful if these properties interact with attributes and a
/// co-scheduled deduction allows to improve the solution. Even if not, thus if
/// attributes/properties are completely isolated, they should use the
/// Attributor framework to reduce the number of fixpoint iteration frameworks
/// in the code base. Note that the Attributor design makes sure that isolated
/// attributes are not impacted, in any way, by others derived at the same time
/// if there is no cross-reasoning performed.
///
/// The public facing interface of the Attributor is kept simple and basically
/// allows abstract attributes to one thing, query abstract attributes
/// in-flight. There are two reasons to do this:
///    a) The optimistic state of one abstract attribute can justify an
///       optimistic state of another, allowing to framework to end up with an
///       optimistic (=best possible) fixpoint instead of one based solely on
///       information in the IR.
///    b) This avoids reimplementing various kinds of lookups, e.g., to check
///       for existing IR attributes, in favor of a single lookups interface
///       provided by an abstract attribute subclass.
///
/// NOTE: The mechanics of adding a new "concrete" abstract attribute are
///       described in the file comment.
struct Attributor {
  /// Constructor
  ///
  /// \param InfoCache Cache to hold various information accessible for
  ///                  the abstract attributes.
  /// \param DepRecomputeInterval Number of iterations until the dependences
  ///                             between abstract attributes are recomputed.
  /// \param Whitelist If not null, a set limiting the attribute opportunities.
  Attributor(InformationCache &InfoCache, unsigned DepRecomputeInterval,
             DenseSet<const char *> *Whitelist = nullptr)
      : InfoCache(InfoCache), DepRecomputeInterval(DepRecomputeInterval),
        Whitelist(Whitelist) {}

  ~Attributor() { DeleteContainerPointers(AllAbstractAttributes); }

  /// Run the analyses until a fixpoint is reached or enforced (timeout).
  ///
  /// The attributes registered with this Attributor can be used after as long
  /// as the Attributor is not destroyed (it owns the attributes now).
  ///
  /// \Returns CHANGED if the IR was changed, otherwise UNCHANGED.
  ChangeStatus run(Module &M);

  /// Lookup an abstract attribute of type \p AAType at position \p IRP. While
  /// no abstract attribute is found equivalent positions are checked, see
  /// SubsumingPositionIterator. Thus, the returned abstract attribute
  /// might be anchored at a different position, e.g., the callee if \p IRP is a
  /// call base.
  ///
  /// This method is the only (supported) way an abstract attribute can retrieve
  /// information from another abstract attribute. As an example, take an
  /// abstract attribute that determines the memory access behavior for a
  /// argument (readnone, readonly, ...). It should use `getAAFor` to get the
  /// most optimistic information for other abstract attributes in-flight, e.g.
  /// the one reasoning about the "captured" state for the argument or the one
  /// reasoning on the memory access behavior of the function as a whole.
  ///
  /// If the flag \p TrackDependence is set to false the dependence from
  /// \p QueryingAA to the return abstract attribute is not automatically
  /// recorded. This should only be used if the caller will record the
  /// dependence explicitly if necessary, thus if it the returned abstract
  /// attribute is used for reasoning. To record the dependences explicitly use
  /// the `Attributor::recordDependence` method.
  template <typename AAType>
  const AAType &getAAFor(const AbstractAttribute &QueryingAA,
                         const IRPosition &IRP, bool TrackDependence = true) {
    return getOrCreateAAFor<AAType>(IRP, &QueryingAA, TrackDependence);
  }

  /// Explicitly record a dependence from \p FromAA to \p ToAA, that is if
  /// \p FromAA changes \p ToAA should be updated as well.
  ///
  /// This method should be used in conjunction with the `getAAFor` method and
  /// with the TrackDependence flag passed to the method set to false. This can
  /// be beneficial to avoid false dependences but it requires the users of
  /// `getAAFor` to explicitly record true dependences through this method.
  void recordDependence(const AbstractAttribute &FromAA,
                        const AbstractAttribute &ToAA);

  /// Introduce a new abstract attribute into the fixpoint analysis.
  ///
  /// Note that ownership of the attribute is given to the Attributor. It will
  /// invoke delete for the Attributor on destruction of the Attributor.
  ///
  /// Attributes are identified by their IR position (AAType::getIRPosition())
  /// and the address of their static member (see AAType::ID).
  template <typename AAType> AAType &registerAA(AAType &AA) {
    static_assert(std::is_base_of<AbstractAttribute, AAType>::value,
                  "Cannot register an attribute with a type not derived from "
                  "'AbstractAttribute'!");
    // Put the attribute in the lookup map structure and the container we use to
    // keep track of all attributes.
    const IRPosition &IRP = AA.getIRPosition();
    auto &KindToAbstractAttributeMap = AAMap[IRP];
    assert(!KindToAbstractAttributeMap.count(&AAType::ID) &&
           "Attribute already in map!");
    KindToAbstractAttributeMap[&AAType::ID] = &AA;
    AllAbstractAttributes.push_back(&AA);
    return AA;
  }

  /// Return the internal information cache.
  InformationCache &getInfoCache() { return InfoCache; }

  /// Determine opportunities to derive 'default' attributes in \p F and create
  /// abstract attribute objects for them.
  ///
  /// \param F The function that is checked for attribute opportunities.
  ///
  /// Note that abstract attribute instances are generally created even if the
  /// IR already contains the information they would deduce. The most important
  /// reason for this is the single interface, the one of the abstract attribute
  /// instance, which can be queried without the need to look at the IR in
  /// various places.
  void identifyDefaultAbstractAttributes(Function &F);

  /// Initialize the information cache for queries regarding function \p F.
  ///
  /// This method needs to be called for all function that might be looked at
  /// through the information cache interface *prior* to looking at them.
  void initializeInformationCache(Function &F);

  /// Mark the internal function \p F as live.
  ///
  /// This will trigger the identification and initialization of attributes for
  /// \p F.
  void markLiveInternalFunction(const Function &F) {
    assert(F.hasLocalLinkage() &&
           "Only local linkage is assumed dead initially.");

    identifyDefaultAbstractAttributes(const_cast<Function &>(F));
  }

  /// Record that \p U is to be replaces with \p NV after information was
  /// manifested. This also triggers deletion of trivially dead istructions.
  bool changeUseAfterManifest(Use &U, Value &NV) {
    Value *&V = ToBeChangedUses[&U];
    if (V && (V->stripPointerCasts() == NV.stripPointerCasts() ||
              isa_and_nonnull<UndefValue>(V)))
      return false;
    assert((!V || V == &NV || isa<UndefValue>(NV)) &&
           "Use was registered twice for replacement with different values!");
    V = &NV;
    return true;
  }

  /// Record that \p I is deleted after information was manifested. This also
  /// triggers deletion of trivially dead istructions.
  void deleteAfterManifest(Instruction &I) { ToBeDeletedInsts.insert(&I); }

  /// Record that \p BB is deleted after information was manifested. This also
  /// triggers deletion of trivially dead istructions.
  void deleteAfterManifest(BasicBlock &BB) { ToBeDeletedBlocks.insert(&BB); }

  /// Record that \p F is deleted after information was manifested.
  void deleteAfterManifest(Function &F) { ToBeDeletedFunctions.insert(&F); }

  /// Return true if \p AA (or its context instruction) is assumed dead.
  ///
  /// If \p LivenessAA is not provided it is queried.
  bool isAssumedDead(const AbstractAttribute &AA, const AAIsDead *LivenessAA);

  /// Check \p Pred on all (transitive) uses of \p V.
  ///
  /// This method will evaluate \p Pred on all (transitive) uses of the
  /// associated value and return true if \p Pred holds every time.
  bool checkForAllUses(const function_ref<bool(const Use &, bool &)> &Pred,
                       const AbstractAttribute &QueryingAA, const Value &V);

  /// Check \p Pred on all function call sites.
  ///
  /// This method will evaluate \p Pred on call sites and return
  /// true if \p Pred holds in every call sites. However, this is only possible
  /// all call sites are known, hence the function has internal linkage.
  bool checkForAllCallSites(const function_ref<bool(AbstractCallSite)> &Pred,
                            const AbstractAttribute &QueryingAA,
                            bool RequireAllCallSites);

  /// Check \p Pred on all values potentially returned by \p F.
  ///
  /// This method will evaluate \p Pred on all values potentially returned by
  /// the function associated with \p QueryingAA. The returned values are
  /// matched with their respective return instructions. Returns true if \p Pred
  /// holds on all of them.
  bool checkForAllReturnedValuesAndReturnInsts(
      const function_ref<bool(Value &, const SmallSetVector<ReturnInst *, 4> &)>
          &Pred,
      const AbstractAttribute &QueryingAA);

  /// Check \p Pred on all values potentially returned by the function
  /// associated with \p QueryingAA.
  ///
  /// This is the context insensitive version of the method above.
  bool checkForAllReturnedValues(const function_ref<bool(Value &)> &Pred,
                                 const AbstractAttribute &QueryingAA);

  /// Check \p Pred on all instructions with an opcode present in \p Opcodes.
  ///
  /// This method will evaluate \p Pred on all instructions with an opcode
  /// present in \p Opcode and return true if \p Pred holds on all of them.
  bool checkForAllInstructions(const function_ref<bool(Instruction &)> &Pred,
                               const AbstractAttribute &QueryingAA,
                               const ArrayRef<unsigned> &Opcodes);

  /// Check \p Pred on all call-like instructions (=CallBased derived).
  ///
  /// See checkForAllCallLikeInstructions(...) for more information.
  bool
  checkForAllCallLikeInstructions(const function_ref<bool(Instruction &)> &Pred,
                                  const AbstractAttribute &QueryingAA) {
    return checkForAllInstructions(Pred, QueryingAA,
                                   {(unsigned)Instruction::Invoke,
                                    (unsigned)Instruction::CallBr,
                                    (unsigned)Instruction::Call});
  }

  /// Check \p Pred on all Read/Write instructions.
  ///
  /// This method will evaluate \p Pred on all instructions that read or write
  /// to memory present in the information cache and return true if \p Pred
  /// holds on all of them.
  bool checkForAllReadWriteInstructions(
      const llvm::function_ref<bool(Instruction &)> &Pred,
      AbstractAttribute &QueryingAA);

  /// Return the data layout associated with the anchor scope.
  const DataLayout &getDataLayout() const { return InfoCache.DL; }

private:
  /// Check \p Pred on all call sites of \p Fn.
  ///
  /// This method will evaluate \p Pred on call sites and return
  /// true if \p Pred holds in every call sites. However, this is only possible
  /// all call sites are known, hence the function has internal linkage.
  bool checkForAllCallSites(const function_ref<bool(AbstractCallSite)> &Pred,
                            const Function &Fn, bool RequireAllCallSites,
                            const AbstractAttribute *QueryingAA);

  /// The private version of getAAFor that allows to omit a querying abstract
  /// attribute. See also the public getAAFor method.
  template <typename AAType>
  const AAType &getOrCreateAAFor(const IRPosition &IRP,
                                 const AbstractAttribute *QueryingAA = nullptr,
                                 bool TrackDependence = false) {
    if (const AAType *AAPtr =
            lookupAAFor<AAType>(IRP, QueryingAA, TrackDependence))
      return *AAPtr;

    // No matching attribute found, create one.
    // Use the static create method.
    auto &AA = AAType::createForPosition(IRP, *this);
    registerAA(AA);

    // For now we ignore naked and optnone functions.
    bool Invalidate = Whitelist && !Whitelist->count(&AAType::ID);
    if (const Function *Fn = IRP.getAnchorScope())
      Invalidate |= Fn->hasFnAttribute(Attribute::Naked) ||
                    Fn->hasFnAttribute(Attribute::OptimizeNone);

    // Bootstrap the new attribute with an initial update to propagate
    // information, e.g., function -> call site. If it is not on a given
    // whitelist we will not perform updates at all.
    if (Invalidate) {
      AA.getState().indicatePessimisticFixpoint();
      return AA;
    }

    AA.initialize(*this);
    AA.update(*this);

    if (TrackDependence && AA.getState().isValidState())
      recordDependence(AA, const_cast<AbstractAttribute &>(*QueryingAA));
    return AA;
  }

  /// Return the attribute of \p AAType for \p IRP if existing.
  template <typename AAType>
  const AAType *lookupAAFor(const IRPosition &IRP,
                            const AbstractAttribute *QueryingAA = nullptr,
                            bool TrackDependence = false) {
    static_assert(std::is_base_of<AbstractAttribute, AAType>::value,
                  "Cannot query an attribute with a type not derived from "
                  "'AbstractAttribute'!");
    assert((QueryingAA || !TrackDependence) &&
           "Cannot track dependences without a QueryingAA!");

    // Lookup the abstract attribute of type AAType. If found, return it after
    // registering a dependence of QueryingAA on the one returned attribute.
    const auto &KindToAbstractAttributeMap = AAMap.lookup(IRP);
    if (AAType *AA = static_cast<AAType *>(
            KindToAbstractAttributeMap.lookup(&AAType::ID))) {
      // Do not register a dependence on an attribute with an invalid state.
      if (TrackDependence && AA->getState().isValidState())
        recordDependence(*AA, const_cast<AbstractAttribute &>(*QueryingAA));
      return AA;
    }
    return nullptr;
  }

  /// The set of all abstract attributes.
  ///{
  using AAVector = SmallVector<AbstractAttribute *, 64>;
  AAVector AllAbstractAttributes;
  ///}

  /// A nested map to lookup abstract attributes based on the argument position
  /// on the outer level, and the addresses of the static member (AAType::ID) on
  /// the inner level.
  ///{
  using KindToAbstractAttributeMap =
      DenseMap<const char *, AbstractAttribute *>;
  DenseMap<IRPosition, KindToAbstractAttributeMap> AAMap;
  ///}

  /// A map from abstract attributes to the ones that queried them through calls
  /// to the getAAFor<...>(...) method.
  ///{
  using QueryMapTy =
      MapVector<const AbstractAttribute *, SetVector<AbstractAttribute *>>;
  QueryMapTy QueryMap;
  ///}

  /// The information cache that holds pre-processed (LLVM-IR) information.
  InformationCache &InfoCache;

  /// Set if the attribute currently updated did query a non-fix attribute.
  bool QueriedNonFixAA;

  /// Number of iterations until the dependences between abstract attributes are
  /// recomputed.
  const unsigned DepRecomputeInterval;

  /// If not null, a set limiting the attribute opportunities.
  const DenseSet<const char *> *Whitelist;

  /// A set to remember the functions we already assume to be live and visited.
  DenseSet<const Function *> VisitedFunctions;

  /// Uses we replace with a new value after manifest is done. We will remove
  /// then trivially dead instructions as well.
  DenseMap<Use *, Value *> ToBeChangedUses;

  /// Functions, blocks, and instructions we delete after manifest is done.
  ///
  ///{
  SmallPtrSet<Function *, 8> ToBeDeletedFunctions;
  SmallPtrSet<BasicBlock *, 8> ToBeDeletedBlocks;
  SmallPtrSet<Instruction *, 8> ToBeDeletedInsts;
  ///}
};

/// An interface to query the internal state of an abstract attribute.
///
/// The abstract state is a minimal interface that allows the Attributor to
/// communicate with the abstract attributes about their internal state without
/// enforcing or exposing implementation details, e.g., the (existence of an)
/// underlying lattice.
///
/// It is sufficient to be able to query if a state is (1) valid or invalid, (2)
/// at a fixpoint, and to indicate to the state that (3) an optimistic fixpoint
/// was reached or (4) a pessimistic fixpoint was enforced.
///
/// All methods need to be implemented by the subclass. For the common use case,
/// a single boolean state or a bit-encoded state, the BooleanState and
/// {Inc,Dec,Bit}IntegerState classes are already provided. An abstract
/// attribute can inherit from them to get the abstract state interface and
/// additional methods to directly modify the state based if needed. See the
/// class comments for help.
struct AbstractState {
  virtual ~AbstractState() {}

  /// Return if this abstract state is in a valid state. If false, no
  /// information provided should be used.
  virtual bool isValidState() const = 0;

  /// Return if this abstract state is fixed, thus does not need to be updated
  /// if information changes as it cannot change itself.
  virtual bool isAtFixpoint() const = 0;

  /// Indicate that the abstract state should converge to the optimistic state.
  ///
  /// This will usually make the optimistically assumed state the known to be
  /// true state.
  ///
  /// \returns ChangeStatus::UNCHANGED as the assumed value should not change.
  virtual ChangeStatus indicateOptimisticFixpoint() = 0;

  /// Indicate that the abstract state should converge to the pessimistic state.
  ///
  /// This will usually revert the optimistically assumed state to the known to
  /// be true state.
  ///
  /// \returns ChangeStatus::CHANGED as the assumed value may change.
  virtual ChangeStatus indicatePessimisticFixpoint() = 0;
};

/// Simple state with integers encoding.
///
/// The interface ensures that the assumed bits are always a subset of the known
/// bits. Users can only add known bits and, except through adding known bits,
/// they can only remove assumed bits. This should guarantee monotoniticy and
/// thereby the existence of a fixpoint (if used corretly). The fixpoint is
/// reached when the assumed and known state/bits are equal. Users can
/// force/inidicate a fixpoint. If an optimistic one is indicated, the known
/// state will catch up with the assumed one, for a pessimistic fixpoint it is
/// the other way around.
template <typename base_ty, base_ty BestState, base_ty WorstState>
struct IntegerStateBase : public AbstractState {
  using base_t = base_ty;

  /// Return the best possible representable state.
  static constexpr base_t getBestState() { return BestState; }

  /// Return the worst possible representable state.
  static constexpr base_t getWorstState() { return WorstState; }

  /// See AbstractState::isValidState()
  /// NOTE: For now we simply pretend that the worst possible state is invalid.
  bool isValidState() const override { return Assumed != getWorstState(); }

  /// See AbstractState::isAtFixpoint()
  bool isAtFixpoint() const override { return Assumed == Known; }

  /// See AbstractState::indicateOptimisticFixpoint(...)
  ChangeStatus indicateOptimisticFixpoint() override {
    Known = Assumed;
    return ChangeStatus::UNCHANGED;
  }

  /// See AbstractState::indicatePessimisticFixpoint(...)
  ChangeStatus indicatePessimisticFixpoint() override {
    Assumed = Known;
    return ChangeStatus::CHANGED;
  }

  /// Return the known state encoding
  base_t getKnown() const { return Known; }

  /// Return the assumed state encoding.
  base_t getAssumed() const { return Assumed; }

  /// Equality for IntegerStateBase.
  bool
  operator==(const IntegerStateBase<base_t, BestState, WorstState> &R) const {
    return this->getAssumed() == R.getAssumed() &&
           this->getKnown() == R.getKnown();
  }

  /// Inequality for IntegerStateBase.
  bool
  operator!=(const IntegerStateBase<base_t, BestState, WorstState> &R) const {
    return !(*this == R);
  }

  /// "Clamp" this state with \p R. The result is subtype dependent but it is
  /// intended that only information assumed in both states will be assumed in
  /// this one afterwards.
  void operator^=(const IntegerStateBase<base_t, BestState, WorstState> &R) {
    handleNewAssumedValue(R.getAssumed());
  }

  void operator|=(const IntegerStateBase<base_t, BestState, WorstState> &R) {
    joinOR(R.getAssumed(), R.getKnown());
  }

  void operator&=(const IntegerStateBase<base_t, BestState, WorstState> &R) {
    joinAND(R.getAssumed(), R.getKnown());
  }

protected:
  /// Handle a new assumed value \p Value. Subtype dependent.
  virtual void handleNewAssumedValue(base_t Value) = 0;

  /// Handle a new known value \p Value. Subtype dependent.
  virtual void handleNewKnownValue(base_t Value) = 0;

  /// Handle a  value \p Value. Subtype dependent.
  virtual void joinOR(base_t AssumedValue, base_t KnownValue) = 0;

  /// Handle a new assumed value \p Value. Subtype dependent.
  virtual void joinAND(base_t AssumedValue, base_t KnownValue) = 0;

  /// The known state encoding in an integer of type base_t.
  base_t Known = getWorstState();

  /// The assumed state encoding in an integer of type base_t.
  base_t Assumed = getBestState();
};

/// Specialization of the integer state for a bit-wise encoding.
template <typename base_ty = uint32_t, base_ty BestState = ~base_ty(0),
          base_ty WorstState = 0>
struct BitIntegerState
    : public IntegerStateBase<base_ty, BestState, WorstState> {
  using base_t = base_ty;

  /// Return true if the bits set in \p BitsEncoding are "known bits".
  bool isKnown(base_t BitsEncoding) const {
    return (this->Known & BitsEncoding) == BitsEncoding;
  }

  /// Return true if the bits set in \p BitsEncoding are "assumed bits".
  bool isAssumed(base_t BitsEncoding) const {
    return (this->Assumed & BitsEncoding) == BitsEncoding;
  }

  /// Add the bits in \p BitsEncoding to the "known bits".
  BitIntegerState &addKnownBits(base_t Bits) {
    // Make sure we never miss any "known bits".
    this->Assumed |= Bits;
    this->Known |= Bits;
    return *this;
  }

  /// Remove the bits in \p BitsEncoding from the "assumed bits" if not known.
  BitIntegerState &removeAssumedBits(base_t BitsEncoding) {
    return intersectAssumedBits(~BitsEncoding);
  }

  /// Remove the bits in \p BitsEncoding from the "known bits".
  BitIntegerState &removeKnownBits(base_t BitsEncoding) {
    this->Known = (this->Known & ~BitsEncoding);
    return *this;
  }

  /// Keep only "assumed bits" also set in \p BitsEncoding but all known ones.
  BitIntegerState &intersectAssumedBits(base_t BitsEncoding) {
    // Make sure we never loose any "known bits".
    this->Assumed = (this->Assumed & BitsEncoding) | this->Known;
    return *this;
  }

private:
  void handleNewAssumedValue(base_t Value) override {
    intersectAssumedBits(Value);
  }
  void handleNewKnownValue(base_t Value) override { addKnownBits(Value); }
  void joinOR(base_t AssumedValue, base_t KnownValue) override {
    this->Known |= KnownValue;
    this->Assumed |= AssumedValue;
  }
  void joinAND(base_t AssumedValue, base_t KnownValue) override {
    this->Known &= KnownValue;
    this->Assumed &= AssumedValue;
  }
};

/// Specialization of the integer state for an increasing value, hence ~0u is
/// the best state and 0 the worst.
template <typename base_ty = uint32_t, base_ty BestState = ~base_ty(0),
          base_ty WorstState = 0>
struct IncIntegerState
    : public IntegerStateBase<base_ty, BestState, WorstState> {
  using base_t = base_ty;

  /// Take minimum of assumed and \p Value.
  IncIntegerState &takeAssumedMinimum(base_t Value) {
    // Make sure we never loose "known value".
    this->Assumed = std::max(std::min(this->Assumed, Value), this->Known);
    return *this;
  }

  /// Take maximum of known and \p Value.
  IncIntegerState &takeKnownMaximum(base_t Value) {
    // Make sure we never loose "known value".
    this->Assumed = std::max(Value, this->Assumed);
    this->Known = std::max(Value, this->Known);
    return *this;
  }

private:
  void handleNewAssumedValue(base_t Value) override {
    takeAssumedMinimum(Value);
  }
  void handleNewKnownValue(base_t Value) override { takeKnownMaximum(Value); }
  void joinOR(base_t AssumedValue, base_t KnownValue) override {
    this->Known = std::max(this->Known, KnownValue);
    this->Assumed = std::max(this->Assumed, AssumedValue);
  }
  void joinAND(base_t AssumedValue, base_t KnownValue) override {
    this->Known = std::min(this->Known, KnownValue);
    this->Assumed = std::min(this->Assumed, AssumedValue);
  }
};

/// Specialization of the integer state for a decreasing value, hence 0 is the
/// best state and ~0u the worst.
template <typename base_ty = uint32_t>
struct DecIntegerState : public IntegerStateBase<base_ty, 0, ~base_ty(0)> {
  using base_t = base_ty;

  /// Take maximum of assumed and \p Value.
  DecIntegerState &takeAssumedMaximum(base_t Value) {
    // Make sure we never loose "known value".
    this->Assumed = std::min(std::max(this->Assumed, Value), this->Known);
    return *this;
  }

  /// Take minimum of known and \p Value.
  DecIntegerState &takeKnownMinimum(base_t Value) {
    // Make sure we never loose "known value".
    this->Assumed = std::min(Value, this->Assumed);
    this->Known = std::min(Value, this->Known);
    return *this;
  }

private:
  void handleNewAssumedValue(base_t Value) override {
    takeAssumedMaximum(Value);
  }
  void handleNewKnownValue(base_t Value) override { takeKnownMinimum(Value); }
  void joinOR(base_t AssumedValue, base_t KnownValue) override {
    this->Assumed = std::min(this->Assumed, KnownValue);
    this->Assumed = std::min(this->Assumed, AssumedValue);
  }
  void joinAND(base_t AssumedValue, base_t KnownValue) override {
    this->Assumed = std::max(this->Assumed, KnownValue);
    this->Assumed = std::max(this->Assumed, AssumedValue);
  }
};

/// Simple wrapper for a single bit (boolean) state.
struct BooleanState : public IntegerStateBase<bool, 1, 0> {
  using base_t = IntegerStateBase::base_t;

  /// Set the assumed value to \p Value but never below the known one.
  void setAssumed(bool Value) { Assumed &= (Known | Value); }

  /// Set the known and asssumed value to \p Value.
  void setKnown(bool Value) {
    Known |= Value;
    Assumed |= Value;
  }

  /// Return true if the state is assumed to hold.
  bool isAssumed() const { return getAssumed(); }

  /// Return true if the state is known to hold.
  bool isKnown() const { return getKnown(); }

private:
  void handleNewAssumedValue(base_t Value) override {
    if (!Value)
      Assumed = Known;
  }
  void handleNewKnownValue(base_t Value) override {
    if (Value)
      Known = (Assumed = Value);
  }
  void joinOR(base_t AssumedValue, base_t KnownValue) override {
    Known |= KnownValue;
    Assumed |= AssumedValue;
  }
  void joinAND(base_t AssumedValue, base_t KnownValue) override {
    Known &= KnownValue;
    Assumed &= AssumedValue;
  }
};

/// Helper struct necessary as the modular build fails if the virtual method
/// IRAttribute::manifest is defined in the Attributor.cpp.
struct IRAttributeManifest {
  static ChangeStatus manifestAttrs(Attributor &A, const IRPosition &IRP,
                                    const ArrayRef<Attribute> &DeducedAttrs);
};

/// Helper to tie a abstract state implementation to an abstract attribute.
template <typename StateTy, typename Base>
struct StateWrapper : public StateTy, public Base {
  /// Provide static access to the type of the state.
  using StateType = StateTy;

  /// See AbstractAttribute::getState(...).
  StateType &getState() override { return *this; }

  /// See AbstractAttribute::getState(...).
  const AbstractState &getState() const override { return *this; }
};

/// Helper class that provides common functionality to manifest IR attributes.
template <Attribute::AttrKind AK, typename Base>
struct IRAttribute : public IRPosition, public Base {
  IRAttribute(const IRPosition &IRP) : IRPosition(IRP) {}
  ~IRAttribute() {}

  /// See AbstractAttribute::initialize(...).
  virtual void initialize(Attributor &A) override {
    const IRPosition &IRP = this->getIRPosition();
    if (isa<UndefValue>(IRP.getAssociatedValue()) || hasAttr(getAttrKind())) {
      this->getState().indicateOptimisticFixpoint();
      return;
    }

    bool IsFnInterface = IRP.isFnInterfaceKind();
    const Function *FnScope = IRP.getAnchorScope();
    // TODO: Not all attributes require an exact definition. Find a way to
    //       enable deduction for some but not all attributes in case the
    //       definition might be changed at runtime, see also
    //       http://lists.llvm.org/pipermail/llvm-dev/2018-February/121275.html.
    // TODO: We could always determine abstract attributes and if sufficient
    //       information was found we could duplicate the functions that do not
    //       have an exact definition.
    if (IsFnInterface && (!FnScope || !FnScope->hasExactDefinition()))
      this->getState().indicatePessimisticFixpoint();
  }

  /// See AbstractAttribute::manifest(...).
  ChangeStatus manifest(Attributor &A) override {
    if (isa<UndefValue>(getIRPosition().getAssociatedValue()))
      return ChangeStatus::UNCHANGED;
    SmallVector<Attribute, 4> DeducedAttrs;
    getDeducedAttributes(getAnchorValue().getContext(), DeducedAttrs);
    return IRAttributeManifest::manifestAttrs(A, getIRPosition(), DeducedAttrs);
  }

  /// Return the kind that identifies the abstract attribute implementation.
  Attribute::AttrKind getAttrKind() const { return AK; }

  /// Return the deduced attributes in \p Attrs.
  virtual void getDeducedAttributes(LLVMContext &Ctx,
                                    SmallVectorImpl<Attribute> &Attrs) const {
    Attrs.emplace_back(Attribute::get(Ctx, getAttrKind()));
  }

  /// Return an IR position, see struct IRPosition.
  const IRPosition &getIRPosition() const override { return *this; }
};

/// Base struct for all "concrete attribute" deductions.
///
/// The abstract attribute is a minimal interface that allows the Attributor to
/// orchestrate the abstract/fixpoint analysis. The design allows to hide away
/// implementation choices made for the subclasses but also to structure their
/// implementation and simplify the use of other abstract attributes in-flight.
///
/// To allow easy creation of new attributes, most methods have default
/// implementations. The ones that do not are generally straight forward, except
/// `AbstractAttribute::updateImpl` which is the location of most reasoning
/// associated with the abstract attribute. The update is invoked by the
/// Attributor in case the situation used to justify the current optimistic
/// state might have changed. The Attributor determines this automatically
/// by monitoring the `Attributor::getAAFor` calls made by abstract attributes.
///
/// The `updateImpl` method should inspect the IR and other abstract attributes
/// in-flight to justify the best possible (=optimistic) state. The actual
/// implementation is, similar to the underlying abstract state encoding, not
/// exposed. In the most common case, the `updateImpl` will go through a list of
/// reasons why its optimistic state is valid given the current information. If
/// any combination of them holds and is sufficient to justify the current
/// optimistic state, the method shall return UNCHAGED. If not, the optimistic
/// state is adjusted to the situation and the method shall return CHANGED.
///
/// If the manifestation of the "concrete attribute" deduced by the subclass
/// differs from the "default" behavior, which is a (set of) LLVM-IR
/// attribute(s) for an argument, call site argument, function return value, or
/// function, the `AbstractAttribute::manifest` method should be overloaded.
///
/// NOTE: If the state obtained via getState() is INVALID, thus if
///       AbstractAttribute::getState().isValidState() returns false, no
///       information provided by the methods of this class should be used.
/// NOTE: The Attributor currently has certain limitations to what we can do.
///       As a general rule of thumb, "concrete" abstract attributes should *for
///       now* only perform "backward" information propagation. That means
///       optimistic information obtained through abstract attributes should
///       only be used at positions that precede the origin of the information
///       with regards to the program flow. More practically, information can
///       *now* be propagated from instructions to their enclosing function, but
///       *not* from call sites to the called function. The mechanisms to allow
///       both directions will be added in the future.
/// NOTE: The mechanics of adding a new "concrete" abstract attribute are
///       described in the file comment.
struct AbstractAttribute {
  using StateType = AbstractState;

  /// Virtual destructor.
  virtual ~AbstractAttribute() {}

  /// Initialize the state with the information in the Attributor \p A.
  ///
  /// This function is called by the Attributor once all abstract attributes
  /// have been identified. It can and shall be used for task like:
  ///  - identify existing knowledge in the IR and use it for the "known state"
  ///  - perform any work that is not going to change over time, e.g., determine
  ///    a subset of the IR, or attributes in-flight, that have to be looked at
  ///    in the `updateImpl` method.
  virtual void initialize(Attributor &A) {}

  /// Return the internal abstract state for inspection.
  virtual StateType &getState() = 0;
  virtual const StateType &getState() const = 0;

  /// Return an IR position, see struct IRPosition.
  virtual const IRPosition &getIRPosition() const = 0;

  /// Helper functions, for debug purposes only.
  ///{
  virtual void print(raw_ostream &OS) const;
  void dump() const { print(dbgs()); }

  /// This function should return the "summarized" assumed state as string.
  virtual const std::string getAsStr() const = 0;
  ///}

  /// Allow the Attributor access to the protected methods.
  friend struct Attributor;

protected:
  /// Hook for the Attributor to trigger an update of the internal state.
  ///
  /// If this attribute is already fixed, this method will return UNCHANGED,
  /// otherwise it delegates to `AbstractAttribute::updateImpl`.
  ///
  /// \Return CHANGED if the internal state changed, otherwise UNCHANGED.
  ChangeStatus update(Attributor &A);

  /// Hook for the Attributor to trigger the manifestation of the information
  /// represented by the abstract attribute in the LLVM-IR.
  ///
  /// \Return CHANGED if the IR was altered, otherwise UNCHANGED.
  virtual ChangeStatus manifest(Attributor &A) {
    return ChangeStatus::UNCHANGED;
  }

  /// Hook to enable custom statistic tracking, called after manifest that
  /// resulted in a change if statistics are enabled.
  ///
  /// We require subclasses to provide an implementation so we remember to
  /// add statistics for them.
  virtual void trackStatistics() const = 0;

  /// The actual update/transfer function which has to be implemented by the
  /// derived classes.
  ///
  /// If it is called, the environment has changed and we have to determine if
  /// the current information is still valid or adjust it otherwise.
  ///
  /// \Return CHANGED if the internal state changed, otherwise UNCHANGED.
  virtual ChangeStatus updateImpl(Attributor &A) = 0;
};

/// Forward declarations of output streams for debug purposes.
///
///{
raw_ostream &operator<<(raw_ostream &OS, const AbstractAttribute &AA);
raw_ostream &operator<<(raw_ostream &OS, ChangeStatus S);
raw_ostream &operator<<(raw_ostream &OS, IRPosition::Kind);
raw_ostream &operator<<(raw_ostream &OS, const IRPosition &);
raw_ostream &operator<<(raw_ostream &OS, const AbstractState &State);
template <typename base_ty, base_ty BestState, base_ty WorstState>
raw_ostream &
operator<<(raw_ostream &OS,
           const IntegerStateBase<base_ty, BestState, WorstState> &State);
///}

struct AttributorPass : public PassInfoMixin<AttributorPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

Pass *createAttributorLegacyPass();

/// ----------------------------------------------------------------------------
///                       Abstract Attribute Classes
/// ----------------------------------------------------------------------------

/// An abstract attribute for the returned values of a function.
struct AAReturnedValues
    : public IRAttribute<Attribute::Returned, AbstractAttribute> {
  AAReturnedValues(const IRPosition &IRP) : IRAttribute(IRP) {}

  /// Return an assumed unique return value if a single candidate is found. If
  /// there cannot be one, return a nullptr. If it is not clear yet, return the
  /// Optional::NoneType.
  Optional<Value *> getAssumedUniqueReturnValue(Attributor &A) const;

  /// Check \p Pred on all returned values.
  ///
  /// This method will evaluate \p Pred on returned values and return
  /// true if (1) all returned values are known, and (2) \p Pred returned true
  /// for all returned values.
  ///
  /// Note: Unlike the Attributor::checkForAllReturnedValuesAndReturnInsts
  /// method, this one will not filter dead return instructions.
  virtual bool checkForAllReturnedValuesAndReturnInsts(
      const function_ref<bool(Value &, const SmallSetVector<ReturnInst *, 4> &)>
          &Pred) const = 0;

  using iterator =
      MapVector<Value *, SmallSetVector<ReturnInst *, 4>>::iterator;
  using const_iterator =
      MapVector<Value *, SmallSetVector<ReturnInst *, 4>>::const_iterator;
  virtual llvm::iterator_range<iterator> returned_values() = 0;
  virtual llvm::iterator_range<const_iterator> returned_values() const = 0;

  virtual size_t getNumReturnValues() const = 0;
  virtual const SmallSetVector<CallBase *, 4> &getUnresolvedCalls() const = 0;

  /// Create an abstract attribute view for the position \p IRP.
  static AAReturnedValues &createForPosition(const IRPosition &IRP,
                                             Attributor &A);

  /// Unique ID (due to the unique address)
  static const char ID;
};

struct AANoUnwind
    : public IRAttribute<Attribute::NoUnwind,
                         StateWrapper<BooleanState, AbstractAttribute>> {
  AANoUnwind(const IRPosition &IRP) : IRAttribute(IRP) {}

  /// Returns true if nounwind is assumed.
  bool isAssumedNoUnwind() const { return getAssumed(); }

  /// Returns true if nounwind is known.
  bool isKnownNoUnwind() const { return getKnown(); }

  /// Create an abstract attribute view for the position \p IRP.
  static AANoUnwind &createForPosition(const IRPosition &IRP, Attributor &A);

  /// Unique ID (due to the unique address)
  static const char ID;
};

struct AANoSync
    : public IRAttribute<Attribute::NoSync,
                         StateWrapper<BooleanState, AbstractAttribute>> {
  AANoSync(const IRPosition &IRP) : IRAttribute(IRP) {}

  /// Returns true if "nosync" is assumed.
  bool isAssumedNoSync() const { return getAssumed(); }

  /// Returns true if "nosync" is known.
  bool isKnownNoSync() const { return getKnown(); }

  /// Create an abstract attribute view for the position \p IRP.
  static AANoSync &createForPosition(const IRPosition &IRP, Attributor &A);

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An abstract interface for all nonnull attributes.
struct AANonNull
    : public IRAttribute<Attribute::NonNull,
                         StateWrapper<BooleanState, AbstractAttribute>> {
  AANonNull(const IRPosition &IRP) : IRAttribute(IRP) {}

  /// Return true if we assume that the underlying value is nonnull.
  bool isAssumedNonNull() const { return getAssumed(); }

  /// Return true if we know that underlying value is nonnull.
  bool isKnownNonNull() const { return getKnown(); }

  /// Create an abstract attribute view for the position \p IRP.
  static AANonNull &createForPosition(const IRPosition &IRP, Attributor &A);

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An abstract attribute for norecurse.
struct AANoRecurse
    : public IRAttribute<Attribute::NoRecurse,
                         StateWrapper<BooleanState, AbstractAttribute>> {
  AANoRecurse(const IRPosition &IRP) : IRAttribute(IRP) {}

  /// Return true if "norecurse" is assumed.
  bool isAssumedNoRecurse() const { return getAssumed(); }

  /// Return true if "norecurse" is known.
  bool isKnownNoRecurse() const { return getKnown(); }

  /// Create an abstract attribute view for the position \p IRP.
  static AANoRecurse &createForPosition(const IRPosition &IRP, Attributor &A);

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An abstract attribute for willreturn.
struct AAWillReturn
    : public IRAttribute<Attribute::WillReturn,
                         StateWrapper<BooleanState, AbstractAttribute>> {
  AAWillReturn(const IRPosition &IRP) : IRAttribute(IRP) {}

  /// Return true if "willreturn" is assumed.
  bool isAssumedWillReturn() const { return getAssumed(); }

  /// Return true if "willreturn" is known.
  bool isKnownWillReturn() const { return getKnown(); }

  /// Create an abstract attribute view for the position \p IRP.
  static AAWillReturn &createForPosition(const IRPosition &IRP, Attributor &A);

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An abstract interface for all noalias attributes.
struct AANoAlias
    : public IRAttribute<Attribute::NoAlias,
                         StateWrapper<BooleanState, AbstractAttribute>> {
  AANoAlias(const IRPosition &IRP) : IRAttribute(IRP) {}

  /// Return true if we assume that the underlying value is alias.
  bool isAssumedNoAlias() const { return getAssumed(); }

  /// Return true if we know that underlying value is noalias.
  bool isKnownNoAlias() const { return getKnown(); }

  /// Create an abstract attribute view for the position \p IRP.
  static AANoAlias &createForPosition(const IRPosition &IRP, Attributor &A);

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An AbstractAttribute for nofree.
struct AANoFree
    : public IRAttribute<Attribute::NoFree,
                         StateWrapper<BooleanState, AbstractAttribute>> {
  AANoFree(const IRPosition &IRP) : IRAttribute(IRP) {}

  /// Return true if "nofree" is assumed.
  bool isAssumedNoFree() const { return getAssumed(); }

  /// Return true if "nofree" is known.
  bool isKnownNoFree() const { return getKnown(); }

  /// Create an abstract attribute view for the position \p IRP.
  static AANoFree &createForPosition(const IRPosition &IRP, Attributor &A);

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An AbstractAttribute for noreturn.
struct AANoReturn
    : public IRAttribute<Attribute::NoReturn,
                         StateWrapper<BooleanState, AbstractAttribute>> {
  AANoReturn(const IRPosition &IRP) : IRAttribute(IRP) {}

  /// Return true if the underlying object is assumed to never return.
  bool isAssumedNoReturn() const { return getAssumed(); }

  /// Return true if the underlying object is known to never return.
  bool isKnownNoReturn() const { return getKnown(); }

  /// Create an abstract attribute view for the position \p IRP.
  static AANoReturn &createForPosition(const IRPosition &IRP, Attributor &A);

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An abstract interface for liveness abstract attribute.
struct AAIsDead : public StateWrapper<BooleanState, AbstractAttribute>,
                  public IRPosition {
  AAIsDead(const IRPosition &IRP) : IRPosition(IRP) {}

  /// Returns true if the underlying value is assumed dead.
  virtual bool isAssumedDead() const = 0;

  /// Returns true if \p BB is assumed dead.
  virtual bool isAssumedDead(const BasicBlock *BB) const = 0;

  /// Returns true if \p BB is known dead.
  virtual bool isKnownDead(const BasicBlock *BB) const = 0;

  /// Returns true if \p I is assumed dead.
  virtual bool isAssumedDead(const Instruction *I) const = 0;

  /// Returns true if \p I is known dead.
  virtual bool isKnownDead(const Instruction *I) const = 0;

  /// This method is used to check if at least one instruction in a collection
  /// of instructions is live.
  template <typename T> bool isLiveInstSet(T begin, T end) const {
    for (const auto &I : llvm::make_range(begin, end)) {
      assert(I->getFunction() == getIRPosition().getAssociatedFunction() &&
             "Instruction must be in the same anchor scope function.");

      if (!isAssumedDead(I))
        return true;
    }

    return false;
  }

  /// Return an IR position, see struct IRPosition.
  const IRPosition &getIRPosition() const override { return *this; }

  /// Create an abstract attribute view for the position \p IRP.
  static AAIsDead &createForPosition(const IRPosition &IRP, Attributor &A);

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// State for dereferenceable attribute
struct DerefState : AbstractState {

  /// State representing for dereferenceable bytes.
  IncIntegerState<> DerefBytesState;

  /// State representing that whether the value is globaly dereferenceable.
  BooleanState GlobalState;

  /// See AbstractState::isValidState()
  bool isValidState() const override { return DerefBytesState.isValidState(); }

  /// See AbstractState::isAtFixpoint()
  bool isAtFixpoint() const override {
    return !isValidState() ||
           (DerefBytesState.isAtFixpoint() && GlobalState.isAtFixpoint());
  }

  /// See AbstractState::indicateOptimisticFixpoint(...)
  ChangeStatus indicateOptimisticFixpoint() override {
    DerefBytesState.indicateOptimisticFixpoint();
    GlobalState.indicateOptimisticFixpoint();
    return ChangeStatus::UNCHANGED;
  }

  /// See AbstractState::indicatePessimisticFixpoint(...)
  ChangeStatus indicatePessimisticFixpoint() override {
    DerefBytesState.indicatePessimisticFixpoint();
    GlobalState.indicatePessimisticFixpoint();
    return ChangeStatus::CHANGED;
  }

  /// Update known dereferenceable bytes.
  void takeKnownDerefBytesMaximum(uint64_t Bytes) {
    DerefBytesState.takeKnownMaximum(Bytes);
  }

  /// Update assumed dereferenceable bytes.
  void takeAssumedDerefBytesMinimum(uint64_t Bytes) {
    DerefBytesState.takeAssumedMinimum(Bytes);
  }

  /// Equality for DerefState.
  bool operator==(const DerefState &R) {
    return this->DerefBytesState == R.DerefBytesState &&
           this->GlobalState == R.GlobalState;
  }

  /// Inequality for DerefState.
  bool operator!=(const DerefState &R) { return !(*this == R); }

  /// See IntegerStateBase::operator^=
  DerefState operator^=(const DerefState &R) {
    DerefBytesState ^= R.DerefBytesState;
    GlobalState ^= R.GlobalState;
    return *this;
  }

  /// See IntegerStateBase::operator&=
  DerefState operator&=(const DerefState &R) {
    DerefBytesState &= R.DerefBytesState;
    GlobalState &= R.GlobalState;
    return *this;
  }

  /// See IntegerStateBase::operator|=
  DerefState operator|=(const DerefState &R) {
    DerefBytesState |= R.DerefBytesState;
    GlobalState |= R.GlobalState;
    return *this;
  }

protected:
  const AANonNull *NonNullAA = nullptr;
};

/// An abstract interface for all dereferenceable attribute.
struct AADereferenceable
    : public IRAttribute<Attribute::Dereferenceable,
                         StateWrapper<DerefState, AbstractAttribute>> {
  AADereferenceable(const IRPosition &IRP) : IRAttribute(IRP) {}

  /// Return true if we assume that the underlying value is nonnull.
  bool isAssumedNonNull() const {
    return NonNullAA && NonNullAA->isAssumedNonNull();
  }

  /// Return true if we know that the underlying value is nonnull.
  bool isKnownNonNull() const {
    return NonNullAA && NonNullAA->isKnownNonNull();
  }

  /// Return true if we assume that underlying value is
  /// dereferenceable(_or_null) globally.
  bool isAssumedGlobal() const { return GlobalState.getAssumed(); }

  /// Return true if we know that underlying value is
  /// dereferenceable(_or_null) globally.
  bool isKnownGlobal() const { return GlobalState.getKnown(); }

  /// Return assumed dereferenceable bytes.
  uint32_t getAssumedDereferenceableBytes() const {
    return DerefBytesState.getAssumed();
  }

  /// Return known dereferenceable bytes.
  uint32_t getKnownDereferenceableBytes() const {
    return DerefBytesState.getKnown();
  }

  /// Create an abstract attribute view for the position \p IRP.
  static AADereferenceable &createForPosition(const IRPosition &IRP,
                                              Attributor &A);

  /// Unique ID (due to the unique address)
  static const char ID;
};

using AAAlignmentStateType =
    IncIntegerState<uint32_t, /* maximal alignment */ 1U << 29, 0>;
/// An abstract interface for all align attributes.
struct AAAlign : public IRAttribute<
                     Attribute::Alignment,
                     StateWrapper<AAAlignmentStateType, AbstractAttribute>> {
  AAAlign(const IRPosition &IRP) : IRAttribute(IRP) {}

  /// Return assumed alignment.
  unsigned getAssumedAlign() const { return getAssumed(); }

  /// Return known alignemnt.
  unsigned getKnownAlign() const { return getKnown(); }

  /// Create an abstract attribute view for the position \p IRP.
  static AAAlign &createForPosition(const IRPosition &IRP, Attributor &A);

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An abstract interface for all nocapture attributes.
struct AANoCapture
    : public IRAttribute<
          Attribute::NoCapture,
          StateWrapper<BitIntegerState<uint16_t, 7, 0>, AbstractAttribute>> {
  AANoCapture(const IRPosition &IRP) : IRAttribute(IRP) {}

  /// State encoding bits. A set bit in the state means the property holds.
  /// NO_CAPTURE is the best possible state, 0 the worst possible state.
  enum {
    NOT_CAPTURED_IN_MEM = 1 << 0,
    NOT_CAPTURED_IN_INT = 1 << 1,
    NOT_CAPTURED_IN_RET = 1 << 2,

    /// If we do not capture the value in memory or through integers we can only
    /// communicate it back as a derived pointer.
    NO_CAPTURE_MAYBE_RETURNED = NOT_CAPTURED_IN_MEM | NOT_CAPTURED_IN_INT,

    /// If we do not capture the value in memory, through integers, or as a
    /// derived pointer we know it is not captured.
    NO_CAPTURE =
        NOT_CAPTURED_IN_MEM | NOT_CAPTURED_IN_INT | NOT_CAPTURED_IN_RET,
  };

  /// Return true if we know that the underlying value is not captured in its
  /// respective scope.
  bool isKnownNoCapture() const { return isKnown(NO_CAPTURE); }

  /// Return true if we assume that the underlying value is not captured in its
  /// respective scope.
  bool isAssumedNoCapture() const { return isAssumed(NO_CAPTURE); }

  /// Return true if we know that the underlying value is not captured in its
  /// respective scope but we allow it to escape through a "return".
  bool isKnownNoCaptureMaybeReturned() const {
    return isKnown(NO_CAPTURE_MAYBE_RETURNED);
  }

  /// Return true if we assume that the underlying value is not captured in its
  /// respective scope but we allow it to escape through a "return".
  bool isAssumedNoCaptureMaybeReturned() const {
    return isAssumed(NO_CAPTURE_MAYBE_RETURNED);
  }

  /// Create an abstract attribute view for the position \p IRP.
  static AANoCapture &createForPosition(const IRPosition &IRP, Attributor &A);

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An abstract interface for value simplify abstract attribute.
struct AAValueSimplify : public StateWrapper<BooleanState, AbstractAttribute>,
                         public IRPosition {
  AAValueSimplify(const IRPosition &IRP) : IRPosition(IRP) {}

  /// Return an IR position, see struct IRPosition.
  const IRPosition &getIRPosition() const { return *this; }

  /// Return an assumed simplified value if a single candidate is found. If
  /// there cannot be one, return original value. If it is not clear yet, return
  /// the Optional::NoneType.
  virtual Optional<Value *> getAssumedSimplifiedValue(Attributor &A) const = 0;

  /// Create an abstract attribute view for the position \p IRP.
  static AAValueSimplify &createForPosition(const IRPosition &IRP,
                                            Attributor &A);

  /// Unique ID (due to the unique address)
  static const char ID;
};

struct AAHeapToStack : public StateWrapper<BooleanState, AbstractAttribute>,
                       public IRPosition {
  AAHeapToStack(const IRPosition &IRP) : IRPosition(IRP) {}

  /// Returns true if HeapToStack conversion is assumed to be possible.
  bool isAssumedHeapToStack() const { return getAssumed(); }

  /// Returns true if HeapToStack conversion is known to be possible.
  bool isKnownHeapToStack() const { return getKnown(); }

  /// Return an IR position, see struct IRPosition.
  const IRPosition &getIRPosition() const { return *this; }

  /// Create an abstract attribute view for the position \p IRP.
  static AAHeapToStack &createForPosition(const IRPosition &IRP, Attributor &A);

  /// Unique ID (due to the unique address)
  static const char ID;
};

/// An abstract interface for all memory related attributes.
struct AAMemoryBehavior
    : public IRAttribute<
          Attribute::ReadNone,
          StateWrapper<BitIntegerState<uint8_t, 3>, AbstractAttribute>> {
  AAMemoryBehavior(const IRPosition &IRP) : IRAttribute(IRP) {}

  /// State encoding bits. A set bit in the state means the property holds.
  /// BEST_STATE is the best possible state, 0 the worst possible state.
  enum {
    NO_READS = 1 << 0,
    NO_WRITES = 1 << 1,
    NO_ACCESSES = NO_READS | NO_WRITES,

    BEST_STATE = NO_ACCESSES,
  };

  /// Return true if we know that the underlying value is not read or accessed
  /// in its respective scope.
  bool isKnownReadNone() const { return isKnown(NO_ACCESSES); }

  /// Return true if we assume that the underlying value is not read or accessed
  /// in its respective scope.
  bool isAssumedReadNone() const { return isAssumed(NO_ACCESSES); }

  /// Return true if we know that the underlying value is not accessed
  /// (=written) in its respective scope.
  bool isKnownReadOnly() const { return isKnown(NO_WRITES); }

  /// Return true if we assume that the underlying value is not accessed
  /// (=written) in its respective scope.
  bool isAssumedReadOnly() const { return isAssumed(NO_WRITES); }

  /// Return true if we know that the underlying value is not read in its
  /// respective scope.
  bool isKnownWriteOnly() const { return isKnown(NO_READS); }

  /// Return true if we assume that the underlying value is not read in its
  /// respective scope.
  bool isAssumedWriteOnly() const { return isAssumed(NO_READS); }

  /// Create an abstract attribute view for the position \p IRP.
  static AAMemoryBehavior &createForPosition(const IRPosition &IRP,
                                             Attributor &A);

  /// Unique ID (due to the unique address)
  static const char ID;
};

} // end namespace llvm

#endif // LLVM_TRANSFORMS_IPO_FUNCTIONATTRS_H

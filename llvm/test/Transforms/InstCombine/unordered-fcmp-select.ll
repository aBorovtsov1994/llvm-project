; NOTE: Assertions have been autogenerated by utils/update_test_checks.py
; RUN: opt -S -instcombine < %s | FileCheck %s

define float @select_max_ugt(float %a, float %b) {
; CHECK-LABEL: @select_max_ugt(
; CHECK-NEXT:    [[CMP_INV:%.*]] = fcmp ole float [[A:%.*]], [[B:%.*]]
; CHECK-NEXT:    [[SEL:%.*]] = select i1 [[CMP_INV]], float [[B]], float [[A]]
; CHECK-NEXT:    ret float [[SEL]]
;
  %cmp = fcmp ugt float %a, %b
  %sel = select i1 %cmp, float %a, float %b
  ret float %sel
}

define float @select_max_uge(float %a, float %b) {
; CHECK-LABEL: @select_max_uge(
; CHECK-NEXT:    [[CMP_INV:%.*]] = fcmp olt float [[A:%.*]], [[B:%.*]]
; CHECK-NEXT:    [[SEL:%.*]] = select i1 [[CMP_INV]], float [[B]], float [[A]]
; CHECK-NEXT:    ret float [[SEL]]
;
  %cmp = fcmp uge float %a, %b
  %sel = select i1 %cmp, float %a, float %b
  ret float %sel
}

define float @select_min_ugt(float %a, float %b) {
; CHECK-LABEL: @select_min_ugt(
; CHECK-NEXT:    [[CMP_INV:%.*]] = fcmp ole float [[A:%.*]], [[B:%.*]]
; CHECK-NEXT:    [[SEL:%.*]] = select i1 [[CMP_INV]], float [[A]], float [[B]]
; CHECK-NEXT:    ret float [[SEL]]
;
  %cmp = fcmp ugt float %a, %b
  %sel = select i1 %cmp, float %b, float %a
  ret float %sel
}

define float @select_min_uge(float %a, float %b) {
; CHECK-LABEL: @select_min_uge(
; CHECK-NEXT:    [[CMP_INV:%.*]] = fcmp olt float [[A:%.*]], [[B:%.*]]
; CHECK-NEXT:    [[SEL:%.*]] = select i1 [[CMP_INV]], float [[A]], float [[B]]
; CHECK-NEXT:    ret float [[SEL]]
;
  %cmp = fcmp uge float %a, %b
  %sel = select i1 %cmp, float %b, float %a
  ret float %sel
}

define float @select_max_ult(float %a, float %b) {
; CHECK-LABEL: @select_max_ult(
; CHECK-NEXT:    [[CMP_INV:%.*]] = fcmp oge float [[A:%.*]], [[B:%.*]]
; CHECK-NEXT:    [[SEL:%.*]] = select i1 [[CMP_INV]], float [[A]], float [[B]]
; CHECK-NEXT:    ret float [[SEL]]
;
  %cmp = fcmp ult float %a, %b
  %sel = select i1 %cmp, float %b, float %a
  ret float %sel
}

define float @select_max_ule(float %a, float %b) {
; CHECK-LABEL: @select_max_ule(
; CHECK-NEXT:    [[CMP_INV:%.*]] = fcmp ogt float [[A:%.*]], [[B:%.*]]
; CHECK-NEXT:    [[SEL:%.*]] = select i1 [[CMP_INV]], float [[A]], float [[B]]
; CHECK-NEXT:    ret float [[SEL]]
;
  %cmp = fcmp ule float %a, %b
  %sel = select i1 %cmp, float %b, float %a
  ret float %sel
}

define float @select_min_ult(float %a, float %b) {
; CHECK-LABEL: @select_min_ult(
; CHECK-NEXT:    [[CMP_INV:%.*]] = fcmp oge float [[A:%.*]], [[B:%.*]]
; CHECK-NEXT:    [[SEL:%.*]] = select i1 [[CMP_INV]], float [[B]], float [[A]]
; CHECK-NEXT:    ret float [[SEL]]
;
  %cmp = fcmp ult float %a, %b
  %sel = select i1 %cmp, float %a, float %b
  ret float %sel
}

define float @select_min_ule(float %a, float %b) {
; CHECK-LABEL: @select_min_ule(
; CHECK-NEXT:    [[CMP_INV:%.*]] = fcmp ogt float [[A:%.*]], [[B:%.*]]
; CHECK-NEXT:    [[SEL:%.*]] = select i1 [[CMP_INV]], float [[B]], float [[A]]
; CHECK-NEXT:    ret float [[SEL]]
;
  %cmp = fcmp ule float %a, %b
  %sel = select i1 %cmp, float %a, float %b
  ret float %sel
}

define float @select_fcmp_une(float %a, float %b) {
; CHECK-LABEL: @select_fcmp_une(
; CHECK-NEXT:    [[CMP_INV:%.*]] = fcmp oeq float [[A:%.*]], [[B:%.*]]
; CHECK-NEXT:    [[SEL:%.*]] = select i1 [[CMP_INV]], float [[B]], float [[A]]
; CHECK-NEXT:    ret float [[SEL]]
;
  %cmp = fcmp une float %a, %b
  %sel = select i1 %cmp, float %a, float %b
  ret float %sel
}

define float @select_fcmp_ueq(float %a, float %b) {
; CHECK-LABEL: @select_fcmp_ueq(
; CHECK-NEXT:    [[CMP_INV:%.*]] = fcmp one float [[A:%.*]], [[B:%.*]]
; CHECK-NEXT:    [[SEL:%.*]] = select i1 [[CMP_INV]], float [[B]], float [[A]]
; CHECK-NEXT:    ret float [[SEL]]
;
  %cmp = fcmp ueq float %a, %b
  %sel = select i1 %cmp, float %a, float %b
  ret float %sel
}

declare void @foo(i1)

define float @select_max_ugt_2_use_cmp(float %a, float %b) {
; CHECK-LABEL: @select_max_ugt_2_use_cmp(
; CHECK-NEXT:    [[CMP:%.*]] = fcmp ugt float [[A:%.*]], [[B:%.*]]
; CHECK-NEXT:    call void @foo(i1 [[CMP]])
; CHECK-NEXT:    [[SEL:%.*]] = select i1 [[CMP]], float [[A]], float [[B]]
; CHECK-NEXT:    ret float [[SEL]]
;
  %cmp = fcmp ugt float %a, %b
  call void @foo(i1 %cmp)
  %sel = select i1 %cmp, float %a, float %b
  ret float %sel
}

define float @select_min_uge_2_use_cmp(float %a, float %b) {
; CHECK-LABEL: @select_min_uge_2_use_cmp(
; CHECK-NEXT:    [[CMP:%.*]] = fcmp uge float [[A:%.*]], [[B:%.*]]
; CHECK-NEXT:    call void @foo(i1 [[CMP]])
; CHECK-NEXT:    [[SEL:%.*]] = select i1 [[CMP]], float [[B]], float [[A]]
; CHECK-NEXT:    ret float [[SEL]]
;
  %cmp = fcmp uge float %a, %b
  call void @foo(i1 %cmp)
  %sel = select i1 %cmp, float %b, float %a
  ret float %sel
}

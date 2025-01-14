from __future__ import print_function
import re
import string
import subprocess
import sys
import copy

if sys.version_info[0] > 2:
  class string:
    expandtabs = str.expandtabs
else:
  import string

##### Common utilities for update_*test_checks.py

def should_add_line_to_output(input_line, prefix_set):
  # Skip any blank comment lines in the IR.
  if input_line.strip() == ';':
    return False
  # Skip any blank lines in the IR.
  #if input_line.strip() == '':
  #  return False
  # And skip any CHECK lines. We're building our own.
  m = CHECK_RE.match(input_line)
  if m and m.group(1) in prefix_set:
    return False

  return True

# Invoke the tool that is being tested.
def invoke_tool(exe, cmd_args, ir):
  with open(ir) as ir_file:
    # TODO Remove the str form which is used by update_test_checks.py and
    # update_llc_test_checks.py
    # The safer list form is used by update_cc_test_checks.py
    if isinstance(cmd_args, list):
      stdout = subprocess.check_output([exe] + cmd_args, stdin=ir_file)
    else:
      stdout = subprocess.check_output(exe + ' ' + cmd_args,
                                       shell=True, stdin=ir_file)
    if sys.version_info[0] > 2:
      stdout = stdout.decode()
  # Fix line endings to unix CR style.
  return stdout.replace('\r\n', '\n')

##### LLVM IR parser

RUN_LINE_RE = re.compile(r'^\s*[;#]\s*RUN:\s*(.*)$')
CHECK_PREFIX_RE = re.compile(r'--?check-prefix(?:es)?[= ](\S+)')
PREFIX_RE = re.compile('^[a-zA-Z0-9_-]+$')
CHECK_RE = re.compile(r'^\s*[;#]\s*([^:]+?)(?:-NEXT|-NOT|-DAG|-LABEL|-SAME)?:')

OPT_FUNCTION_RE = re.compile(
    r'^\s*define\s+(?:internal\s+)?[^@]*@(?P<func>[\w-]+?)\s*'
    r'(?P<args_and_sig>\((\)|(.*?[\w\.\-]+?)\))[^{]*)\{\n(?P<body>.*?)^\}$',
    flags=(re.M | re.S))

ANALYZE_FUNCTION_RE = re.compile(
    r'^\s*\'(?P<analysis>[\w\s-]+?)\'\s+for\s+function\s+\'(?P<func>[\w-]+?)\':'
    r'\s*\n(?P<body>.*)$',
    flags=(re.X | re.S))

IR_FUNCTION_RE = re.compile(r'^\s*define\s+(?:internal\s+)?[^@]*@(\w+)\s*\(')
TRIPLE_IR_RE = re.compile(r'^\s*target\s+triple\s*=\s*"([^"]+)"$')
TRIPLE_ARG_RE = re.compile(r'-mtriple[= ]([^ ]+)')
MARCH_ARG_RE = re.compile(r'-march[= ]([^ ]+)')

SCRUB_LEADING_WHITESPACE_RE = re.compile(r'^(\s+)')
SCRUB_WHITESPACE_RE = re.compile(r'(?!^(|  \w))[ \t]+', flags=re.M)
SCRUB_TRAILING_WHITESPACE_RE = re.compile(r'[ \t]+$', flags=re.M)
SCRUB_KILL_COMMENT_RE = re.compile(r'^ *#+ +kill:.*\n')
SCRUB_LOOP_COMMENT_RE = re.compile(
    r'# =>This Inner Loop Header:.*|# in Loop:.*', flags=re.M)


def error(msg, test_file=None):
  if test_file:
    msg = '{}: {}'.format(msg, test_file)
  print('ERROR: {}'.format(msg), file=sys.stderr)

def warn(msg, test_file=None):
  if test_file:
    msg = '{}: {}'.format(msg, test_file)
  print('WARNING: {}'.format(msg), file=sys.stderr)

def scrub_body(body):
  # Scrub runs of whitespace out of the assembly, but leave the leading
  # whitespace in place.
  body = SCRUB_WHITESPACE_RE.sub(r' ', body)
  # Expand the tabs used for indentation.
  body = string.expandtabs(body, 2)
  # Strip trailing whitespace.
  body = SCRUB_TRAILING_WHITESPACE_RE.sub(r'', body)
  return body

def do_scrub(body, scrubber, scrubber_args, extra):
  if scrubber_args:
    local_args = copy.deepcopy(scrubber_args)
    local_args[0].extra_scrub = extra
    return scrubber(body, *local_args)
  return scrubber(body, *scrubber_args)

# Build up a dictionary of all the function bodies.
class function_body(object):
  def __init__(self, string, extra, args_and_sig):
    self.scrub = string
    self.extrascrub = extra
    self.args_and_sig = args_and_sig
  def is_same_except_arg_names(self, extrascrub, args_and_sig):
    arg_names = set()
    def drop_arg_names(match):
        arg_names.add(match.group(2))
        return match.group(1) + match.group(3)
    def repl_arg_names(match):
        if match.group(2) in arg_names:
            return match.group(1) + match.group(3)
        return match.group(1) + match.group(2) + match.group(3)
    ans0 = IR_VALUE_RE.sub(drop_arg_names, self.args_and_sig)
    ans1 = IR_VALUE_RE.sub(drop_arg_names, args_and_sig)
    if ans0 != ans1:
        return False
    es0 = IR_VALUE_RE.sub(repl_arg_names, self.extrascrub)
    es1 = IR_VALUE_RE.sub(repl_arg_names, extrascrub)
    es0 = SCRUB_IR_COMMENT_RE.sub(r'', es0)
    es1 = SCRUB_IR_COMMENT_RE.sub(r'', es1)
    return es0 == es1

  def __str__(self):
    return self.scrub

def build_function_body_dictionary(function_re, scrubber, scrubber_args, raw_tool_output, prefixes, func_dict, verbose, record_args):
  for m in function_re.finditer(raw_tool_output):
    if not m:
      continue
    func = m.group('func')
    body = m.group('body')
    # Determine if we print arguments, the opening brace, or nothing after the function name
    if record_args and 'args_and_sig' in m.groupdict():
        args_and_sig = scrub_body(m.group('args_and_sig').strip())
    elif 'args_and_sig' in m.groupdict():
        args_and_sig = '('
    else:
        args_and_sig = ''
    scrubbed_body = do_scrub(body, scrubber, scrubber_args, extra = False)
    scrubbed_extra = do_scrub(body, scrubber, scrubber_args, extra = True)
    if 'analysis' in m.groupdict():
      analysis = m.group('analysis')
      if analysis.lower() != 'cost model analysis':
        warn('Unsupported analysis mode: %r!' % (analysis,))
    if func.startswith('stress'):
      # We only use the last line of the function body for stress tests.
      scrubbed_body = '\n'.join(scrubbed_body.splitlines()[-1:])
    if verbose:
      print('Processing function: ' + func, file=sys.stderr)
      for l in scrubbed_body.splitlines():
        print('  ' + l, file=sys.stderr)
    for prefix in prefixes:
      if func in func_dict[prefix] and (str(func_dict[prefix][func]) != scrubbed_body or (func_dict[prefix][func] and func_dict[prefix][func].args_and_sig != args_and_sig)):
        if func_dict[prefix][func] and func_dict[prefix][func].is_same_except_arg_names(scrubbed_extra, args_and_sig):
          func_dict[prefix][func].scrub = scrubbed_extra
          func_dict[prefix][func].args_and_sig = args_and_sig
          continue
        else:
          if prefix == prefixes[-1]:
            warn('Found conflicting asm under the same prefix: %r!' % (prefix,))
          else:
            func_dict[prefix][func] = None
            continue

      func_dict[prefix][func] = function_body(scrubbed_body, scrubbed_extra, args_and_sig)

##### Generator of LLVM IR CHECK lines

SCRUB_IR_COMMENT_RE = re.compile(r'\s*;.*')

# Match things that look at identifiers, but only if they are followed by
# spaces, commas, paren, or end of the string
IR_VALUE_RE = re.compile(r'(\s+)%([\w\.\-]+?)([,\s\(\)]|\Z)')

# Create a FileCheck variable name based on an IR name.
def get_value_name(var):
  if var.isdigit():
    var = 'TMP' + var
  var = var.replace('.', '_')
  var = var.replace('-', '_')
  return var.upper()


# Create a FileCheck variable from regex.
def get_value_definition(var):
  return '[[' + get_value_name(var) + ':%.*]]'


# Use a FileCheck variable.
def get_value_use(var):
  return '[[' + get_value_name(var) + ']]'

# Replace IR value defs and uses with FileCheck variables.
def genericize_check_lines(lines, is_analyze):
  # This gets called for each match that occurs in
  # a line. We transform variables we haven't seen
  # into defs, and variables we have seen into uses.
  def transform_line_vars(match):
    var = match.group(2)
    if var in vars_seen:
      rv = get_value_use(var)
    else:
      vars_seen.add(var)
      rv = get_value_definition(var)
    # re.sub replaces the entire regex match
    # with whatever you return, so we have
    # to make sure to hand it back everything
    # including the commas and spaces.
    return match.group(1) + rv + match.group(3)

  vars_seen = set()
  lines_with_def = []

  for i, line in enumerate(lines):
    # An IR variable named '%.' matches the FileCheck regex string.
    line = line.replace('%.', '%dot')
    # Ignore any comments, since the check lines will too.
    scrubbed_line = SCRUB_IR_COMMENT_RE.sub(r'', line)
    if is_analyze:
      lines[i] = scrubbed_line
    else:
      lines[i] = IR_VALUE_RE.sub(transform_line_vars, scrubbed_line)
  return lines


def add_checks(output_lines, comment_marker, prefix_list, func_dict, func_name, check_label_format, is_asm, is_analyze):
  printed_prefixes = []
  for p in prefix_list:
    checkprefixes = p[0]
    for checkprefix in checkprefixes:
      if checkprefix in printed_prefixes:
        break
      # TODO func_dict[checkprefix] may be None, '' or not exist.
      # Fix the call sites.
      if func_name not in func_dict[checkprefix] or not func_dict[checkprefix][func_name]:
        continue

      # Add some space between different check prefixes, but not after the last
      # check line (before the test code).
      if is_asm:
        if len(printed_prefixes) != 0:
          output_lines.append(comment_marker)

      printed_prefixes.append(checkprefix)
      args_and_sig = str(func_dict[checkprefix][func_name].args_and_sig)
      args_and_sig = genericize_check_lines([args_and_sig], is_analyze)[0]
      if '[[' in args_and_sig:
        output_lines.append(check_label_format % (checkprefix, func_name, ''))
        output_lines.append('%s %s-SAME: %s' % (comment_marker, checkprefix, args_and_sig))
      else:
        output_lines.append(check_label_format % (checkprefix, func_name, args_and_sig))
      func_body = str(func_dict[checkprefix][func_name]).splitlines()

      # For ASM output, just emit the check lines.
      if is_asm:
        output_lines.append('%s %s:       %s' % (comment_marker, checkprefix, func_body[0]))
        for func_line in func_body[1:]:
          output_lines.append('%s %s-NEXT:  %s' % (comment_marker, checkprefix, func_line))
        break

      # For IR output, change all defs to FileCheck variables, so we're immune
      # to variable naming fashions.
      func_body = genericize_check_lines(func_body, is_analyze)

      # This could be selectively enabled with an optional invocation argument.
      # Disabled for now: better to check everything. Be safe rather than sorry.

      # Handle the first line of the function body as a special case because
      # it's often just noise (a useless asm comment or entry label).
      #if func_body[0].startswith("#") or func_body[0].startswith("entry:"):
      #  is_blank_line = True
      #else:
      #  output_lines.append('%s %s:       %s' % (comment_marker, checkprefix, func_body[0]))
      #  is_blank_line = False

      is_blank_line = False

      for func_line in func_body:
        if func_line.strip() == '':
          is_blank_line = True
          continue
        # Do not waste time checking IR comments.
        func_line = SCRUB_IR_COMMENT_RE.sub(r'', func_line)

        # Skip blank lines instead of checking them.
        if is_blank_line:
          output_lines.append('{} {}:       {}'.format(
              comment_marker, checkprefix, func_line))
        else:
          output_lines.append('{} {}-NEXT:  {}'.format(
              comment_marker, checkprefix, func_line))
        is_blank_line = False

      # Add space between different check prefixes and also before the first
      # line of code in the test function.
      output_lines.append(comment_marker)
      break

def add_ir_checks(output_lines, comment_marker, prefix_list, func_dict,
                  func_name, preserve_names):
  # Label format is based on IR string.
  function_def_regex = 'define {{[^@]+}}'
  check_label_format = '{} %s-LABEL: {}@%s%s'.format(comment_marker, function_def_regex)
  add_checks(output_lines, comment_marker, prefix_list, func_dict, func_name,
             check_label_format, False, preserve_names)

def add_analyze_checks(output_lines, comment_marker, prefix_list, func_dict, func_name):
  check_label_format = '{} %s-LABEL: \'%s%s\''.format(comment_marker)
  add_checks(output_lines, comment_marker, prefix_list, func_dict, func_name, check_label_format, False, True)


def check_prefix(prefix):
  if not PREFIX_RE.match(prefix):
        hint = ""
        if ',' in prefix:
          hint = " Did you mean '--check-prefixes=" + prefix + "'?"
        warn(("Supplied prefix '%s' is invalid. Prefix must contain only alphanumeric characters, hyphens and underscores." + hint) %
             (prefix))


def verify_filecheck_prefixes(fc_cmd):
  fc_cmd_parts = fc_cmd.split()
  for part in fc_cmd_parts:
    if "check-prefix=" in part:
      prefix = part.split('=', 1)[1]
      check_prefix(prefix)
    elif "check-prefixes=" in part:
      prefixes = part.split('=', 1)[1].split(',')
      for prefix in prefixes:
        check_prefix(prefix)
        if prefixes.count(prefix) > 1:
          warn("Supplied prefix '%s' is not unique in the prefix list." % (prefix,))

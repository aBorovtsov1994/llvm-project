# -*- Python -*-

import os
import platform
import re
import subprocess
import sys
import tempfile

import lit.formats
import lit.util

from lit.llvm import llvm_config
from lit.llvm.subst import ToolSubst
from lit.llvm.subst import FindTool

# Configuration file for the 'lit' test runner.

# name: The name of this test suite.
config.name = 'debuginfo-tests'

# testFormat: The test format to use to interpret tests.
#
# For now we require '&&' between commands, until they get globally killed and
# the test runner updated.
config.test_format = lit.formats.ShTest(not llvm_config.use_lit_shell)

# suffixes: A list of file extensions to treat as test files.
config.suffixes = ['.c', '.cpp', '.m']

# excludes: A list of directories to exclude from the testsuite. The 'Inputs'
# subdirectories contain auxiliary inputs for various tests in their parent
# directories.
config.excludes = ['Inputs']

# test_source_root: The root path where tests are located.
config.test_source_root = os.path.join(config.debuginfo_tests_src_root)

# test_exec_root: The root path where tests should be run.
config.test_exec_root = config.debuginfo_tests_obj_root

llvm_config.use_default_substitutions()

tools = [
    ToolSubst('%test_debuginfo', command=os.path.join(
        config.debuginfo_tests_src_root, 'llgdb-tests', 'test_debuginfo.pl')),
]

def get_required_attr(config, attr_name):
  attr_value = getattr(config, attr_name, None)
  if attr_value == None:
    lit_config.fatal(
      "No attribute %r in test configuration! You may need to run "
      "tests from your build directory or add this attribute "
      "to lit.site.cfg " % attr_name)
  return attr_value

# If this is an MSVC environment, the tests at the root of the tree are
# unsupported. The local win_cdb test suite, however, is supported.
is_msvc = get_required_attr(config, "is_msvc")
if is_msvc:
    config.available_features.add('msvc')
    # FIXME: We should add some llvm lit utility code to find the Windows SDK
    # and set up the environment appopriately.
    win_sdk = 'C:/Program Files (x86)/Windows Kits/10/'
    arch = 'x64'
    llvm_config.with_system_environment(['LIB', 'LIBPATH', 'INCLUDE'])
    # Clear _NT_SYMBOL_PATH to prevent cdb from attempting to load symbols from
    # the network.
    llvm_config.with_environment('_NT_SYMBOL_PATH', '')
    tools.append(ToolSubst('%cdb', '"%s"' % os.path.join(win_sdk, 'Debuggers',
                                                         arch, 'cdb.exe')))

# clang_src_dir is not used by these tests, but is required by
# use_clang(), so set it to "".
if not hasattr(config, 'clang_src_dir'):
    config.clang_src_dir = ""
llvm_config.use_clang()

if config.llvm_use_sanitizer:
    # Propagate path to symbolizer for ASan/MSan.
    llvm_config.with_system_environment(
        ['ASAN_SYMBOLIZER_PATH', 'MSAN_SYMBOLIZER_PATH'])
llvm_config.with_environment('PATHTOCLANG', llvm_config.config.clang)
llvm_config.with_environment('PATHTOCLANGPP', llvm_config.use_llvm_tool('clang++'))
llvm_config.with_environment('PATHTOCLANGCL', llvm_config.use_llvm_tool('clang-cl'))

# Check which debuggers are available:
built_lldb = llvm_config.use_llvm_tool('lldb', search_env='CLANG')
if built_lldb is not None:
    lldb_path = built_lldb
elif lit.util.which('lldb') is not None:
    lldb_path = lit.util.which('lldb')

if lldb_path is not None:
    config.available_features.add('lldb')

# Produce dexter path, lldb path, and combine into the %dexter substitution
dexter_path = os.path.join(config.debuginfo_tests_src_root,
                           'dexter', 'dexter.py')
dexter_cmd = '{} {} test'.format(config.python3_executable, dexter_path)
if lldb_path is not None:
  dexter_cmd += ' --lldb-executable {}'.format(lldb_path)

tools.append(ToolSubst('%dexter', dexter_cmd))

tool_dirs = [config.llvm_tools_dir]

llvm_config.add_tool_substitutions(tools, tool_dirs)

lit.util.usePlatformSdkOnDarwin(config, lit_config)

# available_features: REQUIRES/UNSUPPORTED lit commands look at this list.
if platform.system() == 'Darwin':
    import subprocess
    xcode_lldb_vers = subprocess.check_output(['xcrun', 'lldb', '--version']).decode("utf-8")
    match = re.search('lldb-(\d+)', xcode_lldb_vers)
    if match:
        apple_lldb_vers = int(match.group(1))
        if apple_lldb_vers < 1000:
            config.available_features.add('apple-lldb-pre-1000')


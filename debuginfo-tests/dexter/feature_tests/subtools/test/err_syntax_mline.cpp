// Purpose:
//      Check that parsing bad commands gives a useful error.
//          - Syntax error (misplaced ',') over multiple lines
//      Check directives are in check.txt to prevent dexter reading any embedded
//      commands.
//
// Note: Despite using 'lldb' as the debugger, lldb is not actually required
//       as the test should finish before lldb would be invoked.
//
// RUN: not %dexter test --builder 'clang' --debugger "lldb" \
// RUN:     --cflags "-O0 -g" -v -- %s \
// RUN:     | FileCheck %s --match-full-lines --strict-whitespace
//
// CHECK:parser error:{{.*}}err_syntax_mline.cpp(24): invalid syntax
// CHECK:    ,'a', 3, 3, 3, 3, on_line=0)
// CHECK:    ^

int main(){
    return 0;
}

/*
DexExpectWatchValue(
    ,'a', 3, 3, 3, 3, on_line=0)
*/

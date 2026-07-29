// RUN: rm -rf %t.dir && mkdir -p %t.dir
// RUN: %clang_profgen -fcoverage-mapping %s -o %t.dir/app
// RUN: env LLVM_PROFILE_DENSE=1 LLVM_PROFILE_FILE=%t.dir/dense.profraw \
// RUN:   %run %t.dir/app
// RUN: env LLVM_PROFILE_FILE=%t.dir/sparse.profraw %run %t.dir/app
// RUN: /bin/sh -c 'test "$(stat -c %%s %t.dir/sparse.profraw)" -lt \
// RUN:   "$(stat -c %%s %t.dir/dense.profraw)"'
// RUN: llvm-profdata show --all-functions %t.dir/dense.profraw | \
// RUN:   FileCheck %s --check-prefix=DENSE-RAW
// RUN: llvm-profdata show --all-functions %t.dir/sparse.profraw | \
// RUN:   FileCheck %s --check-prefix=SPARSE-RAW
// RUN: cd %t.dir && env -u LLVM_PROFILE_FILE -u LLVM_PROFILE_DENSE \
// RUN:   %run ./app
// RUN: llvm-profdata show --all-functions %t.dir/default.profraw | \
// RUN:   FileCheck %s --check-prefix=SPARSE-RAW
// RUN: llvm-profdata merge -sparse %t.dir/dense.profraw \
// RUN:   -o %t.dir/dense.profdata
// RUN: llvm-profdata merge -sparse %t.dir/sparse.profraw \
// RUN:   -o %t.dir/sparse.profdata
// RUN: llvm-profdata show --function=covered --counts \
// RUN:   %t.dir/sparse.profdata | FileCheck %s --check-prefix=COUNTS
// RUN: llvm-cov report %t.dir/app -instr-profile=%t.dir/dense.profdata \
// RUN:   > %t.dir/dense.report
// RUN: llvm-cov report %t.dir/app -instr-profile=%t.dir/sparse.profdata \
// RUN:   > %t.dir/sparse.report
// RUN: diff %t.dir/dense.report %t.dir/sparse.report
//
// COUNTS: covered:
// COUNTS: Function count: 2
// COUNTS: Block counts: [1]
// DENSE-RAW: untouched:
// SPARSE-RAW-NOT: untouched:

__attribute__((noinline)) int covered(int Condition) {
  if (Condition)
    return 1;
  return 0;
}

__attribute__((noinline)) int untouched(void) { return 2; }

int main(void) {
  covered(1);
  covered(0);
  return 0;
}

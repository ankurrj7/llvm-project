// RUN: rm -rf %t.dir && mkdir -p %t.dir
// RUN: %clang_profgen %s -o %t.dir/app
//
// A dense online merger must reject a sparse-mode file without changing it.
// RUN: env LLVM_PROFILE_FILE=%t.dir/sparse-first-%m.profraw %run %t.dir/app
// RUN: cp %t.dir/sparse-first-*.profraw %t.dir/sparse-first.before
// RUN: env LLVM_PROFILE_DENSE=1 \
// RUN:   LLVM_PROFILE_FILE=%t.dir/sparse-first-%m.profraw %run %t.dir/app \
// RUN:   2>&1 | FileCheck %s --check-prefix=DENSE-REJECT
// RUN: cmp %t.dir/sparse-first.before %t.dir/sparse-first-*.profraw
// RUN: llvm-profdata merge %t.dir/sparse-first.before \
// RUN:   -o %t.dir/sparse-first.profdata
//
// A sparse writer must likewise reject an existing dense-mode file.
// RUN: env LLVM_PROFILE_DENSE=1 \
// RUN:   LLVM_PROFILE_FILE=%t.dir/dense-first-%m.profraw %run %t.dir/app
// RUN: cp %t.dir/dense-first-*.profraw %t.dir/dense-first.before
// RUN: env LLVM_PROFILE_FILE=%t.dir/dense-first-%m.profraw %run %t.dir/app \
// RUN:   2>&1 | FileCheck %s --check-prefix=SPARSE-REJECT
// RUN: cmp %t.dir/dense-first.before %t.dir/dense-first-*.profraw
// RUN: llvm-profdata merge %t.dir/dense-first.before \
// RUN:   -o %t.dir/dense-first.profdata
//
// DENSE-REJECT: source profile file is not compatible
// SPARSE-REJECT: Refusing to append sparse profile data to non-sparse or incompatible file

__attribute__((noinline)) void called(void) {}

int main(void) {
  called();
  return 0;
}

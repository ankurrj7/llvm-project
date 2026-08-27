// RUN: %clang -std=c++17 -fprofile-instr-generate -fcoverage-mapping \
// RUN:   -fcoverage-call-continuations -c %s -o %t.o
// RUN: llvm-cov export %t.o --txtcvrgfull --txtcvrg-view=segments \
// RUN:   | FileCheck %s

int value(bool);

int compound(bool fail) {
  int result = 0;
  result += value(fail);
  if (result)
    return result;
  return 0;
}

// A broad continuation region surrounds the nested call region in the raw
// coverage mapping. It resumes after the call, producing A-B-A source slices
// for only two static counters on line 10. The block view reports each counter
// once in that uninterrupted run, while retaining the later continuation block
// on line 11 which proves that the call returned.
// CHECK:      function	"_Z8compoundb"	7	0	0.00
// CHECK-NEXT: 1	8.25	10.13	0
// CHECK-NEXT: 1	10.13	10.24	0
// CHECK-NEXT: 1	11.3	11.14	0
// CHECK-NEXT: 1	12.5	12.18	0
// CHECK-NEXT: 1	12.18	12.19	0
// CHECK-NEXT: 1	13.3	13.11	0
// CHECK-NEXT: 1	13.11	14.2	0

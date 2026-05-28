// RUN: %clang_cc1 -mllvm -emptyline-comment-coverage=false -fprofile-instrument=clang -fcoverage-mapping -dump-coverage-mapping -emit-llvm-only -main-file-name macro-expansion.c %s | FileCheck %s

// CHECK: func
// CHECK:      File 1, [[@LINE+7]]:12 -> [[@LINE+7]]:38 = #0
// CHECK-NEXT: File 1, [[@LINE+6]]:15 -> [[@LINE+6]]:28 = (#0 + #2)
// CHECK-NEXT: File 1, [[@LINE+5]]:21 -> [[@LINE+5]]:22 = (#0 + #2)
// CHECK: Branch,File 1, [[@LINE+4]]:21 -> [[@LINE+4]]:22 = 0, ((#0 + #2) - #4)
// CHECK-NEXT: File 1, [[@LINE+3]]:24 -> [[@LINE+3]]:26 = #4
// CHECK-NEXT: File 1, [[@LINE+2]]:36 -> [[@LINE+2]]:37 = (#2 + #3)
// CHECK-NEXT: Branch,File 1, [[@LINE+1]]:36 -> [[@LINE+1]]:37 = 0, #3
#define M1 do { if (0) {} } while (0)
// CHECK-NEXT: File 2, [[@LINE+12]]:15 -> [[@LINE+12]]:41 = #3
// CHECK-NEXT: File 2, [[@LINE+11]]:18 -> [[@LINE+11]]:31 = (#3 + #5)
// CHECK-NEXT: File 2, [[@LINE+10]]:24 -> [[@LINE+10]]:25 = (#3 + #5)
// CHECK: File 2, [[@LINE+9]]:27 -> [[@LINE+9]]:29 = #7
// CHECK-NEXT: File 2, [[@LINE+8]]:39 -> [[@LINE+8]]:40 = (#5 + #6)
// CHECK-NEXT: Branch,File 2, [[@LINE+7]]:39 -> [[@LINE+7]]:40 = 0, #6
// CHECK-NEXT: File 3, [[@LINE+6]]:15 -> [[@LINE+6]]:41 = #6
// CHECK-NEXT: File 3, [[@LINE+5]]:18 -> [[@LINE+5]]:31 = (#6 + #8)
// CHECK-NEXT: File 3, [[@LINE+4]]:24 -> [[@LINE+4]]:25 = (#6 + #8)
// CHECK: File 3, [[@LINE+3]]:27 -> [[@LINE+3]]:29 = #10
// CHECK-NEXT: File 3, [[@LINE+2]]:39 -> [[@LINE+2]]:40 = (#8 + #9)
// CHECK-NEXT: Branch,File 3, [[@LINE+1]]:39 -> [[@LINE+1]]:40 = 0, #9
#define M2(x) do { if (x) {} } while (0)
// CHECK-NEXT: File 4, [[@LINE+5]]:15 -> [[@LINE+5]]:38 = #9
// CHECK-NEXT: File 4, [[@LINE+4]]:18 -> [[@LINE+4]]:28 = (#9 + #11)
// CHECK-NEXT: Expansion,File 4, [[@LINE+3]]:20 -> [[@LINE+3]]:22 = (#9 + #11)
// CHECK-NEXT: File 4, [[@LINE+2]]:36 -> [[@LINE+2]]:37 = (#11 + #12)
// CHECK-NEXT: Branch,File 4, [[@LINE+1]]:36 -> [[@LINE+1]]:37 = 0, #12
#define M3(x) do { M2(x); } while (0)
// CHECK-NEXT: File 5, [[@LINE+4]]:15 -> [[@LINE+4]]:27 = #12
// CHECK-NEXT: File 5, [[@LINE+3]]:16 -> [[@LINE+3]]:19 = #12
// CHECK-NEXT: Branch,File 5, [[@LINE+2]]:16 -> [[@LINE+2]]:19 = #17, (#12 - #17)
// CHECK-NEXT: File 5, [[@LINE+1]]:23 -> [[@LINE+1]]:26 = #17
#define M4(x) ((x) && (x))
// CHECK-NEXT: Branch,File 5, [[@LINE-1]]:23 -> [[@LINE-1]]:26 = #18, (#17 - #18)
// CHECK-NEXT: File 6, [[@LINE+4]]:15 -> [[@LINE+4]]:27 = #12
// CHECK-NEXT: File 6, [[@LINE+3]]:16 -> [[@LINE+3]]:19 = #12
// CHECK-NEXT: Branch,File 6, [[@LINE+2]]:16 -> [[@LINE+2]]:19 = (#12 - #20), #20
// CHECK-NEXT: File 6, [[@LINE+1]]:23 -> [[@LINE+1]]:26 = #20
#define M5(x) ((x) || (x))
// CHECK-NEXT: Branch,File 6, [[@LINE-1]]:23 -> [[@LINE-1]]:26 = (#20 - #21), #21
// CHECK-NEXT: File 7, [[@LINE+1]]:15 -> [[@LINE+1]]:26 = #12
#define M6(x) ((x) + (x))
// CHECK-NEXT: Branch,File 7, [[@LINE-1]]:15 -> [[@LINE-1]]:26 = #22, (#12 - #22)
// CHECK-NEXT: File 8, [[@LINE+1]]:15 -> [[@LINE+1]]:18 = #12
#define M7(x) (x)

// Check for the expansion of M2 within M3.
// CHECK-NEXT: Branch,File 8, [[@LINE-3]]:15 -> [[@LINE-3]]:18 = #23, (#12 - #23)
// CHECK-NEXT: File 9, {{[0-9]+}}:15 -> {{[0-9]+}}:41 = (#9 + #11)
// CHECK-NEXT: File 9, {{[0-9]+}}:18 -> {{[0-9]+}}:31 = ((#9 + #11) + #13)
// CHECK-NEXT: File 9, {{[0-9]+}}:24 -> {{[0-9]+}}:25 = ((#9 + #11) + #13)
// CHECK: File 9, {{[0-9]+}}:27 -> {{[0-9]+}}:29 = #15
// CHECK-NEXT: File 9, {{[0-9]+}}:39 -> {{[0-9]+}}:40 = (#13 + #14)

void func(int x) {
  if (x) {}
  M1;
  M2(!x);
  M2(x);
  M3(x);
  if (M4(x)) {}
  if (M5(x)) {}
  if (M6(x)) {}
  if (M7(x)) {}
}

int main(int argc, const char *argv[]) {
  func(0);
}

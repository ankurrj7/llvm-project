// RUN: %clang_cc1 -mllvm -emptyline-comment-coverage=false -std=c++11 -fprofile-instrument=clang -fcoverage-mapping -dump-coverage-mapping -emit-llvm-only -main-file-name macro-expressions.cpp -w %s | FileCheck %s

#define EXPR(x) (x)
#define NEXPR(x) (!x)
#define DECL(T, x) T x
#define ASSIGN(x, y) x = y
#define LT(x, y) x < y
#define INC(x) ++x
#define ARR(T, x, y, z) (T[3]){x, y, z}

#define PRI_64_LENGTH_MODIFIER "ll"
#define PRIo64 PRI_64_LENGTH_MODIFIER "o"
#define PRIu64 PRI_64_LENGTH_MODIFIER "u"

#define STMT(s) s

void fn1() {
  STMT(if (1));
  STMT(while (1));
  STMT(for (;;));
  STMT(if) (1);
  STMT(while) (1);
  STMT(for) (;;);
  if (1)
    STMT(if (1)
        STMT(if (1)));
  if (1)
    STMT(if (1)) 0;
  if (1)
    STMT(while (1)) 0;
  if (1)
    STMT(for (;;)) 0;
  while (1)
    STMT(if (1)) 0;
  while (1)
    STMT(while (1)) 0;
  while (1)
    STMT(for (;;)) 0;
  for (;;)
    STMT(if (1)) 0;
  for (;;)
    STMT(while (1)) 0;
  for (;;)
    STMT(for (;;)) 0;
}

void STMT(fn2()) {
}

void STMT(fn3)() {
}

// CHECK: foo
// CHECK-NEXT: File 0, [[@LINE+1]]:17 -> {{[0-9]+}}:2 = #0
void foo(int i) {
  // CHECK-NEXT: File 0, [[@LINE+3]]:7 -> [[@LINE+3]]:8 = #0
  // CHECK: Gap,File 0, [[@LINE+2]]:9 -> [[@LINE+2]]:10 = #1
  // CHECK-NEXT: File 0, [[@LINE+1]]:10 -> [[@LINE+1]]:12 = #1
  if (0) {}

  // CHECK-NEXT: Expansion,File 0, [[@LINE+3]]:7 -> [[@LINE+3]]:11 = #0
  // CHECK-NEXT: Gap,File 0, [[@LINE+2]]:15 -> [[@LINE+2]]:16 = #2
  // CHECK-NEXT: File 0, [[@LINE+1]]:16 -> [[@LINE+1]]:18 = #2
  if (EXPR(i)) {}
  // CHECK-NEXT: Expansion,File 0, [[@LINE+3]]:9 -> [[@LINE+3]]:14 = (#3 + #4)
  // CHECK-NEXT: Gap,File 0, [[@LINE+2]]:19 -> [[@LINE+2]]:20 = #3
  // CHECK-NEXT: File 0, [[@LINE+1]]:20 -> [[@LINE+1]]:22 = #3
  for (;NEXPR(i);) {}
  // CHECK: Expansion,File 0, [[@LINE+5]]:8 -> [[@LINE+5]]:14 = #4
  // CHECK-NEXT: Expansion,File 0, [[@LINE+4]]:33 -> [[@LINE+4]]:35 = (#5 + #6)
  // CHECK-NEXT: Expansion,File 0, [[@LINE+3]]:43 -> [[@LINE+3]]:46 = #5
  // CHECK-NEXT: Gap,File 0, [[@LINE+2]]:50 -> [[@LINE+2]]:51 = #5
  // CHECK: File 0, [[@LINE+1]]:51 -> [[@LINE+1]]:53 = #5
  for (ASSIGN(DECL(int, j), 0); LT(j, i); INC(j)) {}
  // CHECK: Expansion,File 0, [[@LINE+1]]:3 -> [[@LINE+1]]:9 = #6
  ASSIGN(DECL(int, k), 0);
  // CHECK: Expansion,File 0, [[@LINE+4]]:10 -> [[@LINE+4]]:12 = (#7 + #8)
  // CHECK-NEXT: Gap,File 0, [[@LINE+3]]:19 -> [[@LINE+3]]:20 = #7
  // CHECK-NEXT: File 0, [[@LINE+2]]:20 -> [[@LINE+2]]:31 = #7
  // CHECK-NEXT: Expansion,File 0, [[@LINE+1]]:22 -> [[@LINE+1]]:25 = #7
  while (LT(k, i)) { INC(k); }
  // CHECK: File 0, [[@LINE+2]]:6 -> [[@LINE+2]]:8 = (#8 + #9)
  // CHECK-NEXT: Expansion,File 0, [[@LINE+1]]:16 -> [[@LINE+1]]:21 = (#9 + #10)
  do {} while (NEXPR(i));
  // CHECK: Expansion,File 0, [[@LINE+6]]:8 -> [[@LINE+6]]:12 = #10
  // CHECK-NEXT: File 0, [[@LINE+5]]:21 -> [[@LINE+5]]:22 = (#11 + #12)
  // CHECK-NEXT: Branch,File 0, [[@LINE+4]]:21 -> [[@LINE+4]]:22 = #11, #12
  // CHECK-NEXT: Expansion,File 0, [[@LINE+3]]:23 -> [[@LINE+3]]:26 = #10
  // CHECK-NEXT: Gap,File 0, [[@LINE+2]]:41 -> [[@LINE+2]]:42 = #11
  // CHECK: File 0, [[@LINE+1]]:42 -> [[@LINE+1]]:44 = #11
  for (DECL(int, j) : ARR(int, 1, 2, 3)) {}

  // CHECK: File 0, [[@LINE+5]]:10 -> [[@LINE+5]]:11 = #12
  // CHECK-NEXT: Branch,File 0, [[@LINE+4]]:10 -> [[@LINE+4]]:11 = #13, (#12 - #13)
  // CHECK-NEXT: Gap,File 0, [[@LINE+3]]:13 -> [[@LINE+3]]:14 = #13
  // CHECK-NEXT: Expansion,File 0, [[@LINE+2]]:14 -> [[@LINE+2]]:20 = #12
  // CHECK-NEXT: Expansion,File 0, [[@LINE+1]]:23 -> [[@LINE+1]]:29 = #12
  (void)(i ? PRIo64 : PRIu64);

  // CHECK-NEXT: File 0, [[@LINE+6]]:10 -> [[@LINE+6]]:11 = #12
  // CHECK: File 0, [[@LINE+5]]:14 -> [[@LINE+5]]:15 = #14
  // CHECK-NEXT: File 0, [[@LINE+4]]:18 -> [[@LINE+4]]:33 = (#12 - #14)
  // CHECK-NEXT: Expansion,File 0, [[@LINE+3]]:18 -> [[@LINE+3]]:22 = (#12 - #14)
  // CHECK: File 0, [[@LINE+2]]:28 -> [[@LINE+2]]:29 = #15
  // CHECK-NEXT: File 0, [[@LINE+1]]:32 -> [[@LINE+1]]:33 = ((#12 - #14) - #15)
  (void)(i ? i : EXPR(i) ? i : 0);
  // CHECK-NEXT: File 0, [[@LINE+5]]:10 -> [[@LINE+5]]:11 = #12
  // CHECK-NEXT: Branch,File 0, [[@LINE+4]]:10 -> [[@LINE+4]]:11 = #16, (#12 - #16)
  // CHECK-NEXT: File 0, [[@LINE+3]]:15 -> [[@LINE+3]]:27 = (#12 - #16)
  // CHECK-NEXT: Expansion,File 0, [[@LINE+2]]:15 -> [[@LINE+2]]:19 = (#12 - #16)
  // CHECK-NEXT: File 0, [[@LINE+1]]:26 -> [[@LINE+1]]:27 = ((#12 - #16) - #17)
  (void)(i ?: EXPR(i) ?: 0);
}

// CHECK-NEXT: File {{[0-9]+}}, 3:17 -> 3:20 = #0
// CHECK-NEXT: Branch,File {{[0-9]+}}, 3:17 -> 3:20 = #2, (#0 - #2)
// CHECK-NEXT: File {{[0-9]+}}, 4:18 -> 4:22 = (#3 + #4)
// CHECK-NEXT: Branch,File {{[0-9]+}}, 4:18 -> 4:22 = #3, #4
// CHECK-NEXT: File {{[0-9]+}}, 6:22 -> 6:27 = #4
// CHECK-NEXT: File {{[0-9]+}}, 8:16 -> 8:19 = #5
// CHECK-NEXT: File {{[0-9]+}}, 7:18 -> 7:23 = (#5 + #6)
// CHECK-NEXT: Branch,File {{[0-9]+}}, 7:18 -> 7:23 = #5, #6
// CHECK-NEXT: File {{[0-9]+}}, 6:22 -> 6:27 = #6
// CHECK-NEXT: File {{[0-9]+}}, 8:16 -> 8:19 = #7
// CHECK-NEXT: File {{[0-9]+}}, 7:18 -> 7:23 = (#7 + #8)
// CHECK-NEXT: Branch,File {{[0-9]+}}, 7:18 -> 7:23 = #7, #8
// CHECK-NEXT: File {{[0-9]+}}, 4:18 -> 4:22 = (#9 + #10)
// CHECK-NEXT: Branch,File {{[0-9]+}}, 4:18 -> 4:22 = #9, #10
// CHECK-NEXT: File {{[0-9]+}}, 5:20 -> 5:23 = #10
// CHECK-NEXT: File {{[0-9]+}}, 9:25 -> 9:40 = #10
// CHECK-NEXT: File {{[0-9]+}}, 12:16 -> 12:42 = #12
// CHECK-NEXT: Expansion,File {{[0-9]+}}, 12:16 -> 12:38 = #13
// CHECK-NEXT: File {{[0-9]+}}, 12:38 -> 12:42 = #13
// CHECK-NEXT: File {{[0-9]+}}, 13:16 -> 13:42 = #12
// CHECK-NEXT: Expansion,File {{[0-9]+}}, 13:16 -> 13:38 = (#12 - #13)
// CHECK-NEXT: File {{[0-9]+}}, 13:38 -> 13:42 = (#12 - #13)
// CHECK-NEXT: File {{[0-9]+}}, 3:17 -> 3:20 = (#12 - #14)
// CHECK-NEXT: Branch,File {{[0-9]+}}, 3:17 -> 3:20 = #15, ((#12 - #14) - #15)
// CHECK-NEXT: File {{[0-9]+}}, 3:17 -> 3:20 = (#12 - #16)
// CHECK-NEXT: Branch,File {{[0-9]+}}, 3:17 -> 3:20 = #17, ((#12 - #16) - #17)
// CHECK-NEXT: File {{[0-9]+}}, 11:32 -> 11:36 = #13
// CHECK-NEXT: File {{[0-9]+}}, 11:32 -> 11:36 = (#12 - #13)

// CHECK-NOT: File {{[0-9]+}},
// CHECK: main

int main(int argc, const char *argv[]) {
  foo(10);
}

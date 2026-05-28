// RUN: %clang_cc1 -mllvm -emptyline-comment-coverage=false -fprofile-instrument=clang -fcoverage-mapping -dump-coverage-mapping -emit-llvm-only -main-file-name loops.cpp %s | FileCheck %s

// CHECK: main
int main(void) {                    // CHECK-NEXT: File 0, [[@LINE]]:16 -> [[@LINE+9]]:2 = #0
  int j = 0;                        // CHECK: File 0, [[@LINE+1]]:9 -> [[@LINE+1]]:14 = (#1 + #2)
  while(j < 5) ++j;                 // CHECK: Branch,File 0, [[@LINE]]:9 -> [[@LINE]]:14 = #1, #2
                                    // CHECK: File 0, [[@LINE-1]]:16 -> [[@LINE-1]]:19 = #1
  j = 0;
  while                             // CHECK: File 0, [[@LINE+1]]:5 -> [[@LINE+1]]:10 = (#3 + #4)
   (j < 5)                          // CHECK: Branch,File 0, [[@LINE]]:5 -> [[@LINE]]:10 = #3, #4
     ++j;                           // CHECK: File 0, [[@LINE]]:6 -> [[@LINE]]:9 = #3
  return 0;                         // CHECK: File 0, [[@LINE]]:3 -> [[@LINE]]:11 = #4
}

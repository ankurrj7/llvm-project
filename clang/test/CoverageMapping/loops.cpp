// RUN: %clang_cc1 -mllvm -emptyline-comment-coverage=false -std=c++11 -fprofile-instrument=clang -fcoverage-mapping -dump-coverage-mapping -emit-llvm-only -main-file-name loops.cpp %s | FileCheck %s

// CHECK: rangedFor
void rangedFor() {                  // CHECK-NEXT: File 0, [[@LINE]]:18 -> {{[0-9]+}}:2 = #0
  int arr[] = { 1, 2, 3, 4, 5 };
  int sum = 0;                      // CHECK: File 0, [[@LINE+2]]:14 -> [[@LINE+2]]:15 = (#1 + #2)
                                    // CHECK: Branch,File 0, [[@LINE+1]]:14 -> [[@LINE+1]]:15 = #1, #2
  for(auto i : arr) {               // CHECK: File 0, [[@LINE]]:21 -> [[@LINE+6]]:4 = #1
    if (i == 3)
      continue;                     // CHECK: File 0, [[@LINE]]:7 -> [[@LINE]]:15 = #3
    sum += i;                       // CHECK: File 0, [[@LINE]]:5 -> {{[0-9]+}}:4 = (#1 - #3)
    if (sum >= 7)
      break;                        // CHECK: File 0, [[@LINE]]:7 -> [[@LINE]]:12 = #4
  }

  // CHECK: File 0, [[@LINE+1]]:7 -> [[@LINE+1]]:10 = (#2 + #4)
  if (sum) {}
}

                                    // CHECK: main:
int main() {                        // CHECK-NEXT: File 0, [[@LINE]]:12 -> {{.*}}:2 = #0
                                    // CHECK: File 0, [[@LINE+1]]:18 -> [[@LINE+1]]:24 = (#1 + #2)
  for(int i = 0; i < 10; ++i)       // CHECK: Branch,File 0, [[@LINE]]:18 -> [[@LINE]]:24 = #1, #2
     ;
  for(int i = 0;
      i < 10;                       // CHECK: File 0, [[@LINE]]:7 -> [[@LINE]]:13 = (#3 + #4)
      ++i)                          // CHECK: Branch,File 0, [[@LINE-1]]:7 -> [[@LINE-1]]:13 = #3, #4
  {
    int x = 0;                      // CHECK: File 0, [[@LINE-1]]:3 -> [[@LINE+1]]:4 = #3
  }
  int j = 0;                        // CHECK: File 0, [[@LINE+2]]:9 -> [[@LINE+2]]:14 = (#5 + #6)
                                    // CHECK: Branch,File 0, [[@LINE+1]]:9 -> [[@LINE+1]]:14 = #5, #6
  while(j < 5) ++j;                 // CHECK: File 0, [[@LINE]]:16 -> [[@LINE]]:19 = #5

  do {                              // CHECK: File 0, [[@LINE]]:6 -> [[@LINE+2]]:4 = (#6 + #7)
    ++j;
  } while(j < 10);                  // CHECK: File 0, [[@LINE]]:11 -> [[@LINE]]:17 = (#7 + #8)
                                    // CHECK: Branch,File 0, [[@LINE-1]]:11 -> [[@LINE-1]]:17 = #7, #8
  j = 0;
  while                             // CHECK: File 0, [[@LINE+1]]:5 -> [[@LINE+1]]:10 = (#9 + #10)
   (j < 5)                          // CHECK: Branch,File 0, [[@LINE]]:5 -> [[@LINE]]:10 = #9, #10
     ++j;                           // CHECK: File 0, [[@LINE]]:6 -> [[@LINE]]:9 = #9
  do                                // CHECK: File 0, [[@LINE+1]]:5 -> [[@LINE+1]]:8 = (#10 + #11)
    ++j;
  while(j < 10);                    // CHECK: File 0, [[@LINE]]:9 -> [[@LINE]]:15 = (#11 + #12)
                                    // CHECK: Branch,File 0, [[@LINE-1]]:9 -> [[@LINE-1]]:15 = #11, #12
  rangedFor();
  return 0;
}

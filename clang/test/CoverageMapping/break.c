// RUN: %clang_cc1 -mllvm -emptyline-comment-coverage=false -fprofile-instrument=clang -fcoverage-mapping -dump-coverage-mapping -emit-llvm-only -main-file-name break.c %s | FileCheck %s

int main(void) {     // CHECK: File 0, [[@LINE]]:16 -> {{[0-9]+}}:2 = #0
  int cnt = 0;       // CHECK-NEXT: File 0, [[@LINE+1]]:9 -> [[@LINE+1]]:18 = (#1 + #2)
  while(cnt < 100) { // CHECK: File 0, [[@LINE]]:20 -> [[@LINE+3]]:4 = #1
    break;           // CHECK-NEXT: Gap,File 0, [[@LINE]]:11 -> [[@LINE+1]]:5 = 0
    ++cnt;           // CHECK-NEXT: File 0, [[@LINE]]:5 -> [[@LINE+1]]:4 = 0
  }                  // CHECK: File 0, [[@LINE+1]]:9 -> [[@LINE+1]]:18 = (#3 + #4)
  while(cnt < 100) { // CHECK: File 0, [[@LINE]]:20 -> [[@LINE+6]]:4 = #3
    {
      break;         // CHECK: Gap,File 0, [[@LINE]]:13 -> [[@LINE+1]]:7 = 0
      ++cnt;         // CHECK-NEXT: File 0, [[@LINE]]:7 -> [[@LINE+3]]:4 = 0
    }                // CHECK: Gap,File 0, [[@LINE]]:6 -> [[@LINE+1]]:5 = 0
    ++cnt;
  }                  // CHECK: File 0, [[@LINE+1]]:9 -> [[@LINE+1]]:18 = (#5 + #6)
  while(cnt < 100) { // CHECK: File 0, [[@LINE]]:20 -> [[@LINE+7]]:4 = #5
                     // CHECK-NEXT: File 0, [[@LINE+1]]:8 -> [[@LINE+1]]:16 = #5
    if(cnt == 0) {   // CHECK: File 0, [[@LINE]]:18 -> [[@LINE+3]]:6 = #7
      break;         // CHECK: Gap,File 0, [[@LINE]]:13 -> [[@LINE+1]]:7 = 0
      ++cnt;         // CHECK-NEXT: File 0, [[@LINE]]:7 -> [[@LINE+1]]:6 = 0
    }                // CHECK: Gap,File 0, [[@LINE]]:6 -> [[@LINE+1]]:5 = (#5 - #7)
    ++cnt;           // CHECK-NEXT: File 0, [[@LINE]]:5 -> [[@LINE+1]]:4 = (#5 - #7)
  }                  // CHECK: File 0, [[@LINE+1]]:9 -> [[@LINE+1]]:18 = (#8 + #9)
  while(cnt < 100) { // CHECK: File 0, [[@LINE]]:20 -> [[@LINE+8]]:4 = #8
                     // CHECK-NEXT: File 0, [[@LINE+1]]:8 -> [[@LINE+1]]:16 = #8
    if(cnt == 0) {   // CHECK: File 0, [[@LINE]]:18 -> [[@LINE+2]]:6 = #10
      ++cnt;         // CHECK-NEXT: Gap,File 0, [[@LINE+1]]:6 -> [[@LINE+1]]:12 = (#8 - #10)
    } else {         // CHECK-NEXT: File 0, [[@LINE]]:12 -> [[@LINE+2]]:6 = (#8 - #10)
      break;
    }
    ++cnt;
  }
}

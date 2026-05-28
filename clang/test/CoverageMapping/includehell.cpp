// RUN: %clang_cc1 -mllvm -emptyline-comment-coverage=false -fprofile-instrument=clang -fcoverage-mapping -dump-coverage-mapping -emit-llvm-only -main-file-name includehell.cpp %s > %tmapping

int main() {
  int x = 0;

  #include "Inputs/starts_a_scope"
    x = x;
    #include "Inputs/code.h"
    x = x;
  #include "Inputs/ends_a_scope"

  #include "Inputs/starts_a_scope"
    #include "Inputs/code.h"
  #include "Inputs/ends_a_scope"

  #include "Inputs/starts_a_scope"
  #include "Inputs/ends_a_scope"

  return 0;
}

// RUN: FileCheck -input-file %tmapping %s --check-prefix=CHECK-MAIN
// RUN: FileCheck -input-file %tmapping %s --check-prefix=CHECK-START
// RUN: FileCheck -input-file %tmapping %s --check-prefix=CHECK-CODE
// RUN: FileCheck -input-file %tmapping %s --check-prefix=CHECK-END

// CHECK-MAIN: File [[MAIN:[0-9]]], 3:12 -> 20:2 = #0
// CHECK-MAIN-NEXT: Expansion,File [[MAIN]], 6:12 -> 6:35 = #0
// CHECK-MAIN-NEXT: File [[MAIN]], 6:35 -> 10:33 = #1
// CHECK-MAIN-NEXT: Expansion,File [[MAIN]], 8:14 -> 8:29 = #1
// CHECK-MAIN-NEXT: Expansion,File [[MAIN]], 10:12 -> 10:33 = #0
// CHECK-MAIN-NEXT: File [[MAIN]], 10:33 -> 20:2 = #2
// CHECK-MAIN-NEXT: Expansion,File [[MAIN]], 12:12 -> 12:35 = #2
// CHECK-MAIN-NEXT: File [[MAIN]], 12:35 -> 14:33 = #6
// CHECK-MAIN-NEXT: Expansion,File [[MAIN]], 13:14 -> 13:29 = #6
// CHECK-MAIN-NEXT: Expansion,File [[MAIN]], 14:12 -> 14:33 = #2
// CHECK-MAIN-NEXT: File [[MAIN]], 14:33 -> 20:2 = #7
// CHECK-MAIN-NEXT: Expansion,File [[MAIN]], 16:12 -> 16:35 = #7
// CHECK-MAIN-NEXT: File [[MAIN]], 16:35 -> 17:33 = #11
// CHECK-MAIN-NEXT: Expansion,File [[MAIN]], 17:12 -> 17:33 = #7
// CHECK-MAIN-NEXT: File [[MAIN]], 17:33 -> 19:11 = #12

// CHECK-START: File [[START1:[0-9]]], 1:1 -> 5:1 = #0
// CHECK-START: File [[START1]], 4:17 -> 4:22 = (#1 + #2)
// CHECK-START: Branch,File [[START1]], 4:17 -> 4:22 = #1, #2
// CHECK-START: File [[START1]], 4:24 -> 4:27 = #1
// CHECK-START: Gap,File [[START1]], 4:28 -> 4:29 = #1
// CHECK-START: File [[START1]], 4:29 -> 5:1 = #1
// CHECK-START: File [[START2:[0-9]]], 1:1 -> 5:1 = #2
// CHECK-START: File [[START2]], 4:17 -> 4:22 = (#6 + #7)
// CHECK-START: Branch,File [[START2]], 4:17 -> 4:22 = #6, #7
// CHECK-START: File [[START2]], 4:24 -> 4:27 = #6
// CHECK-START: Gap,File [[START2]], 4:28 -> 4:29 = #6
// CHECK-START: File [[START2]], 4:29 -> 5:1 = #6
// CHECK-START: File [[START3:[0-9]]], 1:1 -> 5:1 = #7
// CHECK-START: File [[START3]], 4:17 -> 4:22 = (#11 + #12)
// CHECK-START: Branch,File [[START3]], 4:17 -> 4:22 = #11, #12
// CHECK-START: File [[START3]], 4:24 -> 4:27 = #11
// CHECK-START: Gap,File [[START3]], 4:28 -> 4:29 = #11
// CHECK-START: File [[START3]], 4:29 -> 5:1 = #11

// CHECK-CODE:      File [[CODE1:[0-9]]], 1:1 -> 14:1 = #1
// CHECK-CODE-NEXT: File [[CODE1]], 4:5 -> 4:11 = #1
// CHECK-CODE-NEXT: Branch,File [[CODE1]], 4:5 -> 4:11 = #3, (#1 - #3)
// CHECK-CODE-NEXT: Gap,File [[CODE1]], 4:12 -> 4:13 = #3
// CHECK-CODE: File [[CODE1]], 4:13 -> 6:2 = #3
// CHECK-CODE: Gap,File [[CODE1]], 6:2 -> 6:8 = (#1 - #3)
// CHECK-CODE: File [[CODE1]], 6:8 -> 8:2 = (#1 - #3)
// CHECK-CODE-NEXT: File [[CODE1]], 9:5 -> 9:9 = #1
// CHECK-CODE-NEXT: Branch,File [[CODE1]], 9:5 -> 9:9 = #4, 0
// CHECK-CODE-NEXT: Gap,File [[CODE1]], 9:10 -> 9:11 = #4
// CHECK-CODE: File [[CODE1]], 9:11 -> 11:2 = #4
// CHECK-CODE: Gap,File [[CODE1]], 11:2 -> 11:8 = (#1 - #4)
// CHECK-CODE: File [[CODE1]], 11:8 -> 13:2 = (#1 - #4)
// CHECK-CODE:      File [[CODE2:[0-9]]], 1:1 -> 14:1 = #6
// CHECK-CODE-NEXT: File [[CODE2]], 4:5 -> 4:11 = #6
// CHECK-CODE-NEXT: Branch,File [[CODE2]], 4:5 -> 4:11 = #8, (#6 - #8)
// CHECK-CODE-NEXT: Gap,File [[CODE2]], 4:12 -> 4:13 = #8
// CHECK-CODE: File [[CODE2]], 4:13 -> 6:2 = #8
// CHECK-CODE: Gap,File [[CODE2]], 6:2 -> 6:8 = (#6 - #8)
// CHECK-CODE: File [[CODE2]], 6:8 -> 8:2 = (#6 - #8)
// CHECK-CODE-NEXT: File [[CODE2]], 9:5 -> 9:9 = #6
// CHECK-CODE-NEXT: Branch,File [[CODE2]], 9:5 -> 9:9 = #9, 0
// CHECK-CODE-NEXT: Gap,File [[CODE2]], 9:10 -> 9:11 = #9
// CHECK-CODE: File [[CODE2]], 9:11 -> 11:2 = #9
// CHECK-CODE: Gap,File [[CODE2]], 11:2 -> 11:8 = (#6 - #9)
// CHECK-CODE: File [[CODE2]], 11:8 -> 13:2 = (#6 - #9)

// CHECK-END: File [[END1:[0-9]]], 1:1 -> 6:1 = #0
// CHECK-END: File [[END1]], 1:1 -> 3:2 = #1
// CHECK-END: Gap,File [[END1]], 3:2 -> 5:1 = #2
// CHECK-END: File [[END1]], 5:1 -> 6:1 = #2
// CHECK-END: File [[END1]], 5:5 -> 5:9 = #2
// CHECK-END: Branch,File [[END1]], 5:5 -> 5:9 = #5, 0
// CHECK-END: Gap,File [[END1]], 5:10 -> 5:11 = #5
// CHECK-END: File [[END1]], 5:11 -> 5:16 = #5
// CHECK-END: File [[END2:[0-9]]], 1:1 -> 6:1 = #2
// CHECK-END: File [[END2]], 1:1 -> 3:2 = #6
// CHECK-END: Gap,File [[END2]], 3:2 -> 5:1 = #7
// CHECK-END: File [[END2]], 5:1 -> 6:1 = #7
// CHECK-END: File [[END2]], 5:5 -> 5:9 = #7
// CHECK-END: Branch,File [[END2]], 5:5 -> 5:9 = #10, 0
// CHECK-END: Gap,File [[END2]], 5:10 -> 5:11 = #10
// CHECK-END: File [[END2]], 5:11 -> 5:16 = #10
// CHECK-END: File [[END3:[0-9]]], 1:1 -> 6:1 = #7
// CHECK-END: File [[END3]], 1:1 -> 3:2 = #11
// CHECK-END: Gap,File [[END3]], 3:2 -> 5:1 = #12
// CHECK-END: File [[END3]], 5:1 -> 6:1 = #12
// CHECK-END: File [[END3]], 5:5 -> 5:9 = #12
// CHECK-END: Branch,File [[END3]], 5:5 -> 5:9 = #13, 0
// CHECK-END: Gap,File [[END3]], 5:10 -> 5:11 = #13
// CHECK-END: File [[END3]], 5:11 -> 5:16 = #13

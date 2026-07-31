// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -ftrapv -ftrapv-handler overflow_callback -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefix=ON
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -ftrapv -ftrapv-handler overflow_callback -fprofile-instrument=clang -fcoverage-mapping -emit-llvm -o - %s | FileCheck %s --check-prefix=OFF
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -ftrapv -ftrapv-handler overflow_callback -mllvm -enable-single-byte-coverage=true -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefix=SB
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -ftrapv -ftrapv-handler overflow_callback -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -dump-coverage-mapping -emit-llvm-only -o - %s | FileCheck %s --check-prefix=MAP
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -ftrapv -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefix=TRAP

void sink(int);

void add(int left, int right) {
  int result = left + right;
  sink(result);
}

void compound_add(int *left, int right) {
  *left += right;
  sink(*left);
}

void negate(int value) {
  int result = -value;
  sink(result);
}

void increment(int *value) {
  ++*value;
  sink(*value);
}

// ON-DAG: @__profc_add = private global [3 x i64]
// ON-DAG: @__profc_compound_add = private global [3 x i64]
// ON-DAG: @__profc_negate = private global [3 x i64]
// ON-DAG: @__profc_increment = private global [3 x i64]

// SB-DAG: @__profc_add = private global [3 x i8]
// SB-DAG: @__profc_compound_add = private global [3 x i8]
// SB-DAG: @__profc_negate = private global [3 x i8]
// SB-DAG: @__profc_increment = private global [3 x i8]

// OFF-DAG: @__profc_add = private global [1 x i64]
// OFF-DAG: @__profc_compound_add = private global [1 x i64]
// OFF-DAG: @__profc_negate = private global [1 x i64]
// OFF-DAG: @__profc_increment = private global [1 x i64]

// A built-in trap has the same non-returning overflow edge as a custom
// handler. Its normal-operation continuation must still guard following code.
// TRAP-DAG: @__profc_add = private global [3 x i64]
// TRAP-DAG: @__profc_compound_add = private global [3 x i64]
// TRAP-DAG: @__profc_negate = private global [3 x i64]
// TRAP-DAG: @__profc_increment = private global [3 x i64]

// TRAP-LABEL: define{{.*}} void @add(
// TRAP: br i1 {{.*}}, label %[[CONT:[^, ]+]], label %[[TRAP_BLOCK:[^, ]+]]
// TRAP: [[TRAP_BLOCK]]:
// TRAP-NEXT: call void @llvm.ubsantrap
// TRAP-NEXT: unreachable
// TRAP: [[CONT]]:
// TRAP-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_add, i32 0, i32 1)
// TRAP: call void @sink(

// ON-LABEL: define{{.*}} void @add(
// ON: nooverflow:
// ON-NEXT: {{.*}}phi i32
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_add, i32 0, i32 1)
// ON: call void @sink(
// ON: overflow:
// ON: call i64 (i64, i64, i8, i8, ...) @overflow_callback(

// ON-LABEL: define{{.*}} void @compound_add(
// ON: nooverflow:
// ON: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_compound_add, i32 0, i32 1)
// ON: call void @sink(
// ON: overflow:
// ON: call i64 (i64, i64, i8, i8, ...) @overflow_callback(

// ON-LABEL: define{{.*}} void @negate(
// ON: nooverflow:
// ON-NEXT: {{.*}}phi i32
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_negate, i32 0, i32 1)
// ON: call void @sink(
// ON: overflow:
// ON: call i64 (i64, i64, i8, i8, ...) @overflow_callback(

// ON-LABEL: define{{.*}} void @increment(
// ON: nooverflow:
// ON: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_increment, i32 0, i32 1)
// ON: call void @sink(
// ON: overflow:
// ON: call i64 (i64, i64, i8, i8, ...) @overflow_callback(

// MAP-LABEL: add:
// MAP: Gap,File 0, [[ADD_END:[0-9]+]]:{{[0-9]+}} -> [[ADD_SINK:[0-9]+]]:3 = #1
// MAP-NEXT: File 0, [[ADD_SINK]]:3 -> [[ADD_SINK]]:{{[0-9]+}} = #1
// MAP-LABEL: compound_add:
// MAP: Gap,File 0, [[COMPOUND_END:[0-9]+]]:{{[0-9]+}} -> [[COMPOUND_SINK:[0-9]+]]:3 = #1
// MAP-NEXT: File 0, [[COMPOUND_SINK]]:3 -> [[COMPOUND_SINK]]:{{[0-9]+}} = #1
// MAP-LABEL: negate:
// MAP: Gap,File 0, [[NEGATE_END:[0-9]+]]:{{[0-9]+}} -> [[NEGATE_SINK:[0-9]+]]:3 = #1
// MAP-NEXT: File 0, [[NEGATE_SINK]]:3 -> [[NEGATE_SINK]]:{{[0-9]+}} = #1
// MAP-LABEL: increment:
// MAP: Gap,File 0, [[INC_END:[0-9]+]]:{{[0-9]+}} -> [[INC_SINK:[0-9]+]]:3 = #1
// MAP-NEXT: File 0, [[INC_SINK]]:3 -> [[INC_SINK]]:{{[0-9]+}} = #1

// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefix=ON
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fprofile-instrument=clang -fcoverage-mapping -emit-llvm -o - %s | FileCheck %s --check-prefix=OFF
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -mllvm -enable-single-byte-coverage=true -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefix=SB
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -dump-coverage-mapping -emit-llvm-only -o - %s | FileCheck %s --check-prefix=MAP

_Complex double left_value;
_Complex double right_value;
void sink(int);

void complex_divide(void) {
  _Complex double result = left_value / right_value;
  sink(__real__ result != 0);
}

void complex_multiply(void) {
  _Complex double result = left_value * right_value;
  sink(__real__ result != 0);
}

void complex_divide_assign(void) {
  left_value /= right_value;
  sink(__real__ left_value != 0);
}

// ON-DAG: @__profc_complex_divide = private global [3 x i64]
// ON-DAG: @__profc_complex_multiply = private global [3 x i64]
// ON-DAG: @__profc_complex_divide_assign = private global [3 x i64]

// SB-DAG: @__profc_complex_divide = private global [3 x i8]
// SB-DAG: @__profc_complex_multiply = private global [3 x i8]
// SB-DAG: @__profc_complex_divide_assign = private global [3 x i8]

// OFF-DAG: @__profc_complex_divide = private global [1 x i64]
// OFF-DAG: @__profc_complex_multiply = private global [1 x i64]
// OFF-DAG: @__profc_complex_divide_assign = private global [1 x i64]

// ON-LABEL: define{{.*}} void @complex_divide(
// ON: call {{.*}} @__divdc3(
// ON: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_complex_divide, i32 0, i32 1)
// ON: call void @sink(

// Multiplication may use a fast inline path, but every slow __muldc3 path
// joins the same operation-completion counter before following source.
// ON-LABEL: define{{.*}} void @complex_multiply(
// ON: call {{.*}} @__muldc3(
// ON: br label %[[MUL_CONT:[^, ]+]]
// ON: [[MUL_CONT]]:
// ON: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_complex_multiply, i32 0, i32 1)
// ON: call void @sink(

// ON-LABEL: define{{.*}} void @complex_divide_assign(
// ON: call {{.*}} @__divdc3(
// ON: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_complex_divide_assign, i32 0, i32 1)
// ON: call void @sink(

// MAP-LABEL: complex_divide:
// MAP: Gap,File 0, [[DIV_END:[0-9]+]]:{{[0-9]+}} -> [[DIV_SINK:[0-9]+]]:3 = #1
// MAP-NEXT: File 0, [[DIV_SINK]]:3 -> [[DIV_SINK]]:{{[0-9]+}} = #1
// MAP-LABEL: complex_multiply:
// MAP: Gap,File 0, [[MUL_END:[0-9]+]]:{{[0-9]+}} -> [[MUL_SINK:[0-9]+]]:3 = #1
// MAP-NEXT: File 0, [[MUL_SINK]]:3 -> [[MUL_SINK]]:{{[0-9]+}} = #1
// MAP-LABEL: complex_divide_assign:
// MAP: Gap,File 0, [[ASSIGN_END:[0-9]+]]:{{[0-9]+}} -> [[ASSIGN_SINK:[0-9]+]]:3 = #1
// MAP-NEXT: File 0, [[ASSIGN_SINK]]:3 -> [[ASSIGN_SINK]]:{{[0-9]+}} = #1

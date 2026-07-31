// RUN: %clang_cc1 -std=c11 -triple x86_64-unknown-linux-gnu -Wno-atomic-alignment -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefix=ON
// RUN: %clang_cc1 -std=c11 -triple x86_64-unknown-linux-gnu -Wno-atomic-alignment -fprofile-instrument=clang -fcoverage-mapping -emit-llvm -o - %s | FileCheck %s --check-prefix=OFF
// RUN: %clang_cc1 -std=c11 -triple x86_64-unknown-linux-gnu -Wno-atomic-alignment -mllvm -enable-single-byte-coverage=true -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefix=SB
// RUN: %clang_cc1 -std=c11 -triple x86_64-unknown-linux-gnu -Wno-atomic-alignment -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -dump-coverage-mapping -emit-llvm-only -o - %s | FileCheck %s --check-prefix=MAP

typedef struct {
  long values[4];
} Big;

typedef struct {
  char value[9];
} Padded;

_Atomic(Big) aggregate;
_Atomic(Padded) padded_source;
_Atomic(int) scalar;
_Atomic(__int128) wide;
_Atomic(_Complex double) complex_value;

void after(int);

void ordinary_aggregate(void) {
  Big value = aggregate;
  after(value.values[0]);
}

void ordinary_scalar(void) {
  int value = scalar;
  after(value);
}

void ordinary_complex(void) {
  _Complex double value = complex_value;
  after(value != 0);
}

void unary_operations(void) {
  ++wide;
  after(1);
  wide--;
  after(2);
}

void builtin_operations(Big value) {
  value = __c11_atomic_load(&aggregate, __ATOMIC_SEQ_CST);
  after(3);
  __c11_atomic_store(&aggregate, value, __ATOMIC_SEQ_CST);
  after(4);
  value = __c11_atomic_exchange(&aggregate, value, __ATOMIC_SEQ_CST);
  after(5);
}

void padded_atomic_copy(void) {
  _Atomic(Padded) destination = padded_source;
  after(6);
}

// ON-DAG: @__profc_ordinary_aggregate = private global [3 x i64]
// ON-DAG: @__profc_ordinary_scalar = private global [3 x i64]
// ON-DAG: @__profc_ordinary_complex = private global [3 x i64]
// ON-DAG: @__profc_unary_operations = private global [5 x i64]
// ON-DAG: @__profc_builtin_operations = private global [7 x i64]
// ON-DAG: @__profc_padded_atomic_copy = private global [3 x i64]

// SB-DAG: @__profc_ordinary_aggregate = private global [3 x i8]
// SB-DAG: @__profc_ordinary_scalar = private global [3 x i8]
// SB-DAG: @__profc_ordinary_complex = private global [3 x i8]
// SB-DAG: @__profc_unary_operations = private global [5 x i8]
// SB-DAG: @__profc_builtin_operations = private global [7 x i8]
// SB-DAG: @__profc_padded_atomic_copy = private global [3 x i8]

// OFF-DAG: @__profc_ordinary_aggregate = private global [1 x i64]
// OFF-DAG: @__profc_ordinary_scalar = private global [1 x i64]
// OFF-DAG: @__profc_ordinary_complex = private global [1 x i64]
// OFF-DAG: @__profc_unary_operations = private global [1 x i64]
// OFF-DAG: @__profc_builtin_operations = private global [1 x i64]
// OFF-DAG: @__profc_padded_atomic_copy = private global [1 x i64]

// ON-LABEL: define{{.*}} void @ordinary_aggregate(
// ON: call void @__atomic_load(
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_ordinary_aggregate, i32 0, i32 1)
// ON: call void @after(

// ON-LABEL: define{{.*}} void @ordinary_scalar(
// ON: load atomic i32, ptr @scalar
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_ordinary_scalar, i32 0, i32 1)
// ON: call void @after(

// ON-LABEL: define{{.*}} void @ordinary_complex(
// ON: call void @__atomic_load(
// ON: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_ordinary_complex, i32 0, i32 1)
// ON: call void @after(

// ON-LABEL: define{{.*}} void @unary_operations(
// ON: atomicrmw add ptr @wide
// ON: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_unary_operations, i32 0, i32 1)
// ON: call void @after(i32 noundef 1)
// ON: atomicrmw sub ptr @wide
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_unary_operations, i32 0, i32 3)

// ON-LABEL: define{{.*}} void @builtin_operations(
// ON: call void @__atomic_load(
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_builtin_operations, i32 0, i32 1)
// ON: call void @__atomic_store(
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_builtin_operations, i32 0, i32 3)
// ON: call void @__atomic_exchange(
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_builtin_operations, i32 0, i32 5)

// The aggregate inverse-cast peephole bypasses the inner CastExpr visitor.
// Its atomic-load continuation must still be emitted before following source.
// ON-LABEL: define{{.*}} void @padded_atomic_copy(
// ON: call void @__atomic_load(
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_padded_atomic_copy, i32 0, i32 1)
// ON: call void @after(i32 noundef 6)

// MAP-LABEL: ordinary_aggregate:
// MAP: Gap,File 0, [[AGG_END:[0-9]+]]:{{[0-9]+}} -> [[AGG_AFTER:[0-9]+]]:3 = #1
// MAP-NEXT: File 0, [[AGG_AFTER]]:3 -> [[AGG_AFTER]]:{{[0-9]+}} = #1
// MAP-LABEL: ordinary_scalar:
// MAP: Gap,File 0, [[SCALAR_END:[0-9]+]]:{{[0-9]+}} -> [[SCALAR_AFTER:[0-9]+]]:3 = #1
// MAP-NEXT: File 0, [[SCALAR_AFTER]]:3 -> [[SCALAR_AFTER]]:{{[0-9]+}} = #1
// MAP-LABEL: unary_operations:
// MAP: Gap,File 0, [[INC_END:[0-9]+]]:{{[0-9]+}} -> [[INC_AFTER:[0-9]+]]:3 = #1
// MAP-NEXT: File 0, [[INC_AFTER]]:3 -> [[INC_AFTER]]:{{[0-9]+}} = #1
// MAP-LABEL: builtin_operations:
// MAP: Gap,File 0, [[LOAD_END:[0-9]+]]:{{[0-9]+}} -> [[LOAD_AFTER:[0-9]+]]:3 = #1
// MAP-NEXT: File 0, [[LOAD_AFTER]]:3 -> [[LOAD_AFTER]]:{{[0-9]+}} = #1
// MAP-LABEL: padded_atomic_copy:
// MAP: Gap,File 0, [[COPY_END:[0-9]+]]:{{[0-9]+}} -> [[COPY_AFTER:[0-9]+]]:3 = #1
// MAP-NEXT: File 0, [[COPY_AFTER]]:3 -> [[COPY_AFTER]]:{{[0-9]+}} = #1

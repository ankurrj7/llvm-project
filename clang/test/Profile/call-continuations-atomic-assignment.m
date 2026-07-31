// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fblocks -emit-llvm -o - %s | FileCheck %s --check-prefix=LOWERING
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fblocks -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefix=ON
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fblocks -fprofile-instrument=clang -fcoverage-mapping -emit-llvm -o - %s | FileCheck %s --check-prefix=OFF

typedef struct {
  long values[3];
} Big;
typedef _Atomic(Big) AtomicBig;
typedef _Atomic(__int128) AtomicI128;
typedef _Atomic(_Complex double) AtomicComplex;

AtomicBig *atomic_destination(void);
Big make_atomic_value(void);
AtomicI128 *i128_destination(void);
__int128 make_i128(void);
AtomicComplex *complex_destination(void);
_Complex double make_complex(void);
void lhs_side_effect(void);
void after_assignment(int);

void atomic_assignment(void) {
  (*atomic_destination() = make_atomic_value(), after_assignment(1));
}

void block_atomic_assignment(void) {
  __block AtomicBig value;
  (*(lhs_side_effect(), &value) = make_atomic_value(), after_assignment(2));
}

void i128_compound_assignment(void) {
  (*i128_destination() += make_i128(), after_assignment(3));
}

void complex_atomic_assignment(void) {
  (*complex_destination() = make_complex(), after_assignment(4));
}

void complex_compound_assignment(void) {
  (*complex_destination() += make_complex(), after_assignment(5));
}

// Ordinary aggregate assignment computes its destination before its value.
// This is also the feature-off lowering and must not depend on coverage.
// LOWERING-LABEL: define{{.*}} void @atomic_assignment(
// LOWERING: call ptr @atomic_destination()
// LOWERING: call void @make_atomic_value(
// LOWERING: call void @__atomic_store(
// LOWERING: call void @after_assignment(i32 noundef 1)

// The established __block exception remains RHS-first when the RHS has side
// effects, so that a block copy cannot redirect an already-computed address.
// LOWERING-LABEL: define{{.*}} void @block_atomic_assignment(
// LOWERING: call void @make_atomic_value(
// LOWERING: call void @lhs_side_effect()
// LOWERING: call void @__atomic_store(
// LOWERING: call void @after_assignment(i32 noundef 2)

// Compound atomics preserve their existing RHS-first operand order. The
// update/store is the operation whose normal completion controls the comma.
// LOWERING-LABEL: define{{.*}} void @i128_compound_assignment(
// LOWERING: call i128 @make_i128()
// LOWERING: call ptr @i128_destination()
// LOWERING: atomicrmw add
// LOWERING: call void @after_assignment(i32 noundef 3)

// LOWERING-LABEL: define{{.*}} void @complex_atomic_assignment(
// LOWERING: call{{.*}} @make_complex()
// LOWERING: call ptr @complex_destination()
// LOWERING: call void @__atomic_store(
// LOWERING: call void @after_assignment(i32 noundef 4)

// LOWERING-LABEL: define{{.*}} void @complex_compound_assignment(
// LOWERING: call{{.*}} @make_complex()
// LOWERING: call ptr @complex_destination()
// LOWERING: call void @__atomic_load(
// LOWERING: call void @__atomic_store(
// LOWERING: call void @after_assignment(i32 noundef 5)

// Continuation allocation and emission follow those same orders.
// ON-DAG: @__profc_atomic_assignment = private global [5 x i64]
// ON-DAG: @__profc_block_atomic_assignment = private global [5 x i64]
// ON-DAG: @__profc_i128_compound_assignment = private global [5 x i64]
// ON-DAG: @__profc_complex_atomic_assignment = private global [5 x i64]
// ON-DAG: @__profc_complex_compound_assignment = private global [5 x i64]

// ON-LABEL: define{{.*}} void @atomic_assignment(
// ON: call ptr @atomic_destination()
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_atomic_assignment, i32 0, i32 1)
// ON: call void @make_atomic_value(
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_atomic_assignment, i32 0, i32 2)
// ON: call void @__atomic_store(
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_atomic_assignment, i32 0, i32 3)
// ON: call void @after_assignment(i32 noundef 1)
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_atomic_assignment, i32 0, i32 4)

// ON-LABEL: define{{.*}} void @block_atomic_assignment(
// ON: call void @make_atomic_value(
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_block_atomic_assignment, i32 0, i32 1)
// ON: call void @lhs_side_effect()
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_block_atomic_assignment, i32 0, i32 2)
// ON: call void @__atomic_store(
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_block_atomic_assignment, i32 0, i32 3)
// ON: call void @after_assignment(i32 noundef 2)
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_block_atomic_assignment, i32 0, i32 4)

// ON-LABEL: define{{.*}} void @i128_compound_assignment(
// ON: call i128 @make_i128()
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_i128_compound_assignment, i32 0, i32 1)
// ON: call ptr @i128_destination()
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_i128_compound_assignment, i32 0, i32 2)
// ON: atomicrmw add
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_i128_compound_assignment, i32 0, i32 3)
// ON: call void @after_assignment(i32 noundef 3)
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_i128_compound_assignment, i32 0, i32 4)

// ON-LABEL: define{{.*}} void @complex_atomic_assignment(
// ON: call{{.*}} @make_complex()
// ON: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_complex_atomic_assignment, i32 0, i32 1)
// ON: call ptr @complex_destination()
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_complex_atomic_assignment, i32 0, i32 2)
// ON: call void @__atomic_store(
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_complex_atomic_assignment, i32 0, i32 3)
// ON: call void @after_assignment(i32 noundef 4)
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_complex_atomic_assignment, i32 0, i32 4)

// ON-LABEL: define{{.*}} void @complex_compound_assignment(
// ON: call{{.*}} @make_complex()
// ON: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_complex_compound_assignment, i32 0, i32 1)
// ON: call ptr @complex_destination()
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_complex_compound_assignment, i32 0, i32 2)
// ON: call void @__atomic_load(
// ON: call void @__atomic_store(
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_complex_compound_assignment, i32 0, i32 3)
// ON: call void @after_assignment(i32 noundef 5)
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_complex_compound_assignment, i32 0, i32 4)

// OFF-DAG: @__profc_atomic_assignment = private global [1 x i64]
// OFF-DAG: @__profc_block_atomic_assignment = private global [1 x i64]
// OFF-DAG: @__profc_i128_compound_assignment = private global [1 x i64]
// OFF-DAG: @__profc_complex_atomic_assignment = private global [1 x i64]
// OFF-DAG: @__profc_complex_compound_assignment = private global [1 x i64]

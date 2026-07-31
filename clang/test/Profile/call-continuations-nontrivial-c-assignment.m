// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fobjc-runtime=gnustep-2.0 -fobjc-arc -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefix=ON
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fobjc-runtime=gnustep-2.0 -fobjc-arc -fprofile-instrument=clang -fcoverage-mapping -emit-llvm -o - %s | FileCheck %s --check-prefix=OFF

__attribute__((objc_root_class))
@interface Root
@end

typedef struct {
  __strong Root *value;
} StrongHolder;

typedef struct {
  __weak Root *value;
} WeakHolder;

void after_assignment(int);
int make_argument(void);
void use_ordered(int, int);

void strong_pointer(__strong Root **dst, Root *src) {
  (*dst = src, after_assignment(1));
}

void weak_pointer(__weak Root **dst, Root *src) {
  (*dst = src, after_assignment(2));
}

void strong_struct(StrongHolder *dst, StrongHolder *src) {
  (*dst = *src, after_assignment(3));
}

void weak_struct(WeakHolder *dst, WeakHolder *src) {
  (*dst = *src, after_assignment(4));
}

void strong_struct_standalone(StrongHolder *dst, StrongHolder *src) {
  *dst = *src;
  after_assignment(5);
}

void strong_chained(StrongHolder *first, StrongHolder *second,
                    StrongHolder *third) {
  (*first = *second = *third, after_assignment(6));
}

void ordered_assignment(StrongHolder *dst, StrongHolder *src) {
  use_ordered(((*dst = *src), 1), make_argument());
}

// ARC scalar assignment must publish normal completion after the hidden store
// and before the comma RHS.
// ON-LABEL: define{{.*}} void @strong_pointer(
// ON: call void @llvm.objc.storeStrong
// ON: call void @llvm.objc.storeStrong
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_strong_pointer
// ON: call void @after_assignment(i32 noundef 1)

// ON-LABEL: define{{.*}} void @weak_pointer(
// ON: call ptr @llvm.objc.storeWeak
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_weak_pointer
// ON: call void @after_assignment(i32 noundef 2)

// Non-trivial C struct assignment has the same intra-expression boundary.
// ON-LABEL: define{{.*}} void @strong_struct(
// ON: call void @__copy_assignment_{{.*}}
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_strong_struct
// ON: call void @after_assignment(i32 noundef 3)

// ON-LABEL: define{{.*}} void @weak_struct(
// ON: call void @__copy_assignment_{{.*}}
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_weak_struct
// ON: call void @after_assignment(i32 noundef 4)

// A standalone aggregate assignment first publishes assignment completion and
// then full-expression cleanup completion before the next statement.
// ON-LABEL: define{{.*}} void @strong_struct_standalone(
// ON: call void @__copy_assignment_{{.*}}
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_strong_struct_standalone
// ON: store i64
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_strong_struct_standalone
// ON: call void @after_assignment(i32 noundef 5)

// The inner chained assignment completes before the generated outer copy.
// ON-LABEL: define{{.*}} void @strong_chained(
// ON: call void @__copy_assignment_{{.*}}
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_strong_chained
// ON: call void @__copy_assignment_{{.*}}
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_strong_chained
// ON: call void @after_assignment(i32 noundef 6)

// Assignment as one ordered call operand retains its own completion edge.
// ON-LABEL: define{{.*}} void @ordered_assignment(
// ON: call void @__copy_assignment_{{.*}}
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_ordered_assignment
// ON: call{{.*}} i32 @make_argument()
// ON: call void @use_ordered

// Every added assignment edge remains strictly opt-in.
// OFF-DAG: @__profc_strong_pointer = private global [1 x i64]
// OFF-DAG: @__profc_weak_pointer = private global [1 x i64]
// OFF-DAG: @__profc_strong_struct = private global [1 x i64]
// OFF-DAG: @__profc_weak_struct = private global [1 x i64]
// OFF-DAG: @__profc_strong_struct_standalone = private global [1 x i64]
// OFF-DAG: @__profc_strong_chained = private global [1 x i64]
// OFF-DAG: @__profc_ordered_assignment = private global [1 x i64]

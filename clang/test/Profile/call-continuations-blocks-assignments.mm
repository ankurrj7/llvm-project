// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fobjc-runtime=gnustep-2.0 -fobjc-arc -fblocks -fexceptions -fcxx-exceptions -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefix=ON
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fobjc-runtime=gnustep-2.0 -fobjc-arc -fblocks -fexceptions -fcxx-exceptions -fprofile-instrument=clang -fcoverage-mapping -emit-llvm -o - %s | FileCheck %s --check-prefix=OFF

typedef int (^IntBlock)(void);

void sink(int);

struct ThrowingCapture {
  int value;
  ThrowingCapture(const ThrowingCapture &);
  ~ThrowingCapture() noexcept(false);
};

struct NoThrowCapture {
  int value;
  NoThrowCapture(const NoThrowCapture &) noexcept;
  ~NoThrowCapture() noexcept;
};

void throwing_block_capture(ThrowingCapture captured) {
  {
    IntBlock block = ^{ return captured.value; };
    (void)block;
    sink(1);
  }
  sink(2);
}

void nothrow_block_capture(NoThrowCapture captured) {
  (void)^{ return captured.value; };
  sink(3);
}

void escaping_byref(IntBlock __strong *output) {
  {
    __block int value = 0;
    *output = ^{ return ++value; };
  }
  sink(4);
}

__attribute__((objc_root_class))
@interface Root
@end

void strong_assignment(__strong Root **dst, Root *src) {
  (*dst = src, sink(5));
}

void weak_assignment(__weak Root **dst, Root *src) {
  (*dst = src, sink(6));
}

Root * __strong &strong_assignment_lvalue(Root * __strong &dst, Root *src) {
  return (dst = src);
}

struct CXXAssignment {
  CXXAssignment &operator=(const CXXAssignment &);
};

void cxx_assignment(CXXAssignment &dst, const CXXAssignment &src) {
  (dst = src, sink(7));
}

// Capture-copy completion is recorded immediately after the potentially
// throwing hidden constructor. The nested-scope counter follows both ARC block
// release and direct capture destruction.
// ON-LABEL: define{{.*}} void @_Z22throwing_block_capture15ThrowingCapture(
// ON: call void @_ZN15ThrowingCaptureC1ERKS_
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z22throwing_block_capture15ThrowingCapture, i32 0, i32 1)
// ON: call void @llvm.objc.storeStrong({{.*}}null)
// ON-NEXT: call void @_ZN15ThrowingCaptureD1Ev
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z22throwing_block_capture15ThrowingCapture, i32 0, i32 4)
// ON: call void @_Z4sinki(i32 noundef 2)

// Nothrow capture construction uses the same source completion boundary.
// ON-LABEL: define{{.*}} void @_Z21nothrow_block_capture14NoThrowCapture(
// ON: call void @_ZN14NoThrowCaptureC1ERKS_
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z21nothrow_block_capture14NoThrowCapture, i32 0, i32 1)
// ON: {{call|invoke}} void @_Z4sinki(i32 noundef 3)

// A byref-only block literal has no BlockLiteral capture-copy counter. Its
// escaping byref storage still publishes compound completion after disposal.
// ON-LABEL: define{{.*}} void @_Z14escaping_byrefPU8__strongU13block_pointerFivE(
// ON: call void @_Block_object_dispose
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z14escaping_byrefPU8__strongU13block_pointerFivE, i32 0, i32 3)
// ON: call void @_Z4sinki(i32 noundef 4)

// ARC scalar assignments are instrumented in value and lvalue emission paths.
// ON-LABEL: define{{.*}} void @_Z17strong_assignmentPU8__strongP4RootS0_(
// ON: call void @llvm.objc.storeStrong
// ON: call void @llvm.objc.storeStrong
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z17strong_assignmentPU8__strongP4RootS0_, i32 0, i32 1)
// ON: call void @_Z4sinki(i32 noundef 5)

// ON-LABEL: define{{.*}} void @_Z15weak_assignmentPU6__weakP4RootS0_(
// ON: call ptr @llvm.objc.storeWeak
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z15weak_assignmentPU6__weakP4RootS0_, i32 0, i32 1)
// ON: call void @_Z4sinki(i32 noundef 6)

// ON-LABEL: define{{.*}} ptr @_Z24strong_assignment_lvalueRU8__strongP4RootS0_(
// ON: call void @llvm.objc.storeStrong
// ON: call void @llvm.objc.storeStrong
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z24strong_assignment_lvalueRU8__strongP4RootS0_, i32 0, i32 1)
// ON: ret ptr

// An overloaded C++ operator= is already a normal call. It gets exactly one
// continuation before the comma RHS, not an additional Assignment counter.
// ON-LABEL: define{{.*}} void @_Z14cxx_assignmentR13CXXAssignmentRKS_(
// ON: call{{.*}} @_ZN13CXXAssignmentaSERKS_
// ON-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z14cxx_assignmentR13CXXAssignmentRKS_, i32 0, i32 1)
// ON: store i64
// ON-NOT: load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z14cxx_assignmentR13CXXAssignmentRKS_
// ON: call void @_Z4sinki(i32 noundef 7)

// OFF-DAG: @__profc__Z22throwing_block_capture15ThrowingCapture = private global [1 x i64]
// OFF-DAG: @__profc__Z21nothrow_block_capture14NoThrowCapture = private global [1 x i64]
// OFF-DAG: @__profc__Z14escaping_byrefPU8__strongU13block_pointerFivE = private global [1 x i64]
// OFF-DAG: @__profc__Z17strong_assignmentPU8__strongP4RootS0_ = private global [1 x i64]
// OFF-DAG: @__profc__Z15weak_assignmentPU6__weakP4RootS0_ = private global [1 x i64]
// OFF-DAG: @__profc__Z24strong_assignment_lvalueRU8__strongP4RootS0_ = private global [1 x i64]
// OFF-DAG: @__profc__Z14cxx_assignmentR13CXXAssignmentRKS_ = private global [1 x i64]

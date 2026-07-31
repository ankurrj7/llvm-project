// RUN: %clang_cc1 -triple x86_64-apple-darwin10 -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefix=ON
// RUN: %clang_cc1 -triple x86_64-apple-darwin10 -fprofile-instrument=clang -fcoverage-mapping -emit-llvm -o - %s | FileCheck %s --check-prefix=OFF
// RUN: %clang_cc1 -x c++ -std=c++17 -triple x86_64-apple-darwin10 -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefix=ON
// RUN: %clang_cc1 -x c++ -std=c++17 -triple x86_64-apple-darwin10 -fprofile-instrument=clang -fcoverage-mapping -emit-llvm -o - %s | FileCheck %s --check-prefix=OFF

#define SWIFT_ASYNC_CALL __attribute__((swiftasynccall))
#define ASYNC_CONTEXT __attribute__((swift_async_context))

typedef SWIFT_ASYNC_CALL void (*AsyncFn)(char *ASYNC_CONTEXT);

#ifdef __cplusplus
extern "C" {
#endif

SWIFT_ASYNC_CALL void leaf(char *ASYNC_CONTEXT context) {
  ++*context;
}

SWIFT_ASYNC_CALL void direct(char *ASYNC_CONTEXT context) {
  leaf(context);
  return leaf(context);
}

SWIFT_ASYNC_CALL void indirect(AsyncFn function,
                               char *ASYNC_CONTEXT context) {
  function(context);
  return function(context);
}

void synchronous(char *context) {
  return leaf(context);
}

#ifdef __cplusplus
}
#endif

// The ordinary calls in direct and indirect each need one continuation. Their
// implicit musttail return calls must not reserve counters which cannot be
// emitted between a musttail call and its return. A synchronous caller does
// not receive the implicit-musttail treatment.
// ON-DAG: @__profc_leaf = private global [1 x i64]
// ON-DAG: @__profc_direct = private global [2 x i64]
// ON-DAG: @__profc_indirect = private global [2 x i64]
// ON-DAG: @__profc_synchronous = private global [2 x i64]

// OFF-DAG: @__profc_leaf = private global [1 x i64]
// OFF-DAG: @__profc_direct = private global [1 x i64]
// OFF-DAG: @__profc_indirect = private global [1 x i64]
// OFF-DAG: @__profc_synchronous = private global [1 x i64]

// ON-LABEL: define swifttailcc void @direct(
// ON: call swifttailcc void @leaf(
// ON-NEXT: load i64, ptr getelementptr inbounds ([2 x i64], ptr @__profc_direct, i32 0, i32 1)
// ON: musttail call swifttailcc void @leaf(
// ON-NEXT: ret void

// ON-LABEL: define swifttailcc void @indirect(
// ON: call swifttailcc void %{{[0-9]+}}(
// ON-NEXT: load i64, ptr getelementptr inbounds ([2 x i64], ptr @__profc_indirect, i32 0, i32 1)
// ON: musttail call swifttailcc void %{{[0-9]+}}(
// ON-NEXT: ret void

// ON-LABEL: define void @synchronous(
// ON: call swifttailcc void @leaf(
// ON-NEXT: load i64, ptr getelementptr inbounds ([2 x i64], ptr @__profc_synchronous, i32 0, i32 1)
// ON: ret void

// OFF-LABEL: define swifttailcc void @direct(
// OFF-NOT: getelementptr inbounds ({{.*}}@__profc_direct
// OFF: musttail call swifttailcc void @leaf(
// OFF-NEXT: ret void

// OFF-LABEL: define swifttailcc void @indirect(
// OFF-NOT: getelementptr inbounds ({{.*}}@__profc_indirect
// OFF: musttail call swifttailcc void %{{[0-9]+}}(
// OFF-NEXT: ret void

// OFF-LABEL: define void @synchronous(
// OFF-NOT: getelementptr inbounds ({{.*}}@__profc_synchronous
// OFF: call swifttailcc void @leaf(
// OFF-NEXT: ret void

// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefix=ITANIUM
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fprofile-instrument=clang -fcoverage-mapping -emit-llvm -o - %s | FileCheck %s --check-prefix=OFF
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -mllvm -enable-single-byte-coverage=true -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefix=SB
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -dump-coverage-mapping -emit-llvm-only -o - %s | FileCheck %s --check-prefix=MAP
// RUN: %clang_cc1 -std=c++20 -triple x86_64-pc-windows-msvc -fms-compatibility-version=19.25 -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefix=MS
// RUN: %clang_cc1 -std=c++20 -triple x86_64-pc-windows-msvc -fms-compatibility-version=19.25 -fno-ms-tls-guards -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefix=MS-NO-GUARDS
// RUN: %clang_cc1 -std=c++20 -triple x86_64-pc-windows-msvc -fms-compatibility-version=19.20 -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefix=MS-NO-GUARDS

struct Value {
  Value();
  ~Value();
  int number;
};

extern thread_local Value global_value;

struct Holder {
  static thread_local Value member_value;
};

Holder &get_holder();
void sink(int);

void direct_access() {
  int value = global_value.number;
  sink(value);
}

void member_access() {
  int value = get_holder().member_value.number;
  sink(value);
}

void discarded_access() {
  (void)global_value;
  sink(1);
}

thread_local const int constant_value = 7;

void constant_access() {
  sink(constant_value);
}

#ifndef _MSC_VER
static thread_local Value weak_value __attribute__((weakref("global_value")));

void weakref_access() {
  sink(weak_value.number);
}

void function_local_access() {
  static thread_local Value local_value;
  sink(local_value.number);
}
#endif

// ITANIUM-DAG: @__profc__Z13direct_accessv = private global [3 x i64]
// ITANIUM-DAG: @__profc__Z13member_accessv = private global [4 x i64]
// ITANIUM-DAG: @__profc__Z16discarded_accessv = private global [3 x i64]
// ITANIUM-DAG: @__profc__Z15constant_accessv = private global [3 x i64]
// ITANIUM-DAG: @__profc__Z14weakref_accessv = private global [2 x i64]
// ITANIUM-DAG: @__profc__Z21function_local_accessv = private global [4 x i64]

// SB-DAG: @__profc__Z13direct_accessv = private global [3 x i8]
// SB-DAG: @__profc__Z13member_accessv = private global [4 x i8]
// SB-DAG: @__profc__Z16discarded_accessv = private global [3 x i8]
// SB-DAG: @__profc__Z15constant_accessv = private global [3 x i8]
// SB-DAG: @__profc__Z14weakref_accessv = private global [2 x i8]
// SB-DAG: @__profc__Z21function_local_accessv = private global [4 x i8]

// OFF-DAG: @__profc__Z13direct_accessv = private global [1 x i64]
// OFF-DAG: @__profc__Z13member_accessv = private global [1 x i64]
// OFF-DAG: @__profc__Z16discarded_accessv = private global [1 x i64]
// OFF-DAG: @__profc__Z15constant_accessv = private global [1 x i64]
// OFF-DAG: @__profc__Z14weakref_accessv = private global [1 x i64]
// OFF-DAG: @__profc__Z21function_local_accessv = private global [1 x i64]

// ITANIUM-LABEL: define{{.*}} void @_Z13direct_accessv(
// ITANIUM: call ptr @_ZTW12global_value()
// ITANIUM-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z13direct_accessv, i32 0, i32 1)
// ITANIUM: call void @_Z4sinki(

// The written base call completes first (#1). The hidden static-member TLS
// wrapper then completes (#2), which is the source count of the following call.
// ITANIUM-LABEL: define{{.*}} void @_Z13member_accessv(
// ITANIUM: call{{.*}} @_Z10get_holderv()
// ITANIUM: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z13member_accessv, i32 0, i32 1)
// ITANIUM: call ptr @_ZTWN6Holder12member_valueE()
// ITANIUM-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z13member_accessv, i32 0, i32 2)
// ITANIUM: call void @_Z4sinki(

// ITANIUM-LABEL: define{{.*}} void @_Z16discarded_accessv(
// ITANIUM: call ptr @_ZTW12global_value()
// ITANIUM-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z16discarded_accessv, i32 0, i32 1)

// Constant-initialized scalar TLS uses no wrapper. It still gets the harmless
// direct-path TLS continuation needed for source-stable inline/COMDAT layouts.
// ITANIUM-LABEL: define{{.*}} void @_Z15constant_accessv(
// ITANIUM-NOT: call ptr @_ZTW
// ITANIUM: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z15constant_accessv, i32 0, i32 2)
// ITANIUM: call void @_Z4sinki(i32 noundef 7)
// ITANIUM: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z15constant_accessv, i32 0, i32 1)

// A weakref access is emitted through the aliased variable rather than a TLS
// wrapper owned by this declaration, so it gets no TLS-access continuation.
// ITANIUM-LABEL: define{{.*}} void @_Z14weakref_accessv(
// ITANIUM-NOT: call ptr @_ZTW
// ITANIUM: call void @_Z4sinki(

// Function-local dynamic TLS initialization has constructor and declaration
// continuations. It must not also reserve a global TLS-access continuation.
// ITANIUM-LABEL: define{{.*}} void @_Z21function_local_accessv(
// ITANIUM-NOT: call ptr @_ZTW
// ITANIUM: call{{.*}} @_ZN5ValueC1Ev
// ITANIUM: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z21function_local_accessv, i32 0, i32 1)
// ITANIUM: call void @_Z4sinki(

// MAP-LABEL: _Z13direct_accessv:
// MAP: Gap,File 0, [[DIRECT_END:[0-9]+]]:{{[0-9]+}} -> [[DIRECT_SINK:[0-9]+]]:3 = #1
// MAP-NEXT: File 0, [[DIRECT_SINK]]:3 -> [[DIRECT_SINK]]:{{[0-9]+}} = #1
// MAP-LABEL: _Z13member_accessv:
// MAP: Gap,File 0, [[MEMBER_END:[0-9]+]]:{{[0-9]+}} -> [[MEMBER_SINK:[0-9]+]]:3 = #2
// MAP-NEXT: File 0, [[MEMBER_SINK]]:3 -> [[MEMBER_SINK]]:{{[0-9]+}} = #2
// MAP-LABEL: _Z16discarded_accessv:
// MAP: Gap,File 0, [[DISCARD_END:[0-9]+]]:{{[0-9]+}} -> [[DISCARD_SINK:[0-9]+]]:3 = #1
// MAP-NEXT: File 0, [[DISCARD_SINK]]:3 -> [[DISCARD_SINK]]:{{[0-9]+}} = #1

// MS-DAG: @"__profc_?direct_access@@YAXXZ" = private global [3 x i64]
// MS-DAG: @"__profc_?member_access@@YAXXZ" = private global [4 x i64]
// MS-DAG: @"__profc_?discarded_access@@YAXXZ" = private global [3 x i64]
// MS-DAG: @"__profc_?constant_access@@YAXXZ" = private global [3 x i64]
// MS: call void @__dyn_tls_on_demand_init()
// MS: dyntls.continue:
// MS-NEXT: {{.*}}load i64, ptr getelementptr inbounds ([3 x i64], ptr @"__profc_?direct_access@@YAXXZ", i32 0, i32 1)

// Disabling guards, or targeting the older compatibility mode, removes the
// hidden on-demand call. The harmless direct-path TLS counter remains so an
// inline function has one layout whether a TU uses a wrapper or direct access.
// MS-NO-GUARDS-DAG: @"__profc_?direct_access@@YAXXZ" = private global [3 x i64]
// MS-NO-GUARDS-DAG: @"__profc_?member_access@@YAXXZ" = private global [4 x i64]
// MS-NO-GUARDS-DAG: @"__profc_?discarded_access@@YAXXZ" = private global [3 x i64]
// MS-NO-GUARDS-NOT: @__dyn_tls_on_demand_init

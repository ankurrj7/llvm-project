// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fcxx-exceptions -fexceptions -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefixes=ON,ITANIUM
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fcxx-exceptions -fexceptions -O2 -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefixes=ON,ITANIUM-O2
// RUN: %clang_cc1 -std=c++20 -triple x86_64-pc-windows-msvc -fcxx-exceptions -fexceptions -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefixes=ON,MS
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fcxx-exceptions -fexceptions -fprofile-instrument=clang -fcoverage-mapping -emit-llvm -o - %s | FileCheck %s --check-prefix=OFF
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fcxx-exceptions -fexceptions -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -dump-coverage-mapping -emit-llvm-only -o - %s | FileCheck %s --check-prefix=MAP

struct Base {
  virtual ~Base();
};

struct Derived : Base {};

extern "C" void *reference_cast(Base *Value) {
  return &dynamic_cast<Derived &>(*Value);
}

extern "C" void *pointer_cast(Base *Value) {
  return dynamic_cast<Derived *>(Value);
}

extern "C" Base *static_reference_upcast(Derived &Value) {
  Base &Reference = dynamic_cast<Base &>(Value);
  return &Reference;
}

// A reference dynamic_cast has a normal continuation only on success. Both
// Itanium optimization levels increment counter #1 after the run-time cast;
// the bad-cast path throws without reaching it.
// ON-DAG: @__profc_reference_cast = private global [2 x i64]
// ON-DAG: @__profc_pointer_cast = private global [1 x i64]
// ON-DAG: @__profc_static_reference_upcast = private global [1 x i64]

// ITANIUM-LABEL: define{{.*}} ptr @reference_cast(
// ITANIUM: call ptr @__dynamic_cast
// ITANIUM: br i1 {{.*}}, label %[[BAD:[^, ]+]], label %[[GOOD:[^, ]+]]
// ITANIUM: [[BAD]]:
// ITANIUM: call void @__cxa_bad_cast
// ITANIUM-NEXT: unreachable
// ITANIUM: [[GOOD]]:
// ITANIUM: load i64, ptr getelementptr inbounds ({{.*}}@__profc_reference_cast{{.*}}i32 1)
// ITANIUM: ret ptr

// ITANIUM-O2-LABEL: define{{.*}} ptr @reference_cast(
// ITANIUM-O2: call ptr @__dynamic_cast
// ITANIUM-O2: br i1 {{.*}}, label %[[BAD_O2:[^, ]+]], label %[[GOOD_O2:[^, ]+]]
// ITANIUM-O2: [[BAD_O2]]:
// ITANIUM-O2: call void @__cxa_bad_cast
// ITANIUM-O2: [[GOOD_O2]]:
// ITANIUM-O2: load i64, ptr getelementptr inbounds nuw (i8, ptr @__profc_reference_cast, i64 8)
// ITANIUM-O2: ret ptr

// A statically resolved reference upcast never reaches EmitDynamicCast, so it
// must not reserve the run-time-cast continuation. Its return remains covered
// by the function-entry counter on both optimization levels.
// ITANIUM-LABEL: define{{.*}} ptr @static_reference_upcast(
// ITANIUM-NOT: call ptr @__dynamic_cast
// ITANIUM: ret ptr
// ITANIUM-O2-LABEL: define{{.*}} ptr @static_reference_upcast(
// ITANIUM-O2-NOT: call ptr @__dynamic_cast
// ITANIUM-O2: ret ptr

// The Microsoft ABI also reserves exactly one reference-cast continuation;
// pointer casts stay excluded on both ABIs.
// MS-LABEL: define{{.*}} ptr @reference_cast(
// MS: call ptr @__RTDynamicCast
// MS: load i64, ptr getelementptr inbounds ({{.*}}@__profc_reference_cast{{.*}}i32 1)
// MS: ret ptr
// MS-LABEL: define{{.*}} ptr @pointer_cast(
// MS: call ptr @__RTDynamicCast
// MS-NOT: getelementptr inbounds ({{.*}}@__profc_pointer_cast{{.*}}i32 1)
// MS: ret ptr
// MS-LABEL: define{{.*}} ptr @static_reference_upcast(
// MS-NOT: call ptr @__RTDynamicCast
// MS: ret ptr

// OFF-DAG: @__profc_reference_cast = private global [1 x i64]
// OFF-DAG: @__profc_pointer_cast = private global [1 x i64]
// OFF-DAG: @__profc_static_reference_upcast = private global [1 x i64]

// The successful reference path owns the return continuation. A pointer cast
// has no such region because null is an ordinary result.
// MAP-LABEL: reference_cast:
// MAP-NEXT:  File 0, [[REF_START:[0-9]+]]:46 -> [[REF_RETURN:[0-9]+]]:42 = #0
// MAP-NEXT:  File 0, [[REF_RETURN]]:3 -> [[REF_END:[0-9]+]]:2 = #1
// MAP-LABEL: pointer_cast:
// MAP-NEXT:  File 0, [[PTR_START:[0-9]+]]:44 -> [[PTR_END:[0-9]+]]:2 = #0
// MAP-NOT:   = #1
// MAP-LABEL: static_reference_upcast:
// MAP-NEXT:  File 0, [[UPCAST_START:[0-9]+]]:58 -> [[UPCAST_END:[0-9]+]]:2 = #0
// MAP-NOT:   = #1

// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefix=EXTERN
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -DDEFINE_TLS -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefix=DEFN

extern thread_local int shared_value;

inline int inline_access() {
  return shared_value;
}

#ifdef DEFINE_TLS
thread_local int shared_value = 1;
#endif

int use_inline() {
  return inline_access();
}

// The same inline/COMDAT function must have the same counter layout in a TU
// that calls the TLS wrapper and a TU that owns the direct TLS definition.
// EXTERN: @__profc__Z13inline_accessv = linkonce_odr hidden global [2 x i64]
// DEFN: @__profc__Z13inline_accessv = linkonce_odr hidden global [2 x i64]

// EXTERN-LABEL: define linkonce_odr{{.*}} @_Z13inline_accessv(
// EXTERN: call ptr @_ZTW12shared_value()
// EXTERN-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z13inline_accessv, i32 0, i32 1)

// DEFN-LABEL: define linkonce_odr{{.*}} @_Z13inline_accessv(
// DEFN-NOT: call ptr @_ZTW12shared_value()
// DEFN: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z13inline_accessv, i32 0, i32 1)

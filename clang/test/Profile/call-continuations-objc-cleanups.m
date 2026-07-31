// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fobjc-runtime=gnustep-2.0 -fobjc-arc -fblocks -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefixes=ON,IR
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fobjc-runtime=gnustep-2.0 -fobjc-arc -fblocks -fprofile-instrument=clang -fcoverage-mapping -emit-llvm -o - %s | FileCheck %s --check-prefix=OFF
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fobjc-runtime=gnustep-2.0 -fobjc-arc -fblocks -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -dump-coverage-mapping -emit-llvm-only -o - %s | FileCheck %s --check-prefix=MAP

typedef void (^Block)(void);

void sink(void);
void use(id);

struct StrongHolder {
  __strong id value;
};

void strong_cleanup(id object) {
  {
    __strong id value = object;
    use(value);
  }
  sink();
}

void weak_cleanup(id object) {
  {
    __weak id value = object;
    use(value);
  }
  sink();
}

void nontrivial_c_struct_cleanup(void) {
  {
    struct StrongHolder holder;
    use(holder.value);
  }
  sink();
}

void escaping_byref_cleanup(Block __strong *output) {
  {
    __block int value = 0;
    *output = ^{ ++value; };
  }
  sink();
}

// ON-DAG: @__profc_strong_cleanup = private global [4 x i64]
// ON-DAG: @__profc_weak_cleanup = private global [5 x i64]
// ON-DAG: @__profc_nontrivial_c_struct_cleanup = private global [5 x i64]
// ON-DAG: @__profc_escaping_byref_cleanup = private global [5 x i64]

// Every non-none destruction kind seeds the source following its nested scope
// only after the hidden cleanup operation completes.
// IR-LABEL: define{{.*}} void @strong_cleanup(
// IR: call void @llvm.objc.storeStrong(ptr %value, ptr null)
// IR-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_strong_cleanup
// MAP-LABEL: strong_cleanup:
// MAP: Gap,File 0, {{[0-9]+}}:4 -> {{[0-9]+}}:3 = #2
// MAP-NEXT: File 0, {{[0-9]+}}:3 -> {{[0-9]+}}:9 = #2

// IR-LABEL: define{{.*}} void @weak_cleanup(
// IR: call void @llvm.objc.destroyWeak(ptr %value)
// IR-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_weak_cleanup
// MAP-LABEL: weak_cleanup:
// MAP: Gap,File 0, {{[0-9]+}}:4 -> {{[0-9]+}}:3 = #3
// MAP-NEXT: File 0, {{[0-9]+}}:3 -> {{[0-9]+}}:9 = #3

// A non-trivial C struct has a declaration boundary after its implicit default
// constructor and a scope boundary after its implicit destructor.
// IR-LABEL: define{{.*}} void @nontrivial_c_struct_cleanup(
// IR: call void @__default_constructor_8_s0
// IR-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_nontrivial_c_struct_cleanup
// IR: call void @__destructor_8_s0
// IR-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_nontrivial_c_struct_cleanup
// MAP-LABEL: nontrivial_c_struct_cleanup:
// MAP: Gap,File 0, {{[0-9]+}}:32 -> {{[0-9]+}}:5 = #1
// MAP: Gap,File 0, {{[0-9]+}}:4 -> {{[0-9]+}}:3 = #3
// MAP-NEXT: File 0, {{[0-9]+}}:3 -> {{[0-9]+}}:9 = #3

// Escaping __block storage is disposed at the scope boundary even when its
// primitive type has no language destructor.
// IR-LABEL: define{{.*}} void @escaping_byref_cleanup(
// IR: call void @_Block_object_dispose
// IR-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc_escaping_byref_cleanup
// MAP-LABEL: escaping_byref_cleanup:
// MAP: Gap,File 0, {{[0-9]+}}:4 -> {{[0-9]+}}:3 = #3
// MAP-NEXT: File 0, {{[0-9]+}}:3 -> {{[0-9]+}}:9 = #3

// OFF-DAG: @__profc_strong_cleanup = private global [1 x i64]
// OFF-DAG: @__profc_weak_cleanup = private global [1 x i64]
// OFF-DAG: @__profc_nontrivial_c_struct_cleanup = private global [1 x i64]
// OFF-DAG: @__profc_escaping_byref_cleanup = private global [1 x i64]

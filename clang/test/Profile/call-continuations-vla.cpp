// RUN: %clang_cc1 -std=gnu++20 -triple x86_64-unknown-linux-gnu -fcxx-exceptions -fexceptions -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefix=IR
// RUN: %clang_cc1 -std=gnu++20 -triple x86_64-unknown-linux-gnu -fcxx-exceptions -fexceptions -fprofile-instrument=clang -fcoverage-mapping -emit-llvm -o - %s | FileCheck %s --check-prefix=NOCC
// RUN: %clang_cc1 -std=gnu++20 -triple x86_64-unknown-linux-gnu -fcxx-exceptions -fexceptions -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -dump-coverage-mapping -emit-llvm-only -o - %s | FileCheck %s --check-prefix=MAP

struct Guard {
  Guard();
  ~Guard();
};

int make_bound();
void *pointer_initializer();

int declaration() {
  int (*pointer)[(Guard(), make_bound())] =
      reinterpret_cast<decltype(pointer)>(pointer_initializer());
  return pointer != nullptr;
}

int alias() {
  using pointer_type = int (*)[(Guard(), make_bound())];
  pointer_type pointer = reinterpret_cast<pointer_type>(pointer_initializer());
  return pointer != nullptr;
}

int explicit_cast() {
  return (int (*)[(Guard(), make_bound())])pointer_initializer() != nullptr;
}

// Keep the exact VLA type-evaluation modeling opt-in.
// NOCC-DAG: @__profc__Z11declarationv = private global [1 x i64]
// NOCC-DAG: @__profc__Z5aliasv = private global [1 x i64]
// NOCC-DAG: @__profc__Z13explicit_castv = private global [1 x i64]

// The temporary constructor and bound call each get their normal call
// continuations. The whole-type completion is emitted only after the bound has
// been converted, and it executes before the declarator initializer.
// IR-LABEL: define{{.*}} i32 @_Z11declarationv(
// IR: call void @_ZN5GuardC1Ev
// IR-NEXT: load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z11declarationv
// IR: invoke{{.*}} i32 @_Z10make_boundv()
// IR: load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z11declarationv
// IR: zext i32
// IR-NEXT: load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z11declarationv
// IR: invoke{{.*}} ptr @_Z19pointer_initializerv()

// A block-scope alias binds its variable extent exactly once. Reusing that
// alias for the initializer type must not evaluate make_bound again.
// IR-LABEL: define{{.*}} i32 @_Z5aliasv(
// IR: invoke{{.*}} i32 @_Z10make_boundv()
// IR-NOT: invoke{{.*}} i32 @_Z10make_boundv()
// IR: invoke{{.*}} ptr @_Z19pointer_initializerv()

// Explicit cast type evaluation also completes before its operand starts.
// IR-LABEL: define{{.*}} i32 @_Z13explicit_castv(
// IR: invoke{{.*}} i32 @_Z10make_boundv()
// IR: zext i32
// IR-NEXT: load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z13explicit_castv
// IR: invoke{{.*}} ptr @_Z19pointer_initializerv()

// The mapping after a type expression uses the whole-type completion rather
// than the entry count. The constructor continuation covers the written bound.
// MAP-LABEL: _Z11declarationv:
// MAP: File 0, [[DECL_LINE:[0-9]+]]:28 -> [[DECL_LINE]]:40 = #1
// MAP-NEXT: File 0, [[DECL_INIT:[0-9]+]]:7 -> [[DECL_INIT]]:64 = #3
// MAP-LABEL: _Z5aliasv:
// MAP: File 0, [[ALIAS_LINE:[0-9]+]]:42 -> [[ALIAS_LINE]]:54 = #1
// MAP: File 0, [[ALIAS_INIT:[0-9]+]]:3 -> [[ALIAS_INIT]]:78 = #3
// MAP-LABEL: _Z13explicit_castv:
// MAP: File 0, [[CAST_LINE:[0-9]+]]:29 -> [[CAST_LINE]]:41 = #1
// MAP-NEXT: File 0, [[CAST_LINE]]:44 -> [[CAST_LINE]]:65 = #3

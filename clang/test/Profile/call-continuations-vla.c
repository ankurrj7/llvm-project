// RUN: %clang_cc1 -std=c2y -triple x86_64-unknown-linux-gnu -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefix=IR
// RUN: %clang_cc1 -std=c2y -triple x86_64-unknown-linux-gnu -mllvm -enable-single-byte-coverage=true -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefix=SB
// RUN: %clang_cc1 -std=c2y -triple x86_64-unknown-linux-gnu -fprofile-instrument=clang -fcoverage-mapping -emit-llvm -o - %s | FileCheck %s --check-prefix=NOCC
// RUN: %clang_cc1 -std=c2y -triple x86_64-unknown-linux-gnu -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -dump-coverage-mapping -emit-llvm-only -o - %s | FileCheck %s --check-prefix=MAP

typedef __builtin_va_list va_list;

int first_bound(int);
int second_bound(int);
int initializer(void);
int (*pointer_initializer(void))[4];

int nested_bounds(int mode) {
  int array[first_bound(mode)][second_bound(mode)];
  int initialized = initializer();
  return initialized + (sizeof(array) != 0);
}

int multiple_declarators(int mode) {
  int first[first_bound(mode)], second[second_bound(mode)];
  return sizeof(first) + sizeof(second);
}

int typedef_cache(int mode) {
  typedef int (*pointer_type)[first_bound(mode)];
  pointer_type pointer = 0;
  return sizeof(*pointer) != 0;
}

int explicit_cast(int mode) {
  return (int (*)[first_bound(mode)])pointer_initializer() != 0;
}

int compound_literal(int mode) {
  return (int (*)[first_bound(mode)]){pointer_initializer()} != 0;
}

int variadic_type(int mode, va_list arguments) {
  return __builtin_va_arg(arguments, int (*)[first_bound(mode)]) != 0;
}

int typeof_expression(int mode, int (*pointer)[4]) {
  (void)(typeof(first_bound(mode),
               (int (*)[second_bound(mode)])pointer)){pointer};
  return pointer != 0;
}

int sizeof_type(int mode) {
  return sizeof(int[first_bound(mode)][second_bound(mode)]) != 0;
}

int countof_variable_outer(int mode) {
  return _Countof(int[first_bound(mode)][7]);
}

int countof_constant_outer(int mode) {
  return _Countof(int[7][first_bound(mode)]);
}

int parameter_reuse(int mode, int (*pointer)[first_bound(mode)]) {
  return sizeof(*pointer) != 0;
}

// Feature-off counter layouts remain unchanged.
// SB-DAG: @__profc_nested_bounds = private global [6 x i8]
// NOCC-DAG: @__profc_nested_bounds = private global [1 x i64]
// NOCC-DAG: @__profc_multiple_declarators = private global [1 x i64]
// NOCC-DAG: @__profc_typedef_cache = private global [1 x i64]
// NOCC-DAG: @__profc_explicit_cast = private global [1 x i64]
// NOCC-DAG: @__profc_compound_literal = private global [1 x i64]
// NOCC-DAG: @__profc_variadic_type = private global [1 x i64]
// NOCC-DAG: @__profc_typeof_expression = private global [1 x i64]
// NOCC-DAG: @__profc_sizeof_type = private global [1 x i64]
// NOCC-DAG: @__profc_countof_variable_outer = private global [1 x i64]
// NOCC-DAG: @__profc_countof_constant_outer = private global [1 x i64]
// NOCC-DAG: @__profc_parameter_reuse = private global [1 x i64]

// The second bound's call continuation is followed by one whole-type
// completion counter before the initializer starts.
// IR-LABEL: define{{.*}} i32 @nested_bounds(
// IR: call i32 @first_bound
// IR-NEXT: load i64, ptr getelementptr inbounds ({{.*}}@__profc_nested_bounds
// IR: call i32 @second_bound
// IR-NEXT: load i64, ptr getelementptr inbounds ({{.*}}@__profc_nested_bounds
// IR: zext i32
// IR-NEXT: load i64, ptr getelementptr inbounds ({{.*}}@__profc_nested_bounds
// IR: call i32 @initializer

// Single-byte coverage uses the same continuation layout and stores a byte
// after each bound and after whole-type completion.
// SB-LABEL: define{{.*}} i32 @nested_bounds(
// SB: call i32 @first_bound
// SB-NEXT: store i8 0, ptr getelementptr inbounds ({{.*}}@__profc_nested_bounds
// SB: call i32 @second_bound
// SB-NEXT: store i8 0, ptr getelementptr inbounds ({{.*}}@__profc_nested_bounds
// SB: zext i32
// SB-NEXT: store i8 0, ptr getelementptr inbounds ({{.*}}@__profc_nested_bounds

// Each declarator owns its own whole-type completion. The second declarator
// starts under the first type's completion and receives a separate phase only
// after its own bound has returned and been converted.
// IR-LABEL: define{{.*}} i32 @multiple_declarators(
// IR: call i32 @first_bound
// IR: zext i32
// IR-NEXT: load i64, ptr getelementptr inbounds ({{.*}}@__profc_multiple_declarators
// IR: call i32 @second_bound
// IR: zext i32
// IR-NEXT: load i64, ptr getelementptr inbounds ({{.*}}@__profc_multiple_declarators

// A typedef binds its VLA size once. Reusing the typedef and querying the
// pointee size must not emit or reserve a second bound evaluation.
// IR-LABEL: define{{.*}} i32 @typedef_cache(
// IR: call i32 @first_bound
// IR-NOT: call i32 @first_bound
// IR: ret i32

// Mapping follows run-time evaluation order. The first bound's continuation
// covers the second bound, and the whole-type completion covers the initializer.
// MAP-LABEL: nested_bounds:
// MAP: File 0, [[NESTED_LINE:[0-9]+]]:32 -> [[NESTED_LINE]]:50 = #1
// MAP-NEXT: Gap,File 0, [[NESTED_LINE]]:52 -> [[NESTED_INIT:[0-9]+]]:3 = #3
// MAP-NEXT: File 0, [[NESTED_INIT]]:3 -> [[NESTED_INIT]]:34 = #3

// MAP-LABEL: multiple_declarators:
// MAP: File 0, [[MULTI_LINE:[0-9]+]]:40 -> [[MULTI_LINE]]:58 = #2

// The cast and compound-literal operands are covered by the type completion,
// not by the function-entry count.
// MAP-LABEL: explicit_cast:
// MAP: File 0, [[CAST_LINE:[0-9]+]]:38 -> [[CAST_LINE]]:59 = #2
// MAP-LABEL: compound_literal:
// MAP: File 0, [[COMPOUND_LINE:[0-9]+]]:38 -> [[COMPOUND_LINE]]:60 = #2

// va_arg's post-type source range and typeof's nested operand use their exact
// completion counters without violating source order.
// MAP-LABEL: variadic_type:
// MAP: Gap,File 0, [[VA_LINE:[0-9]+]]:65 -> [[VA_LINE]]:69 = #2
// MAP-LABEL: typeof_expression:
// MAP: File 0, [[TYPEOF_LINE:[0-9]+]]:25 -> [[TYPEOF_LINE]]:43 = #1
// MAP-NEXT: File 0, [[TYPEOF_LINE]]:45 -> [[TYPEOF_LINE]]:52 = #3
// MAP-NEXT: File 0, [[TYPEOF_LINE]]:54 -> {{[0-9]+}}:2 = #4

// A constant _Countof outer extent does not create hidden evaluated regions.
// Parameter bounds are prologue-only and are likewise excluded from body source
// mapping; counter zero starts at the written body.
// MAP-LABEL: countof_constant_outer:
// MAP-NEXT: File 0, {{[0-9]+}}:38 -> {{[0-9]+}}:2 = #0
// MAP-LABEL: parameter_reuse:
// MAP-NEXT: File 0, {{[0-9]+}}:66 -> {{[0-9]+}}:26 = #0

// Type binding completes before the explicit cast operand is evaluated.
// IR-LABEL: define{{.*}} i32 @explicit_cast(
// IR: call i32 @first_bound
// IR-NEXT: load i64, ptr getelementptr inbounds ({{.*}}@__profc_explicit_cast
// IR: zext i32
// IR-NEXT: load i64, ptr getelementptr inbounds ({{.*}}@__profc_explicit_cast
// IR: call ptr @pointer_initializer

// Type binding also precedes a compound literal's initializer.
// IR-LABEL: define{{.*}} i32 @compound_literal(
// IR: call i32 @first_bound
// IR: zext i32
// IR-NEXT: load i64, ptr getelementptr inbounds ({{.*}}@__profc_compound_literal
// IR: call ptr @pointer_initializer

// va_list evaluation precedes the hidden type evaluation, whose completion
// counter precedes the ABI-specific argument load.
// IR-LABEL: define{{.*}} i32 @variadic_type(
// IR: load ptr, ptr %arguments.addr
// IR: call i32 @first_bound
// IR: zext i32
// IR-NEXT: load i64, ptr getelementptr inbounds ({{.*}}@__profc_variadic_type
// IR: getelementptr inbounds nuw %struct.__va_list_tag

// typeof(expr) evaluates the written expression once. Its nested VM cast gets
// a completion counter, followed by the outer type completion counter.
// IR-LABEL: define{{.*}} i32 @typeof_expression(
// IR: call i32 @first_bound
// IR: call i32 @second_bound
// IR: zext i32
// IR-NEXT: load i64, ptr getelementptr inbounds ({{.*}}@__profc_typeof_expression
// IR: load ptr, ptr %pointer.addr
// IR-NEXT: load i64, ptr getelementptr inbounds ({{.*}}@__profc_typeof_expression

// sizeof(type) evaluates both extents in emitter order and completes only
// after the second one.
// IR-LABEL: define{{.*}} i32 @sizeof_type(
// IR: call i32 @first_bound
// IR: call i32 @second_bound
// IR: mul nuw i64 4
// IR-NEXT: load i64, ptr getelementptr inbounds ({{.*}}@__profc_sizeof_type

// _Countof evaluates a variable outer extent, but not a constant outer extent
// solely because its element type has a variable extent.
// IR-LABEL: define{{.*}} i32 @countof_variable_outer(
// IR: call i32 @first_bound
// IR: ret i32
// IR-LABEL: define{{.*}} i32 @countof_constant_outer(
// IR-NOT: call i32 @first_bound
// IR: ret i32 7

// Parameter bounds are evaluated before ordinary body counter zero. The body
// reuses the cached bound and must not call first_bound a second time.
// IR-LABEL: define{{.*}} i32 @parameter_reuse(
// IR: call i32 @first_bound
// IR-NOT: @__profc_parameter_reuse
// IR: load i64, ptr @__profc_parameter_reuse
// IR-NOT: call i32 @first_bound
// IR: ret i32

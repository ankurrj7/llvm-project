// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefixes=ON,IR
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fprofile-instrument=clang -fcoverage-mapping -emit-llvm -o - %s | FileCheck %s --check-prefix=OFF
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -dump-coverage-mapping -emit-llvm-only -o - %s | FileCheck %s --check-prefix=MAP

void sink(int);
int dynamic_value(int);

void static_multi_declaration() {
  static int first = dynamic_value(1), second = dynamic_value(2);
  sink(first + second);
}

void tls_multi_declaration() {
  thread_local int first = dynamic_value(1), second = dynamic_value(2);
  sink(first + second);
}

struct ConstantDtor {
  int value;
  ~ConstantDtor();
};

void constant_static_declaration() {
  static ConstantDtor value = {7};
  sink(value.value);
}

using size_t = decltype(sizeof(0));

struct TupleLike {
  int first;
  int second;
};

namespace std {
template <class T> struct tuple_size;
template <> struct tuple_size<TupleLike> {
  static constexpr size_t value = 2;
};
template <size_t I, class T> struct tuple_element;
template <size_t I> struct tuple_element<I, TupleLike> {
  using type = int;
};
} // namespace std

template <size_t I> int get(TupleLike &&value) {
  return I == 0 ? value.first : value.second;
}

TupleLike make_tuple();

void tuple_decomposition() {
  auto [first, second] = make_tuple();
  sink(first + second);
}

struct Condition {
  explicit operator bool() const;
};

Condition make_condition();

void condition_declaration() {
  if (Condition value = make_condition())
    sink(1);
  sink(2);
}

void negative_automatic_declarations() {
  int first = 1, second = 2;
  int dynamic = dynamic_value(3);
  sink(first + second + dynamic);
}

struct ArrayBox {
  int values[32];
};

ArrayBox constant_array_expression() {
  return ArrayBox{{0,  1,  2,  3,  4,  5,  6,  7,
                   8,  9,  10, 11, 12, 13, 14, 15,
                   16, 17, 18, 19, 20, 21, 22, 23,
                   24, 25, 26, 27, 28, 29, 30, 31}};
}

// ON-DAG: @__profc__Z24static_multi_declarationv = private global [6 x i64]
// ON-DAG: @__profc__Z21tls_multi_declarationv = private global [6 x i64]
// ON-DAG: @__profc__Z27constant_static_declarationv = private global [3 x i64]
// ON-DAG: @__profc__Z19tuple_decompositionv = private global [4 x i64]
// ON-DAG: @__profc__Z21condition_declarationv = private global [7 x i64]
// ON-DAG: @__profc__Z31negative_automatic_declarationsv = private global [3 x i64]

// Each guarded declarator has an independent completion counter. The first
// one is in the common guard exit block before the second guard is checked, so
// guard retries and later calls cannot bypass the second declarator's source
// count.
// IR-LABEL: define{{.*}} void @_Z24static_multi_declarationv(
// IR: br i1 {{.*}}, label %{{.*}}, label %[[STATIC_FIRST_END:[^, ]+]]
// IR: call{{.*}} i32 @_Z13dynamic_valuei
// IR: call void @__cxa_guard_release
// IR: br label %[[STATIC_FIRST_END]]
// IR: [[STATIC_FIRST_END]]:
// IR-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z24static_multi_declarationv
// IR: store i64
// IR-NEXT: {{.*}}load atomic i8, ptr @_ZGVZ24static_multi_declarationvE6second
// MAP-LABEL: _Z24static_multi_declarationv:
// MAP: File 0, {{[0-9]+}}:49 -> {{[0-9]+}}:65 = #2
// MAP-NEXT: Gap,File 0, {{[0-9]+}}:66 -> {{[0-9]+}}:3 = #4
// MAP-NEXT: File 0, {{[0-9]+}}:3 -> {{[0-9]+}}:23 = #4

// IR-LABEL: define{{.*}} void @_Z21tls_multi_declarationv(
// IR: call{{.*}} i32 @_Z13dynamic_valuei
// IR: load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z21tls_multi_declarationv
// IR: store i64
// IR: call{{.*}} i32 @_Z13dynamic_valuei
// MAP-LABEL: _Z21tls_multi_declarationv:
// MAP: File 0, {{[0-9]+}}:55 -> {{[0-9]+}}:71 = #2
// MAP-NEXT: Gap,File 0, {{[0-9]+}}:72 -> {{[0-9]+}}:3 = #4
// MAP-NEXT: File 0, {{[0-9]+}}:3 -> {{[0-9]+}}:23 = #4

// Constant initialization still needs a declaration boundary when hidden
// guarded destructor registration follows it.
// IR-LABEL: define{{.*}} void @_Z27constant_static_declarationv(
// IR: call i32 @__cxa_atexit
// IR-NEXT: call void @__cxa_guard_release
// IR: load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z27constant_static_declarationv
// MAP-LABEL: _Z27constant_static_declarationv:
// MAP: Gap,File 0, {{[0-9]+}}:35 -> {{[0-9]+}}:3 = #1
// MAP-NEXT: File 0, {{[0-9]+}}:3 -> {{[0-9]+}}:20 = #1

// Tuple-like binding get<I> calls are compiler-generated and finish before the
// declaration counter.
// IR-LABEL: define{{.*}} void @_Z19tuple_decompositionv(
// IR: call i64 @_Z10make_tuplev
// IR: call{{.*}} i32 @_Z3getILm0EEiO9TupleLike
// IR: call{{.*}} i32 @_Z3getILm1EEiO9TupleLike
// IR-NEXT: store i32
// IR-NEXT: load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z19tuple_decompositionv
// MAP-LABEL: _Z19tuple_decompositionv:
// MAP: Gap,File 0, {{[0-9]+}}:39 -> {{[0-9]+}}:3 = #2
// MAP-NEXT: File 0, {{[0-9]+}}:3 -> {{[0-9]+}}:23 = #2

// A condition declaration completes after contextual conversion, immediately
// before the branch.
// IR-LABEL: define{{.*}} void @_Z21condition_declarationv(
// IR: call{{.*}} @_Z14make_conditionv
// IR: call{{.*}} i1 @_ZNK9ConditioncvbEv
// IR: load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z21condition_declarationv
// IR: store i64
// IR-NEXT: {{.*}}load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z21condition_declarationv
// IR: store i64
// IR-NEXT: br i1
// MAP-LABEL: _Z21condition_declarationv:
// MAP: File 0, {{[0-9]+}}:3 -> {{[0-9]+}}:10 = ((#3 + #5) - #1)

// Ordinary constant and automatic initializers do not reserve declaration
// counters. Only the two written calls do.
// OFF-DAG: @__profc__Z31negative_automatic_declarationsv = private global [1 x i64]

// Call-continuation coverage must preserve the constant aggregate memcpy fast
// path instead of expanding the array into per-element stores.
// IR-LABEL: define{{.*}} void @_Z25constant_array_expressionv(
// IR: call void @llvm.memcpy

// The entire feature remains opt-in.
// OFF-DAG: @__profc__Z24static_multi_declarationv = private global [1 x i64]
// OFF-DAG: @__profc__Z21tls_multi_declarationv = private global [1 x i64]
// OFF-DAG: @__profc__Z27constant_static_declarationv = private global [1 x i64]
// OFF-DAG: @__profc__Z19tuple_decompositionv = private global [1 x i64]
// OFF-DAG: @__profc__Z21condition_declarationv = private global [2 x i64]
// OFF-DAG: @__profc__Z25constant_array_expressionv = private global [1 x i64]

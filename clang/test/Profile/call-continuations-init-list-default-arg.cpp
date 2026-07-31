// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fcxx-exceptions -fexceptions -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefix=IR
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fcxx-exceptions -fexceptions -fprofile-instrument=clang -fcoverage-mapping -emit-llvm -o - %s | FileCheck %s --check-prefix=OFF-IR
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fcxx-exceptions -fexceptions -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -dump-coverage-mapping -emit-llvm-only -o - %s | FileCheck %s --check-prefix=MAP
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fcxx-exceptions -fexceptions -fprofile-instrument=clang -fcoverage-mapping -dump-coverage-mapping -emit-llvm-only -o - %s | FileCheck %s --check-prefix=OFF-MAP

namespace std {
using size_t = decltype(sizeof(0));
template <class T> class initializer_list {
  const T *Begin;
  size_t Size;
};
} // namespace std

struct Element {
  Element();
  ~Element();
};

void sink();

void initializer_list_case() {
  std::initializer_list<Element> values = {{}, {}};
  sink();
}

struct Aggregate {
  Element First;
  Element Second;
};

void aggregate_list_case() {
  Aggregate value = {{}, {}};
  sink();
}

struct DefaultOwner {
  DefaultOwner();
  ~DefaultOwner();
};

void consume(DefaultOwner value = DefaultOwner());

void default_argument_case() {
  consume();
  sink();
}

// The semantic form of each initializer list contains implicit constructor and
// destructor nodes that are absent from the written form. They must be visited
// exactly once by the structural hash walk. These checks both guard against the
// previous missing-owner assertion and pin the complete counter layouts.
// IR-DAG: @__profc__Z21initializer_list_casev = private global [5 x i64]
// IR-DAG: @__profc__Z19aggregate_list_casev = private global [4 x i64]
// OFF-IR-DAG: @__profc__Z21initializer_list_casev = private global [1 x i64]
// OFF-IR-DAG: @__profc__Z19aggregate_list_casev = private global [1 x i64]

// A CXXDefaultArgExpr executes at the caller use. Its declaration-owned source
// expression must remain opaque to caller mapping; the post-call region starts
// at consume() and owns the following sink() only after all temporary cleanup
// completes.
// IR-DAG: @__profc__Z21default_argument_casev = private global [5 x i64]
// OFF-IR-DAG: @__profc__Z21default_argument_casev = private global [1 x i64]
// MAP-LABEL: _Z21default_argument_casev:
// MAP-NEXT:  File 0, [[BODY_LINE:[0-9]+]]:30 -> [[CALL_LINE:[0-9]+]]:3 = #0
// MAP-NEXT:  Gap,File 0, [[CALL_LINE]]:13 -> [[SINK_LINE:[0-9]+]]:3 = #3
// MAP-NEXT:  File 0, [[SINK_LINE]]:3 -> [[SINK_LINE]]:9 = #3
// OFF-MAP-LABEL: _Z21default_argument_casev:
// OFF-MAP-NEXT:  File 0, [[OFF_BODY_LINE:[0-9]+]]:30 -> [[OFF_END_LINE:[0-9]+]]:2 = #0

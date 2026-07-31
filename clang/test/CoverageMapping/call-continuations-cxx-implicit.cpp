// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -dump-coverage-mapping -emit-llvm-only -o - %s | FileCheck %s --check-prefix=MAP

void sink();

struct Automatic {
  Automatic();
  ~Automatic();
};

Automatic make_automatic();

void automatic_nested_scope() {
  {
    Automatic value;
  }
  sink();
}

void full_expression_temporary() {
  make_automatic();
  sink();
}

void delete_expression(Automatic *p) {
  delete p;
  sink();
}

Automatic *new_expression() {
  Automatic *p = new Automatic;
  sink();
  return p;
}

struct Base {
  Base();
};

struct Member {
  Member();
};

int make_initial_value();

struct Complete : Base {
  Member member;
  int value = make_initial_value();
  Complete();
};

Complete::Complete() {
  sink();
}

template <bool Enabled>
void discarded_constexpr_branch() {
  if constexpr (Enabled) {
    sink();
  } else {
    Automatic discarded;
    sink();
  }
  sink();
}

template void discarded_constexpr_branch<true>();

// Destruction occurs at the closing brace. The gap and the following call must
// use the post-cleanup counter (#2), not the constructor counter (#1).
// MAP-LABEL: _Z22automatic_nested_scopev:
// MAP: Gap,File 0, [[AUTO_CLOSE:[0-9]+]]:4 -> [[AUTO_SINK:[0-9]+]]:3 = #2
// MAP-NEXT: File 0, [[AUTO_SINK]]:3 -> [[AUTO_SINK]]:9 = #2

// The call that creates the temporary has counter #1. The full-expression
// cleanup has counter #2 and controls the next source region.
// MAP-LABEL: _Z25full_expression_temporaryv:
// MAP: Gap,File 0, [[TEMP_END:[0-9]+]]:20 -> [[TEMP_SINK:[0-9]+]]:3 = #2
// MAP-NEXT: File 0, [[TEMP_SINK]]:3 -> [[TEMP_SINK]]:9 = #2

// A delete expression owns one continuation covering successful null and
// non-null completion. The following source must not remain under #0.
// MAP-LABEL: _Z17delete_expressionP9Automatic:
// MAP: Gap,File 0, [[DELETE_END:[0-9]+]]:12 -> [[DELETE_SINK:[0-9]+]]:3 = #1
// MAP-NEXT: File 0, [[DELETE_SINK]]:3 -> [[DELETE_SINK]]:9 = #1

// Allocation completion is counter #1, construction is #2, and the complete
// new-expression is #3. The following call then advances coverage to #4.
// MAP-LABEL: _Z14new_expressionv:
// MAP: Gap,File 0, [[NEW_END:[0-9]+]]:32 -> [[NEW_SINK:[0-9]+]]:3 = #3
// MAP-NEXT: File 0, [[NEW_SINK]]:3 -> [[NEW_SINK]]:9 = #3
// MAP-NEXT: Gap,File 0, [[NEW_SINK]]:10 -> [[NEW_RETURN:[0-9]+]]:3 = #4
// MAP-NEXT: File 0, [[NEW_RETURN]]:3 -> [[NEW_RETURN]]:11 = #4

// The written constructor body is seeded by a counter reached only after the
// implicit base, member, and default-member initializers all return.
// MAP-LABEL: _ZN8CompleteC2Ev:
// MAP: File 0, [[CTOR_BODY:[0-9]+]]:22 -> [[CTOR_SINK:[0-9]+]]:9 = #[[PROLOGUE:[1-9][0-9]*]]

// The non-selected constexpr arm remains skipped and cannot contribute an
// implicit cleanup continuation to the selected specialization.
// MAP-LABEL: _Z26discarded_constexpr_branchILb1EEvv:
// MAP: Skipped,File 0, [[ELSE_START:[0-9]+]]:4 -> [[ELSE_END:[0-9]+]]:4 = 0
// MAP: Gap,File 0, [[ELSE_END]]:4 -> [[AFTER_IF:[0-9]+]]:3 = #2

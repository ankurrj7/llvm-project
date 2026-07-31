// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fcxx-exceptions -fexceptions -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -dump-coverage-mapping -emit-llvm-only -o - %s | FileCheck %s --check-prefix=ON
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fcxx-exceptions -fexceptions -O2 -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -dump-coverage-mapping -emit-llvm-only -o - %s | FileCheck %s --check-prefix=ON
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fcxx-exceptions -fexceptions -fprofile-instrument=clang -fcoverage-mapping -dump-coverage-mapping -emit-llvm-only -o - %s | FileCheck %s --check-prefix=OFF

void sink(int);
int call(bool);

struct TrivialCondition {
  explicit operator bool() const;
};

TrivialCondition make_trivial(bool);
TrivialCondition stop_condition() __attribute__((noreturn));

void if_no_else(bool Value) {
  if (TrivialCondition Condition = make_trivial(Value)) {
    volatile int Then = 1;
    (void)Then;
  }
  volatile int After = 2;
  (void)After;
}

void if_else(bool Value) {
  if (TrivialCondition Condition = make_trivial(Value)) {
    volatile int Then = 1;
    (void)Then;
  } else {
    volatile int Else = 2;
    (void)Else;
  }
  volatile int After = 3;
  (void)After;
}

void if_noreturn() {
  if (TrivialCondition Condition = stop_condition()) {
    volatile int Then = 1;
    (void)Then;
  }
  volatile int After = 2;
  (void)After;
}

struct CleanupCondition {
  CleanupCondition();
  ~CleanupCondition();
  explicit operator bool() const;
};

CleanupCondition make_cleanup(bool);

void if_cleanup(bool Value) {
  if (CleanupCondition Condition = make_cleanup(Value)) {
    volatile int Then = 1;
    (void)Then;
  } else {
    volatile int Else = 2;
    (void)Else;
  }
  volatile int After = 3;
  (void)After;
}

void init_list_first(bool Stop) {
  int Values[] = {call(Stop), 2, 3};
  sink(Values[0]);
}

void init_list_middle(bool Stop) {
  int Values[] = {1, call(Stop), 3};
  sink(Values[1]);
}

void init_list_last(bool Stop) {
  int Values[] = {1, 2, call(Stop)};
  sink(Values[2]);
}

struct Pair {
  int First;
  int Second;
};

void init_list_nested(bool Stop) {
  Pair Values[] = {{call(Stop), 2}, {3, 4}};
  sink(Values[0].First);
}

// A condition-declaration continuation is the parent of the branch, but it
// must not become an open source region spanning the selected body. The
// post-if region is compared with the original function count and therefore
// becomes #3 even when both arms complete normally.
// ON-LABEL: _Z10if_no_elseb:
// ON-NEXT:  File 0, [[NOELSE_START:[0-9]+]]:29 -> [[NOELSE_END:[0-9]+]]:2 = #0
// ON-NEXT:  File 0, [[NOELSE_COND:[0-9]+]]:7 -> [[NOELSE_COND]]:55 = #0
// ON-NEXT:  File 0, [[NOELSE_COND]]:24 -> [[NOELSE_COND]]:33 = #2
// ON-NEXT:  Branch,File 0, [[NOELSE_COND]]:24 -> [[NOELSE_COND]]:33 = #1, (#3 - #1)
// ON-NEXT:  Gap,File 0, [[NOELSE_COND]]:56 -> [[NOELSE_COND]]:57 = #1
// ON-NEXT:  File 0, [[NOELSE_COND]]:57 -> [[NOELSE_BODY_END:[0-9]+]]:4 = #1
// ON-NEXT:  Gap,File 0, [[NOELSE_BODY_END]]:4 -> [[NOELSE_AFTER:[0-9]+]]:3 = #3
// ON-NEXT:  File 0, [[NOELSE_AFTER]]:3 -> [[NOELSE_END]]:2 = #3

// The explicit else has one region per arm and one post-if region. In
// particular, there is no second then-body region owned by declaration #3.
// ON-LABEL: _Z7if_elseb:
// ON-NEXT:  File 0, [[ELSE_START:[0-9]+]]:26 -> [[ELSE_END:[0-9]+]]:2 = #0
// ON-NEXT:  File 0, [[ELSE_COND:[0-9]+]]:7 -> [[ELSE_COND]]:55 = #0
// ON-NEXT:  File 0, [[ELSE_COND]]:24 -> [[ELSE_COND]]:33 = #2
// ON-NEXT:  Branch,File 0, [[ELSE_COND]]:24 -> [[ELSE_COND]]:33 = #1, (#3 - #1)
// ON-NEXT:  Gap,File 0, [[ELSE_COND]]:56 -> [[ELSE_COND]]:57 = #1
// ON-NEXT:  File 0, [[ELSE_COND]]:57 -> [[THEN_END:[0-9]+]]:4 = #1
// ON-NEXT:  Gap,File 0, [[THEN_END]]:4 -> [[ELSE_BODY:[0-9]+]]:10 = (#3 - #1)
// ON-NEXT:  File 0, [[ELSE_BODY]]:10 -> [[ELSE_BODY_END:[0-9]+]]:4 = (#3 - #1)
// ON-NEXT:  Gap,File 0, [[ELSE_BODY_END]]:4 -> [[ELSE_AFTER:[0-9]+]]:3 = #3
// ON-NEXT:  File 0, [[ELSE_AFTER]]:3 -> [[ELSE_END]]:2 = #3

// A statically noreturn initializer never reaches declaration or branch
// continuation counters. The mapped post-if expression is consequently zero
// at run time, rather than inheriting function-entry #0.
// ON-LABEL: _Z11if_noreturnv:
// ON:       File 0, [[NORETURN_COND:[0-9]+]]:24 -> [[NORETURN_COND]]:33 = 0
// ON:       Gap,File 0, [[NORETURN_BODY_END:[0-9]+]]:4 -> [[NORETURN_AFTER:[0-9]+]]:3 = #2
// ON-NEXT:  File 0, [[NORETURN_AFTER]]:3 -> {{[0-9]+}}:2 = #2

// The condition object's destructor runs after the selected arm. Its IfExit
// counter (#6), not the branch-parent counter (#4), owns source after the if.
// ON-LABEL: _Z10if_cleanupb:
// ON:       Branch,File 0, [[CLEANUP_COND:[0-9]+]]:24 -> [[CLEANUP_COND]]:33 = #1, (#4 - #1)
// ON:       Gap,File 0, [[CLEANUP_BODY_END:[0-9]+]]:4 -> [[CLEANUP_AFTER:[0-9]+]]:3 = #6
// ON-NEXT:  File 0, [[CLEANUP_AFTER]]:3 -> {{[0-9]+}}:2 = #6

// A returned call in the first, middle, or last initializer position must own
// the following statement. Nested aggregate lists must preserve the same gap
// counter through every enclosing VisitStmt.
// ON-LABEL: _Z15init_list_firstb:
// ON-NEXT:  File 0, [[FIRST_START:[0-9]+]]:33 -> [[FIRST_INIT:[0-9]+]]:29 = #0
// ON-NEXT:  Gap,File 0, [[FIRST_INIT]]:30 -> [[FIRST_INIT]]:31 = #1
// ON-NEXT:  File 0, [[FIRST_INIT]]:31 -> [[FIRST_SINK:[0-9]+]]:18 = #1
// ON-NEXT:  Gap,File 0, [[FIRST_INIT]]:37 -> [[FIRST_SINK]]:3 = #1
// ON-LABEL: _Z16init_list_middleb:
// ON-NEXT:  File 0, [[MIDDLE_START:[0-9]+]]:34 -> [[MIDDLE_INIT:[0-9]+]]:32 = #0
// ON-NEXT:  Gap,File 0, [[MIDDLE_INIT]]:33 -> [[MIDDLE_INIT]]:34 = #1
// ON-NEXT:  File 0, [[MIDDLE_INIT]]:34 -> [[MIDDLE_SINK:[0-9]+]]:18 = #1
// ON-NEXT:  Gap,File 0, [[MIDDLE_INIT]]:37 -> [[MIDDLE_SINK]]:3 = #1
// ON-LABEL: _Z14init_list_lastb:
// ON-NEXT:  File 0, [[LAST_START:[0-9]+]]:32 -> [[LAST_INIT:[0-9]+]]:35 = #0
// ON-NEXT:  Gap,File 0, [[LAST_INIT]]:37 -> [[LAST_SINK:[0-9]+]]:3 = #1
// ON-NEXT:  File 0, [[LAST_SINK]]:3 -> [[LAST_SINK]]:18 = #1
// ON-LABEL: _Z16init_list_nestedb:
// ON-NEXT:  File 0, [[NESTED_START:[0-9]+]]:34 -> [[NESTED_INIT:[0-9]+]]:31 = #0
// ON-NEXT:  Gap,File 0, [[NESTED_INIT]]:32 -> [[NESTED_INIT]]:33 = #1
// ON-NEXT:  File 0, [[NESTED_INIT]]:33 -> [[NESTED_SINK:[0-9]+]]:24 = #1
// ON-NEXT:  Gap,File 0, [[NESTED_INIT]]:36 -> [[NESTED_INIT]]:37 = #1
// ON-NEXT:  Gap,File 0, [[NESTED_INIT]]:45 -> [[NESTED_SINK]]:3 = #1

// Feature-off mapping remains the original single root region for all four
// initializer-list functions; no continuation or gap region is introduced.
// OFF-LABEL: _Z15init_list_firstb:
// OFF-NEXT:  File 0, [[OFF_FIRST_START:[0-9]+]]:33 -> [[OFF_FIRST_END:[0-9]+]]:2 = #0
// OFF-NEXT: _Z16init_list_middleb:
// OFF-NEXT:  File 0, [[OFF_MIDDLE_START:[0-9]+]]:34 -> [[OFF_MIDDLE_END:[0-9]+]]:2 = #0
// OFF-NEXT: _Z14init_list_lastb:
// OFF-NEXT:  File 0, [[OFF_LAST_START:[0-9]+]]:32 -> [[OFF_LAST_END:[0-9]+]]:2 = #0
// OFF-NEXT: _Z16init_list_nestedb:
// OFF-NEXT:  File 0, [[OFF_NESTED_START:[0-9]+]]:34 -> [[OFF_NESTED_END:[0-9]+]]:2 = #0

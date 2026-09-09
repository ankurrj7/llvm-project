// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -dump-coverage-mapping -emit-llvm-only -o - %s | FileCheck %s --check-prefix=MAP
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-obj -o %t.o %s
// RUN: llvm-cov export %t.o --empty-profile > /dev/null

struct Widget {
  Widget(int, void * = nullptr);
};

Widget *new_with_default_arg(int value) {
  return new Widget(value);
}

struct StepName {
  StepName(const char *);
};

int begin_step(const StepName &, int = 0, int = 1, int = 2);
void finish_step(int);

void declaration_initializer() {
  int step =
      begin_step("declaration",
                 2,
                 3);
  finish_step(step);
}

struct Resource {
  Resource() {
    int step = begin_step(
        "constructor",
        2,
        3);
    finish_step(step);
  }
};

Resource *make_resource() {
  return new Resource;
}

// An omitted default argument has no caller-side spelling. Keep its completion
// count as flow state while mapping each written operand in its own source
// range. This prevents a later-written argument from being ended at the
// default argument's earlier use location.
//
// MAP-LABEL: _Z20new_with_default_argi:
// MAP: File 0, [[NEW_LINE:[0-9]+]]:21 -> [[NEW_LINE]]:26 = #1
// MAP-LABEL: _Z23declaration_initializerv:
// MAP: File 0, [[DECL_ARG:[0-9]+]]:18 -> [[DECL_ARG]]:31 = #0
// MAP: File 0, [[DECL_END:[0-9]+]]:18 -> [[DECL_END]]:19 = #2
// MAP-LABEL: _ZN8ResourceC2Ev:
// MAP: File 0, [[CTOR_ARG:[0-9]+]]:9 -> [[CTOR_ARG]]:22 = #1
// MAP: File 0, [[CTOR_END:[0-9]+]]:9 -> [[CTOR_END]]:10 = #3

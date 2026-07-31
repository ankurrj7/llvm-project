// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fobjc-runtime=gnustep-2.0 -fobjc-arc -fblocks -fexceptions -fcxx-exceptions -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -dump-coverage-mapping -emit-llvm-only -o - %s | FileCheck %s --check-prefix=MAP

typedef int (^IntBlock)(void);

void sink(int);

struct Capture {
  int value;
  Capture(const Capture &);
  ~Capture() noexcept(false);
};

void block_comma(Capture captured) {
  ((void)^{ return captured.value; }, sink(1));
  sink(2);
}

void block_scope(Capture captured) {
  {
    IntBlock block = ^{ return captured.value; };
    (void)block;
    sink(3);
  }
  sink(4);
}

__attribute__((objc_root_class))
@interface Root
@end

void strong_assignment(__strong Root **dst, Root *src) {
  (*dst = src, sink(5));
}

void weak_assignment(__weak Root **dst, Root *src) {
  (*dst = src, sink(6));
}

Root * __strong &strong_assignment_lvalue(Root * __strong &dst, Root *src) {
  return (dst = src);
}

struct CXXAssignment {
  CXXAssignment &operator=(const CXXAssignment &);
};

void cxx_assignment(CXXAssignment &dst, const CXXAssignment &src) {
  (dst = src, sink(7));
}

// A comma RHS is controlled by successful completion of hidden block-capture
// construction (#1). The following statement is reached only after the
// full-expression capture cleanup (#3).
// MAP-LABEL: _Z11block_comma7Capture:
// MAP: Gap,File 0, [[BLOCK_END:[0-9]+]]:38 -> [[COMMA_SINK:[0-9]+]]:39 = #1
// MAP-NEXT: File 0, [[COMMA_SINK]]:39 -> [[COMMA_SINK]]:46 = #1
// MAP: Gap,File 0, [[COMMA_EXPR_END:[0-9]+]]:48 -> [[AFTER_COMMA:[0-9]+]]:3 = #3
// MAP-NEXT: File 0, [[AFTER_COMMA]]:3 -> [[AFTER_COMMA]]:10 = #3

// A named local block extends its direct capture to the nested scope. Source
// after that scope is controlled by the post-destruction counter (#4).
// MAP-LABEL: _Z11block_scope7Capture:
// MAP: Gap,File 0, [[DECL_END:[0-9]+]]:50 -> [[SCOPE_BODY:[0-9]+]]:5 = #2
// MAP-NEXT: File 0, [[SCOPE_BODY]]:5 -> [[SCOPE_SINK:[0-9]+]]:12 = #2
// MAP: Gap,File 0, [[SCOPE_END:[0-9]+]]:4 -> [[AFTER_SCOPE:[0-9]+]]:3 = #4
// MAP-NEXT: File 0, [[AFTER_SCOPE]]:3 -> [[AFTER_SCOPE]]:10 = #4

// ARC strong and weak assignments both seed their comma RHS from Assignment
// counter #1.
// MAP-LABEL: _Z17strong_assignmentPU8__strongP4RootS0_:
// MAP: Gap,File 0, [[STRONG_END:[0-9]+]]:15 -> [[STRONG_SINK:[0-9]+]]:16 = #1
// MAP-NEXT: File 0, [[STRONG_SINK]]:16 -> [[STRONG_SINK]]:23 = #1

// MAP-LABEL: _Z15weak_assignmentPU6__weakP4RootS0_:
// MAP: Gap,File 0, [[WEAK_END:[0-9]+]]:15 -> [[WEAK_SINK:[0-9]+]]:16 = #1
// MAP-NEXT: File 0, [[WEAK_SINK]]:16 -> [[WEAK_SINK]]:23 = #1

// Assignment emitted as an lvalue controls the return region.
// MAP-LABEL: _Z24strong_assignment_lvalueRU8__strongP4RootS0_:
// MAP: File 0, [[RETURN_LINE:[0-9]+]]:3 -> [[RETURN_END:[0-9]+]]:2 = #1

// Overloaded C++ operator= has only its existing normal Call continuation.
// MAP-LABEL: _Z14cxx_assignmentR13CXXAssignmentRKS_:
// MAP: Gap,File 0, [[CXX_END:[0-9]+]]:14 -> [[CXX_SINK:[0-9]+]]:15 = #1
// MAP-NEXT: File 0, [[CXX_SINK]]:15 -> [[CXX_SINK]]:22 = #1

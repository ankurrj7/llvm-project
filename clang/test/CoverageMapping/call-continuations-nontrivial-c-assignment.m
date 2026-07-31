// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fobjc-runtime=gnustep-2.0 -fobjc-arc -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -dump-coverage-mapping -emit-llvm-only -o - %s | FileCheck %s --check-prefix=MAP

__attribute__((objc_root_class))
@interface Root
@end

typedef struct {
  __strong Root *value;
} StrongHolder;

typedef struct {
  __weak Root *value;
} WeakHolder;

void after_assignment(int);
int make_argument(void);
void use_ordered(int, int);

void strong_struct(StrongHolder *dst, StrongHolder *src) {
  (*dst = *src, after_assignment(1));
}

void weak_struct(WeakHolder *dst, WeakHolder *src) {
  (*dst = *src, after_assignment(2));
}

void strong_struct_standalone(StrongHolder *dst, StrongHolder *src) {
  *dst = *src;
  after_assignment(3);
}

void strong_chained(StrongHolder *first, StrongHolder *second,
                    StrongHolder *third) {
  (*first = *second = *third, after_assignment(4));
}

void ordered_assignment(StrongHolder *dst, StrongHolder *src) {
  use_ordered(((*dst = *src), 1), make_argument());
}

// A non-trivial C struct assignment controls the comma RHS directly, before
// the enclosing full-expression cleanup.
// MAP-LABEL: strong_struct:
// MAP: Gap,File 0, [[STRONG_END:[0-9]+]]:16 -> [[STRONG_CALL:[0-9]+]]:17 = #1
// MAP-NEXT: File 0, [[STRONG_CALL]]:17 -> [[STRONG_CALL]]:36 = #1

// MAP-LABEL: weak_struct:
// MAP: Gap,File 0, [[WEAK_END:[0-9]+]]:16 -> [[WEAK_CALL:[0-9]+]]:17 = #1
// MAP-NEXT: File 0, [[WEAK_CALL]]:17 -> [[WEAK_CALL]]:36 = #1

// For a standalone assignment, its counter is followed by the full-expression
// completion counter (#2), which controls the next statement.
// MAP-LABEL: strong_struct_standalone:
// MAP: Gap,File 0, [[STANDALONE_END:[0-9]+]]:15 -> [[STANDALONE_CALL:[0-9]+]]:3 = #2
// MAP-NEXT: File 0, [[STANDALONE_CALL]]:3 -> [[STANDALONE_CALL]]:22 = #2

// Nested assignment completion is allocated inside-out; the comma RHS is
// reached only after outer assignment counter #2.
// MAP-LABEL: strong_chained:
// MAP: Gap,File 0, [[CHAIN_END:[0-9]+]]:30 -> [[CHAIN_CALL:[0-9]+]]:31 = #2
// MAP-NEXT: File 0, [[CHAIN_CALL]]:31 -> [[CHAIN_CALL]]:50 = #2

// Call-operand traversal preserves the assignment edge and target evaluation
// order. On this target the source after the first operand uses counter #2.
// MAP-LABEL: ordered_assignment:
// MAP: Gap,File 0, [[ORDERED_END:[0-9]+]]:30 -> [[ORDERED_VALUE:[0-9]+]]:31 = #2
// MAP-NEXT: File 0, [[ORDERED_VALUE]]:31 -> [[ORDERED_VALUE]]:50 = #2

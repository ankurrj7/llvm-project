// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fblocks -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -dump-coverage-mapping -emit-llvm-only -o - %s | FileCheck %s --check-prefix=MAP

typedef struct {
  long values[3];
} Big;
typedef _Atomic(Big) AtomicBig;
typedef _Atomic(__int128) AtomicI128;
typedef _Atomic(_Complex double) AtomicComplex;

AtomicBig *atomic_destination(void);
Big make_atomic_value(void);
AtomicI128 *i128_destination(void);
__int128 make_i128(void);
AtomicComplex *complex_destination(void);
_Complex double make_complex(void);
void lhs_side_effect(void);
void after_assignment(int);

void atomic_assignment(void) {
  (*atomic_destination() =
       make_atomic_value(),
   after_assignment(1));
}

void block_atomic_assignment(void) {
  __block AtomicBig value;
  (*(lhs_side_effect(), &value) =
       make_atomic_value(),
   after_assignment(2));
}

void i128_compound_assignment(void) {
  (*i128_destination() +=
       make_i128(),
   after_assignment(3));
}

void complex_atomic_assignment(void) {
  (*complex_destination() =
       make_complex(),
   after_assignment(4));
}

void complex_compound_assignment(void) {
  (*complex_destination() +=
       make_complex(),
   after_assignment(5));
}

// The ordinary atomic destination call is counter #1, the RHS call is #2,
// and the comma RHS is controlled by assignment completion counter #3.
// MAP-LABEL: atomic_assignment:
// MAP: Gap,File 0, [[ATOMIC_RHS_END:[0-9]+]]:28 -> [[ATOMIC_AFTER:[0-9]+]]:4 = #3
// MAP-NEXT: File 0, [[ATOMIC_AFTER]]:4 -> [[ATOMIC_AFTER]]:23 = #3

// The side-effecting __block case evaluates the RHS first (#1) and its LHS
// call second (#2); assignment completion controls the comma RHS (#3).
// MAP-LABEL: block_atomic_assignment:
// MAP: Gap,File 0, [[BLOCK_RHS_END:[0-9]+]]:28 -> [[BLOCK_AFTER:[0-9]+]]:4 = #3
// MAP-NEXT: File 0, [[BLOCK_AFTER]]:4 -> [[BLOCK_AFTER]]:23 = #3

// Compound assignments and non-aggregate atomic assignments evaluate the RHS
// call first (#1), the LHS call second (#2), then use assignment completion
// counter #3 for the comma RHS.
// MAP-LABEL: i128_compound_assignment:
// MAP: Gap,File 0, [[I128_RHS_END:[0-9]+]]:{{[0-9]+}} -> [[I128_AFTER:[0-9]+]]:4 = #3
// MAP-NEXT: File 0, [[I128_AFTER]]:4 -> [[I128_AFTER]]:23 = #3

// MAP-LABEL: complex_atomic_assignment:
// MAP: Gap,File 0, [[COMPLEX_RHS_END:[0-9]+]]:{{[0-9]+}} -> [[COMPLEX_AFTER:[0-9]+]]:4 = #3
// MAP-NEXT: File 0, [[COMPLEX_AFTER]]:4 -> [[COMPLEX_AFTER]]:23 = #3

// MAP-LABEL: complex_compound_assignment:
// MAP: Gap,File 0, [[COMPLEX_COMPOUND_RHS_END:[0-9]+]]:{{[0-9]+}} -> [[COMPLEX_COMPOUND_AFTER:[0-9]+]]:4 = #3
// MAP-NEXT: File 0, [[COMPLEX_COMPOUND_AFTER]]:4 -> [[COMPLEX_COMPOUND_AFTER]]:23 = #3

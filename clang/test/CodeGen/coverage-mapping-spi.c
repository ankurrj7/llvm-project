// RUN: rm -f %t.o %t.spi
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-obj \
// RUN:   -fprofile-instrument=clang -fcoverage-mapping \
// RUN:   -fcoverage-mapping-spi=%t.spi -fcoverage-mapping-spi-key=stable \
// RUN:   -o %t.o %s
// RUN: test -s %t.o
// RUN: test -s %t.spi
// RUN: llvm-profdata merge %S/Inputs/coverage-mapping-spi.proftext \
// RUN:   -o %t.profdata
// RUN: llvm-cov export %t.spi --txtcvrgfull \
// RUN:   | FileCheck %s --check-prefix=SPI-BASELINE
// RUN: llvm-cov export %t.spi --txtcvrg -instr-profile=%t.profdata \
// RUN:   | FileCheck %s --check-prefix=SPI-EXECUTION
// RUN: rm -f %t.o %t.spi
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-obj \
// RUN:   -DFAIL -fprofile-instrument=clang -fcoverage-mapping \
// RUN:   -fcoverage-mapping-spi=%t.spi -fcoverage-mapping-spi-key=stable \
// RUN:   -o %t.o %s
// RUN: test ! -e %t.spi
// RUN: rm -f %t.latest.spi %t.first.o %t.second.o
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-obj -DFIRST \
// RUN:   -fprofile-instrument=clang -fcoverage-mapping \
// RUN:   -fcoverage-mapping-spi=%t.latest.spi \
// RUN:   -fcoverage-mapping-spi-key=stable -o %t.first.o %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-obj -DSECOND \
// RUN:   -fprofile-instrument=clang -fcoverage-mapping \
// RUN:   -fcoverage-mapping-spi=%t.latest.spi \
// RUN:   -fcoverage-mapping-spi-key=stable -o %t.second.o %s
// RUN: llvm-cov export %t.latest.spi -empty-profile \
// RUN:   | FileCheck %s --check-prefix=LATEST

// LATEST-NOT: first_only
// LATEST: second_only

// SPI-BASELINE:      txtcvrg 3 baseline branches=0 mcdc=0
// SPI-BASELINE:      function "main" 1 0 0.00
// SPI-BASELINE-NEXT: 1 53.16 53.29 0

// SPI-EXECUTION:      txtcvrg 3 execution branches=0 mcdc=0
// SPI-EXECUTION:      function "main" 1 1 100.00
// SPI-EXECUTION-NEXT: 1 53.16 53.29 1

#ifdef FAIL
#error expected failure
#endif

#if defined(FIRST)
static int first_only(void) { return 1; }
#elif defined(SECOND)
static int second_only(void) { return 2; }
#endif

int main(void) { return 0; }

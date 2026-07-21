// RUN: rm -f %t.o %t.spi
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-obj \
// RUN:   -fprofile-instrument=clang -fcoverage-mapping \
// RUN:   -fcoverage-mapping-spi=%t.spi -fcoverage-mapping-spi-key=stable \\
// RUN:   -o %t.o %s
// RUN: test -s %t.o
// RUN: test -s %t.spi
// RUN: llvm-profdata merge %S/Inputs/coverage-mapping-spi.proftext \
// RUN:   -o %t.profdata
// RUN: llvm-cov export %t.spi -empty-profile -format=covered-functions \
// RUN:   | FileCheck %s --check-prefix=SPI-BASELINE
// RUN: llvm-cov export %t.spi -coverage-only -format=covered-functions \
// RUN:   -instr-profile=%t.profdata \
// RUN:   | FileCheck %s --check-prefix=SPI-EXECUTION
// RUN: rm -f %t.o %t.spi
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-obj \
// RUN:   -DFAIL -fprofile-instrument=clang -fcoverage-mapping \
// RUN:   -fcoverage-mapping-spi=%t.spi -fcoverage-mapping-spi-key=stable \\
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

// SPI-BASELINE:      covered-functions-format 3
// SPI-BASELINE-NEXT: mode - baseline
// SPI-BASELINE:      function - "main"
// SPI-BASELINE-NEXT: function-id - "main" 0x0000000000000018
// SPI-BASELINE-NEXT: entry-count - 0

// SPI-EXECUTION:      covered-functions-format 3
// SPI-EXECUTION-NEXT: mode - execution
// SPI-EXECUTION:      function - "main"
// SPI-EXECUTION-NEXT: function-id - "main" 0x0000000000000018
// SPI-EXECUTION-NEXT: entry-count - 1

#ifdef FAIL
#error expected failure
#endif

#if defined(FIRST)
static int first_only(void) { return 1; }
#elif defined(SECOND)
static int second_only(void) { return 2; }
#endif

int main(void) { return 0; }

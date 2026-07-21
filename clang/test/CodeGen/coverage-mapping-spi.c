// RUN: rm -f %t.o %t.spi
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-obj \
// RUN:   -fprofile-instrument=clang -fcoverage-mapping \
// RUN:   -fcoverage-mapping-spi=%t.spi -o %t.o %s
// RUN: test -s %t.o
// RUN: test -s %t.spi
// RUN: rm -f %t.o %t.spi
// RUN: not %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-obj \
// RUN:   -DFAIL -fprofile-instrument=clang -fcoverage-mapping \
// RUN:   -fcoverage-mapping-spi=%t.spi -o %t.o %s
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

#ifdef FAIL
#error expected failure
#endif

#if defined(FIRST)
static int first_only(void) { return 1; }
#elif defined(SECOND)
static int second_only(void) { return 2; }
#endif

int main(void) { return 0; }

// REQUIRES: x86-registered-target
// UNSUPPORTED: system-windows
//
// RUN: rm -rf %t && split-file %s %t
// RUN: %clang -O2 -std=gnu89 -mllvm -enable-name-compression=false \
// RUN:   -fprofile-instr-generate -fcoverage-mapping \
// RUN:   -fcoverage-mapping-spi=%t/coverage.spi \
// RUN:   -Xclang -fcoverage-mapping-spi-key=main-key -c %t/main.c \
// RUN:   -o %t/main.o
// RUN: %clang -O2 -std=gnu89 -mllvm -enable-name-compression=false \
// RUN:   -fprofile-instr-generate -fcoverage-mapping \
// RUN:   -fcoverage-mapping-spi=%t/coverage.spi \
// RUN:   -Xclang -fcoverage-mapping-spi-key=a-key -c %t/a.c -o %t/a.o
// RUN: %clang -O2 -std=gnu89 -mllvm -enable-name-compression=false \
// RUN:   -fprofile-instr-generate -fcoverage-mapping \
// RUN:   -fcoverage-mapping-spi=%t/coverage.spi \
// RUN:   -Xclang -fcoverage-mapping-spi-key=b-key -c %t/b.c -o %t/b.o
// RUN: %clang -O2 -std=gnu89 -mllvm -enable-name-compression=false \
// RUN:   -fprofile-instr-generate -fcoverage-mapping \
// RUN:   -fcoverage-mapping-spi=%t/coverage.spi \
// RUN:   -Xclang -fcoverage-mapping-spi-key=c-key -c %t/c.c -o %t/c.o
// RUN: %clang -O2 -std=gnu89 -mllvm -enable-name-compression=false \
// RUN:   -fprofile-instr-generate -fcoverage-mapping \
// RUN:   -fcoverage-mapping-spi=%t/coverage.spi \
// RUN:   -Xclang -fcoverage-mapping-spi-key=d-key -c %t/d.c -o %t/d.o
// RUN: %clang -O2 -std=gnu89 -mllvm -enable-name-compression=false \
// RUN:   -fprofile-instr-generate -fcoverage-mapping \
// RUN:   -fcoverage-mapping-spi=%t/coverage.spi \
// RUN:   -Xclang -fcoverage-mapping-spi-key=provider-key -c %t/provider.c \
// RUN:   -o %t/provider.o
// RUN: llvm-profdata merge -sparse %t/profile.proftext -o %t/repro.profdata
// RUN: llvm-cov export %t/coverage.spi --dump --txtcvrg \
// RUN:   --instr-profile=%t/repro.profdata > %t/spi.txt 2> %t/spi.err
// RUN: test ! -s %t/spi.err
// RUN: FileCheck %s --check-prefix=EXEC < %t/spi.txt

// EXEC: function "hdr_branch" 4 3 75.00
// EXEC-NEXT: 1 {{[0-9]+}}.37 {{[0-9]+}}.2 1
// EXEC-NEXT: 1 {{[0-9]+}}.7 {{[0-9]+}}.13 1
// EXEC-NEXT: 1 {{[0-9]+}}.5 {{[0-9]+}}.18 0
// EXEC-NEXT: 1 {{[0-9]+}}.3 {{[0-9]+}}.16 1

//--- profile.proftext
hdr_branch
# Func Hash:
11211998296
# Num Counters:
2
# Counter Values:
1
0

//--- inline_hdr.h
extern inline int hdr_branch(int x);

extern inline int hdr_branch(int x) {
  if (x > 10)
    return x - 10;
  return x + 10;
}

//--- provider.c
int hdr_branch(int x) {
  if (x > 10)
    return x - 10;
  return x + 10;
}

//--- main.c
#include "inline_hdr.h"

int a(void);
int b(void);
int c(void);
int d(void);

int main(void) {
  return a() + b() + c() + d() == 19 ? 0 : 1;
}

//--- a.c
#include "inline_hdr.h"

int a(void) {
  return hdr_branch(20);
}

//--- b.c
#include "inline_hdr.h"

int b(void) {
  return 2;
}

//--- c.c
#include "inline_hdr.h"

int c(void) {
  return 3;
}

//--- d.c
#include "inline_hdr.h"

int d(void) {
  return 4;
}

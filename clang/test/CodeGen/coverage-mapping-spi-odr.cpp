// Verify that coverage SPI preserves the canonical profile identity of C++ ODR
// functions while continuing to distinguish ordinary functions by logical
// compilation unit.

// RUN: rm -rf %t && split-file %s %t && cd %t
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -emit-llvm \
// RUN:   -fprofile-instrument=clang -fcoverage-mapping \
// RUN:   -fcoverage-mapping-spi=%t/ir.spi \
// RUN:   -fcoverage-mapping-spi-key=unit-a -o %t/a.ll %t/a.cpp
// RUN: FileCheck %s --check-prefix=IR < %t/a.ll
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -O2 -emit-obj \
// RUN:   -fprofile-instrument=clang -fcoverage-mapping \
// RUN:   -fcoverage-mapping-spi=%t/coverage.spi \
// RUN:   -fcoverage-mapping-spi-key=unit-a -o %t/a.o %t/a.cpp
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -O2 -emit-obj \
// RUN:   -fprofile-instrument=clang -fcoverage-mapping \
// RUN:   -fcoverage-mapping-spi=%t/coverage.spi \
// RUN:   -fcoverage-mapping-spi-key=unit-b -o %t/b.o %t/b.cpp
// RUN: llvm-cov export %t/coverage.spi -empty-profile \
// RUN:   | FileCheck %s --check-prefix=EXPORT

// IR-DAG: @__profc__Z13shared_inlinei
// IR-DAG: @__profc__Z15shared_templateIiET_S0_
// IR-DAG: @__profc__ZN13SharedVirtualD2Ev
// IR-DAG: @__profc__ZNK13SharedDerivedIiE5applyEi
// IR-DAG: @__profc__ZN15ExplicitWeakODRIiE5valueEv = weak_odr
// IR-DAG: @"__profc___llvm_covspi$1${{[0-9a-f]+}}$_Z6from_av"

// EXPORT-DAG: "name":"_Z13shared_inlinei"
// EXPORT-DAG: "name":"_Z15shared_templateIiET_S0_"
// EXPORT-DAG: "name":"_ZN13SharedVirtualD2Ev"
// EXPORT-DAG: "name":"_ZNK13SharedDerivedIiE5applyEi"
// EXPORT-DAG: "name":"_ZN15ExplicitWeakODRIiE5valueEv"
// EXPORT-DAG: "name":"_Z6from_av"
// EXPORT-DAG: "name":"_Z6from_bv"
// EXPORT: "totals":{{.*}}"functions":{"count":7

//--- shared.hpp
#pragma once

__attribute__((noinline)) inline int shared_inline(int Value) {
  if (Value > 0)
    return Value + 10;
  return Value - 10;
}

template <typename T>
__attribute__((noinline)) T shared_template(T Value) {
  if (Value > 0)
    return Value * 2;
  return Value - 2;
}

struct SharedVirtual {
  virtual ~SharedVirtual() = default;
  virtual int apply(int Value) const = 0;
};

template <typename T> struct SharedDerived final : SharedVirtual {
  int apply(int Value) const override {
    return shared_template<T>(static_cast<T>(Value));
  }
};

#ifdef EMIT_WEAK_ODR
template <typename T> struct ExplicitWeakODR {
  __attribute__((used, noinline)) constexpr int value() { return 1; }
};

template struct ExplicitWeakODR<int>;
#endif

//--- a.cpp
#define EMIT_WEAK_ODR
#include "shared.hpp"

int from_a() {
  SharedDerived<int> Object;
  ExplicitWeakODR<int> Explicit;
  return shared_inline(Object.apply(1)) + Explicit.value();
}

//--- b.cpp
#include "shared.hpp"

int from_b() {
  SharedDerived<int> Object;
  return shared_inline(Object.apply(2));
}

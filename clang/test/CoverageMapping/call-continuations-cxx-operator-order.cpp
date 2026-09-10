// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -Wno-unused-value -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -dump-coverage-mapping -emit-llvm-only -o - %s | FileCheck %s --check-prefix=MAP
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -Wno-unused-value -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-obj -o %t.o %s
// RUN: llvm-cov export %t.o --empty-profile > /dev/null
// RUN: rm -f %t.spi
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -Wno-unused-value -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -fcoverage-mapping-spi=%t.spi -fcoverage-mapping-spi-key=operator-order -emit-obj -o %t.spi.o %s
// RUN: llvm-cov export %t.spi --txtcvrgfull --txtcvrg-view=regions > /dev/null
// RUN: llvm-cov export %t.spi --txtcvrgfull --txtcvrg-view=segments > /dev/null

void sink();

struct AssignmentCounter {
  AssignmentCounter &operator=(const unsigned &);
};

struct AssignmentStats {
  AssignmentCounter first;
  AssignmentCounter second;
};

struct AssignmentIterator {
  AssignmentStats *operator->();
};

void overloaded_assignment_with_arrow(AssignmentIterator it) {
  it->first = 0;
  it->second = 0;
}

struct ContinuationStream {
  ContinuationStream &operator<<(unsigned);
  ContinuationStream &operator<<(ContinuationStream &(*)(ContinuationStream &));
};

ContinuationStream &operator<<(ContinuationStream &, const char *);
ContinuationStream &finish(ContinuationStream &);

void overloaded_stream_chain(ContinuationStream &out, const char *name,
                             unsigned value, char *text) {
  sink();
  sink();
  out << name << " checkpoint " << value << ": " <<
    text << " seconds" << finish;
}

struct Value {
  Value &operator=(const Value &);
  Value &operator+=(const Value &);
  Value operator+(const Value &) const;
  bool operator==(const Value &) const;
  int operator<=>(const Value &) const;
  Value &operator++();
  Value operator++(int);
  Value &operator,(const Value &);
  Value &operator[](unsigned);
  Value &operator()(unsigned = 0);
};

Value operator-(const Value &, const Value &);

#define ASSIGN_THROUGH_ARROW(Iter) (Iter)->second = 0

void operator_matrix(Value a, Value b, AssignmentIterator it) {
  sink();
  a = b;
  sink();
  a += b;
  sink();
  a + b;
  sink();
  a - b;
  sink();
  ++a;
  sink();
  a++;
  sink();
  (a, b);
  sink();
  a[1];
  sink();
  a();
  sink();
  a != b;
  sink();
  a < b;
  sink();
  ASSIGN_THROUGH_ARROW(it);
}

template <class T> void dependent_operators(T a, T b) {
  sink();
  a = b;
  sink();
  a + b;
  sink();
  a != b;
}

template void dependent_operators<Value>(Value, Value);

// An overloaded assignment evaluates its RHS before its source-leading object.
// Anchor the continuation before visiting an overloaded operator-> in that
// object, so the region does not run backwards from '=' to '->'.
// MAP-LABEL: _Z32overloaded_assignment_with_arrow18AssignmentIterator:
// MAP: Gap,File 0, [[FIRST_ASSIGN:[0-9]+]]:17 -> [[SECOND_ASSIGN:[0-9]+]]:3 = #[[AFTER_FIRST:[0-9]+]]
// MAP-NEXT: File 0, [[SECOND_ASSIGN]]:3 -> [[SECOND_ASSIGN]]:7 = #[[AFTER_FIRST]]

// A non-assignment overloaded operator needs the same source anchor. Without
// it, the continuation after the preceding call can start at the final operand
// and end on an earlier line in this left-associated chain.
// MAP-LABEL: _Z23overloaded_stream_chainR18ContinuationStreamPKcjPc:
// MAP: Gap,File 0, [[FIRST_SINK:[0-9]+]]:10 -> [[SECOND_SINK:[0-9]+]]:3 = #[[AFTER_FIRST_SINK:[0-9]+]]
// MAP-NEXT: File 0, [[SECOND_SINK]]:3 -> [[SECOND_SINK]]:9 = #[[AFTER_FIRST_SINK]]
// MAP-NEXT: Gap,File 0, [[SECOND_SINK]]:10 -> [[STREAM_LINE:[0-9]+]]:3 = #[[AFTER_SECOND_SINK:[0-9]+]]
// MAP-NEXT: File 0, [[STREAM_LINE]]:3 -> [[STREAM_LINE]]:14 = #[[AFTER_SECOND_SINK]]

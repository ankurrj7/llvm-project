// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefix=IR
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -mllvm -enable-single-byte-coverage=true -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefix=SB
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fprofile-instrument=clang -fcoverage-mapping -emit-llvm -o - %s | FileCheck %s --check-prefix=NOCC

void sink();

struct Automatic {
  Automatic();
  ~Automatic();
};

struct NoReturnDtor {
  NoReturnDtor();
  ~NoReturnDtor() __attribute__((noreturn));
};

struct NoReturnCtor {
  NoReturnCtor() __attribute__((noreturn));
};

Automatic make_automatic();
NoReturnDtor make_noreturn_dtor();

void automatic_nested_scope() {
  {
    Automatic value;
  }
  sink();
}

void automatic_nested_scope_noreturn() {
  {
    NoReturnDtor value;
  }
  sink();
}

void full_expression_temporary() {
  make_automatic();
  sink();
}

void full_expression_temporary_noreturn() {
  make_noreturn_dtor();
  sink();
}

void delete_expression(Automatic *p) {
  delete p;
  sink();
}

void delete_expression_noreturn(NoReturnDtor *p) {
  delete p;
  sink();
}

Automatic *new_expression() {
  Automatic *p = new Automatic;
  sink();
  return p;
}

NoReturnCtor *new_expression_noreturn() {
  NoReturnCtor *p = new NoReturnCtor;
  sink();
  return p;
}

struct Base {
  Base();
};

struct Member {
  Member();
};

int make_initial_value();

struct Complete : Base {
  Member member;
  int value = make_initial_value();
  Complete();
};

Complete::Complete() {
  sink();
}

struct NoReturnBase {
  NoReturnBase() __attribute__((noreturn));
};

struct BlockedByBase : NoReturnBase {
  BlockedByBase();
};

BlockedByBase::BlockedByBase() {
  sink();
}

struct NoReturnMember {
  NoReturnMember() __attribute__((noreturn));
};

struct BlockedByMember {
  NoReturnMember member;
  BlockedByMember();
};

BlockedByMember::BlockedByMember() {
  sink();
}

int no_return_initial_value() __attribute__((noreturn));

struct BlockedByDefault {
  int value = no_return_initial_value();
  BlockedByDefault();
};

BlockedByDefault::BlockedByDefault() {
  sink();
}

struct Delegating {
  Delegating();
  Delegating(int);
};

Delegating::Delegating(int) {
  sink();
}

Delegating::Delegating() : Delegating(1) {
  sink();
}

struct VirtualBase {
  VirtualBase();
};

struct VirtualComplete : virtual VirtualBase {
  Member member;
  VirtualComplete();
};

VirtualComplete::VirtualComplete() {
  sink();
}

template <bool Enabled>
void discarded_constexpr_branch() {
  if constexpr (Enabled) {
    sink();
  } else {
    Automatic discarded;
    sink();
  }
  sink();
}

template void discarded_constexpr_branch<true>();

// The discarded branch must not reserve implicit-operation or call
// continuation counters. This specialization has one ordinary branch counter
// and exactly two emitted call-continuation counters.
// IR-DAG: @__profc__Z26discarded_constexpr_branchILb1EEvv = {{.*}}global [4 x i64]

// Keep all newly modeled implicit operations strictly opt-in. Without the
// feature, their pre-existing counter layouts must not change.
// NOCC-DAG: @__profc__Z22automatic_nested_scopev = private global [1 x i64]
// NOCC-DAG: @__profc__Z25full_expression_temporaryv = private global [1 x i64]
// NOCC-DAG: @__profc__Z17delete_expressionP9Automatic = private global [1 x i64]
// NOCC-DAG: @__profc__Z14new_expressionv = private global [1 x i64]
// NOCC-DAG: @__profc__ZN8CompleteC2Ev = private global [1 x i64]
// NOCC-DAG: @__profc__Z26discarded_constexpr_branchILb1EEvv = {{.*}}global [2 x i64]

// A successful automatic-object cleanup increments the nested compound
// continuation before execution reaches the following statement.
// IR-LABEL: define{{.*}} void @_Z22automatic_nested_scopev(
// IR: call void @_ZN9AutomaticD1Ev
// IR-NEXT: load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z22automatic_nested_scopev
// IR: call void @_Z4sinkv

// SB-LABEL: define{{.*}} void @_Z22automatic_nested_scopev(
// SB: call void @_ZN9AutomaticD1Ev
// SB-NEXT: store i8 0, ptr getelementptr inbounds ({{.*}}@__profc__Z22automatic_nested_scopev
// SB: call void @_Z4sinkv

// A destructor declared noreturn has no normal cleanup continuation, and the
// statement following the scope is not emitted.
// IR-LABEL: define{{.*}} void @_Z31automatic_nested_scope_noreturnv(
// IR: call void @_ZN12NoReturnDtorD1Ev
// IR-NEXT: unreachable

// The full-expression continuation is after destruction of the temporary,
// not merely after the call that produced it.
// IR-LABEL: define{{.*}} void @_Z25full_expression_temporaryv(
// IR: call void @_ZN9AutomaticD1Ev
// IR-NEXT: load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z25full_expression_temporaryv
// IR: call void @_Z4sinkv

// IR-LABEL: define{{.*}} void @_Z34full_expression_temporary_noreturnv(
// IR: call void @_ZN12NoReturnDtorD1Ev
// IR-NEXT: unreachable

// The delete-expression continuation is reached after both destruction and
// deallocation. It is also the merge point for the null-pointer path.
// IR-LABEL: define{{.*}} void @_Z17delete_expressionP9Automatic(
// IR: br i1 {{.*}}, label %[[DELETE_END:[^, ]+]], label %[[DELETE_NOTNULL:[^, ]+]]
// IR: [[DELETE_NOTNULL]]:
// IR: call void @_ZN9AutomaticD1Ev
// IR: call void @_ZdlPv
// IR: br label %[[DELETE_END]]
// IR: [[DELETE_END]]:
// IR-NEXT: load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z17delete_expressionP9Automatic
// IR: call void @_Z4sinkv

// IR-LABEL: define{{.*}} void @_Z26delete_expression_noreturnP12NoReturnDtor(
// IR: call void @_ZN12NoReturnDtorD1Ev
// IR-NEXT: unreachable

// Allocation completion, construction, and the complete new-expression each
// have a distinct continuation in that order.
// IR-LABEL: define{{.*}} ptr @_Z14new_expressionv(
// IR: call{{.*}} ptr @_Znwm
// IR-NEXT: load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z14new_expressionv
// IR: call void @_ZN9AutomaticC1Ev
// IR-NEXT: load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z14new_expressionv
// IR: store i64
// IR-NEXT: load i64, ptr getelementptr inbounds ({{.*}}@__profc__Z14new_expressionv
// IR: call void @_Z4sinkv

// IR-LABEL: define{{.*}} ptr @_Z23new_expression_noreturnv(
// IR: call void @_ZN12NoReturnCtorC1Ev
// IR-NEXT: unreachable

// Implicit base/member/default-member initialization must all complete before
// the constructor-prologue counter seeds coverage for the written body.
// IR-LABEL: define{{.*}} void @_ZN8CompleteC2Ev(
// IR: call void @_ZN4BaseC2Ev
// IR: call void @_ZN6MemberC1Ev
// IR: call{{.*}} i32 @_Z18make_initial_valuev
// IR: store i32
// IR: load i64, ptr getelementptr inbounds ({{.*}}@__profc__ZN8CompleteC2Ev
// IR: store i64
// IR-NEXT: call void @_Z4sinkv

// SB-LABEL: define{{.*}} void @_ZN8CompleteC2Ev(
// SB: call{{.*}} i32 @_Z18make_initial_valuev
// SB: store i32
// SB-NEXT: store i8 0, ptr getelementptr inbounds ({{.*}}@__profc__ZN8CompleteC2Ev
// SB-NEXT: call void @_Z4sinkv

// A constructor-prologue counter is not emitted when an implicit base,
// member, or default-member initializer does not return.
// IR-LABEL: define{{.*}} void @_ZN13BlockedByBaseC2Ev(
// IR: call void @_ZN12NoReturnBaseC2Ev
// IR-NEXT: unreachable

// IR-LABEL: define{{.*}} void @_ZN15BlockedByMemberC2Ev(
// IR: call void @_ZN14NoReturnMemberC1Ev
// IR-NEXT: unreachable

// IR-LABEL: define{{.*}} void @_ZN16BlockedByDefaultC2Ev(
// IR: call{{.*}} i32 @_Z23no_return_initial_valuev
// IR-NEXT: unreachable

// A delegating constructor reaches its body only after the target constructor
// returns. The constructor-prologue counter follows the delegated construction
// continuation and is the final increment before the written body.
// IR-LABEL: define{{.*}} void @_ZN10DelegatingC2Ev(
// IR: call void @_ZN10DelegatingC2Ei
// IR: load i64, ptr getelementptr inbounds ({{.*}}@__profc__ZN10DelegatingC2Ev
// IR: store i64
// IR-NEXT: load i64, ptr getelementptr inbounds ({{.*}}@__profc__ZN10DelegatingC2Ev
// IR: store i64
// IR-NEXT: call void @_Z4sinkv

// Base-object construction skips the virtual base, while complete-object
// construction emits it. Both variants must seed the body only after the
// initializers that actually run in that variant have completed.
// IR-LABEL: define{{.*}} void @_ZN15VirtualCompleteC2Ev(
// IR-NOT: call void @_ZN11VirtualBaseC2Ev
// IR: call void @_ZN6MemberC1Ev
// IR: load i64, ptr getelementptr inbounds ({{.*}}@__profc__ZN15VirtualCompleteC2Ev
// IR: store i64
// IR-NEXT: call void @_Z4sinkv

// IR-LABEL: define{{.*}} void @_ZN15VirtualCompleteC1Ev(
// IR: call void @_ZN11VirtualBaseC2Ev
// IR: call void @_ZN6MemberC1Ev
// IR: load i64, ptr getelementptr inbounds ({{.*}}@__profc__ZN15VirtualCompleteC1Ev
// IR: store i64
// IR-NEXT: call void @_Z4sinkv

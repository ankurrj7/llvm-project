// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefix=IR
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-linux-gnu -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -dump-coverage-mapping -emit-llvm-only -o - %s | FileCheck %s --check-prefix=MAP

namespace std {
template <typename... T> struct coroutine_traits;

template <class Promise = void> struct coroutine_handle {
  coroutine_handle() = default;
  static coroutine_handle from_address(void *) noexcept { return {}; }
};

template <> struct coroutine_handle<void> {
  coroutine_handle() = default;
  static coroutine_handle from_address(void *) noexcept { return {}; }
  template <class PromiseType>
  coroutine_handle(coroutine_handle<PromiseType>) noexcept {}
};
} // namespace std

struct suspend_always {
  bool await_ready() noexcept;
  void await_suspend(std::coroutine_handle<>) noexcept;
  void await_resume() noexcept;
};

struct awaiter;

template <> struct std::coroutine_traits<int> {
  struct promise_type {
    int get_return_object();
    suspend_always initial_suspend();
    suspend_always final_suspend() noexcept;
    void unhandled_exception() noexcept;
    void return_value(int);
    awaiter yield_value(int);
  };
};

struct awaiter {
  bool await_ready();
  void await_suspend(std::coroutine_handle<>);
  int await_resume();
};

struct noreturn_awaiter {
  bool await_ready();
  void await_suspend(std::coroutine_handle<>);
  int await_resume() __attribute__((noreturn));
};

void sink();

// MAP-LABEL: await_then_sink:
extern "C" int await_then_sink() {
  (void)co_await awaiter{}; // MAP: Gap,File 0, [[#@LINE]]:28 -> [[#@LINE+1]]:3 = #[[AWAIT_CONT:[1-9][0-9]*]]
  sink();                   // MAP-NEXT: File 0, [[#@LINE]]:3 -> [[#@LINE]]:9 = #[[AWAIT_CONT]]
  co_return 0;
}

// MAP-LABEL: noreturn_await_then_sink:
extern "C" int noreturn_await_then_sink() {
  (void)co_await noreturn_awaiter{}; // MAP: Gap,File 0, [[#@LINE]]:37 -> [[#@LINE+1]]:3 = #[[NORETURN_AWAIT_CONT:[1-9][0-9]*]]
  sink();                            // MAP-NEXT: File 0, [[#@LINE]]:3 -> [[#@LINE]]:9 = #[[NORETURN_AWAIT_CONT]]
  co_return 0;
}

// MAP-LABEL: yield_then_sink:
extern "C" int yield_then_sink() {
  co_yield 1; // MAP: Gap,File 0, [[#@LINE]]:14 -> [[#@LINE+1]]:3 = #[[YIELD_CONT:[1-9][0-9]*]]
  sink();     // MAP-NEXT: File 0, [[#@LINE]]:3 -> [[#@LINE]]:9 = #[[YIELD_CONT]]
  co_return 0;
}

// The source continuation of co_await is after await_resume. A counter after
// await_ready or await_suspend would incorrectly count a suspended coroutine
// that has not resumed.
// IR-LABEL: define{{.*}} i32 @await_then_sink(
// IR: call{{.*}} i32 @_ZN7awaiter12await_resumeEv
// IR-NEXT: load i64, ptr getelementptr inbounds ({{.*}}@__profc_await_then_sink
// IR: call void @_Z4sinkv

// No continuation increment can be emitted on a non-returning resume path.
// IR-LABEL: define{{.*}} i32 @noreturn_await_then_sink(
// IR: call{{.*}} i32 @_ZN16noreturn_awaiter12await_resumeEv
// IR-NEXT: unreachable

// co_yield uses the same post-await_resume source continuation rule.
// IR-LABEL: define{{.*}} i32 @yield_then_sink(
// IR: call{{.*}} i32 @_ZN7awaiter12await_resumeEv
// IR: load i64, ptr getelementptr inbounds ({{.*}}@__profc_yield_then_sink
// IR: call void @_Z4sinkv

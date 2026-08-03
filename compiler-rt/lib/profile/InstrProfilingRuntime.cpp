//===- InstrProfilingRuntime.cpp - PGO runtime initialization -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#if defined(__linux__)
#include <unistd.h>
#endif

extern "C" {

#include "InstrProfiling.h"
#include "InstrProfilingInternal.h"

#if defined(__linux__) && defined(COMPILER_RT_PROFILE_COVERAGE_RUNTIME)
static volatile int ProfileDumpCurrentImageInProgress;
static volatile int ProfileDumpCurrentImageProcess;

static void updateDumpCurrentImageStateForProcess(void) {
  int CurrentProcess = (int)getpid();
  for (;;) {
    int Process =
        __sync_val_compare_and_swap(&ProfileDumpCurrentImageProcess, 0, 0);
    if (Process == CurrentProcess)
      return;
    if (Process == -CurrentProcess)
      continue;
    if (__sync_val_compare_and_swap(&ProfileDumpCurrentImageProcess, Process,
                                    -CurrentProcess) != Process)
      continue;

    /* A child must not inherit a guard owned by a vanished parent thread or
     * the parent's one-shot dump result. */
    __sync_lock_release(&ProfileDumpCurrentImageInProgress);
    lprofSetProfileDumped(0);
    lprofUpdateProfileNameForCurrentProcess();
    __sync_lock_test_and_set(&ProfileDumpCurrentImageProcess, CurrentProcess);
    return;
  }
}

COMPILER_RT_VISIBILITY int __llvm_profile_dump_current_image_impl(void) {
  updateDumpCurrentImageStateForProcess();
  if (__sync_lock_test_and_set(&ProfileDumpCurrentImageInProgress, 1))
    return -1;
  int Result = lprofProfileDumpFailed();
  if (!Result && !lprofProfileDumped())
    Result = __llvm_profile_dump();
  __sync_lock_release(&ProfileDumpCurrentImageInProgress);
  return Result;
}

/*
 * Keep this entry point externally visible so __llvm_profile_dump_all() can
 * find the copy belonging to each loaded ELF image. The function it calls is
 * hidden, ensuring that the dump uses this image's profile sections and runtime
 * state rather than another image's interposed runtime.
 */
__attribute__((visibility("default"))) COMPILER_RT_USED int
__llvm_profile_dump_current_image(void) {
  return __llvm_profile_dump_current_image_impl();
}
#endif

static int RegisterRuntime() {
  __llvm_profile_initialize();
#if defined(__linux__) && defined(COMPILER_RT_PROFILE_COVERAGE_RUNTIME)
  ProfileDumpCurrentImageProcess = (int)getpid();
#endif
#ifdef _AIX
  extern COMPILER_RT_VISIBILITY void *__llvm_profile_keep[];
  (void)*(void *volatile *)__llvm_profile_keep;
#endif
  return 0;
}

/* int __llvm_profile_runtime  */
COMPILER_RT_VISIBILITY int INSTR_PROF_PROFILE_RUNTIME_VAR = RegisterRuntime();
}

//===- InstrProfilingRuntime.cpp - PGO runtime initialization -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

extern "C" {

#include "InstrProfiling.h"
#include "InstrProfilingInternal.h"

#if defined(__linux__) && defined(COMPILER_RT_PROFILE_COVERAGE_RUNTIME)
static volatile int ProfileDumpCurrentImageInProgress;

COMPILER_RT_VISIBILITY int __llvm_profile_dump_current_image_impl(void) {
  if (__sync_lock_test_and_set(&ProfileDumpCurrentImageInProgress, 1))
    return -1;
  int Result = lprofProfileDumped() ? 0 : __llvm_profile_dump();
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
#ifdef _AIX
  extern COMPILER_RT_VISIBILITY void *__llvm_profile_keep[];
  (void)*(void *volatile *)__llvm_profile_keep;
#endif
  return 0;
}

/* int __llvm_profile_runtime  */
COMPILER_RT_VISIBILITY int INSTR_PROF_PROFILE_RUNTIME_VAR = RegisterRuntime();
}

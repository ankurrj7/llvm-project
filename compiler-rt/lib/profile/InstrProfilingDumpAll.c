/*===- InstrProfilingDumpAll.c - Dump profiles from loaded images ---------===*\
|*
|* Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
|* See https://llvm.org/LICENSE.txt for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
\*===----------------------------------------------------------------------===*/

#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <link.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#endif

#include "InstrProfiling.h"

#if defined(__linux__)

typedef int (*ProfileDumpFunction)(void);

typedef struct {
  char *Name;
  uintptr_t AddressBegin;
  uintptr_t AddressEnd;
  void *Handle;
  ProfileDumpFunction Dump;
} ProfileDumpAllEntry;

typedef struct {
  const void *CurrentAddress;
  ProfileDumpAllEntry *Entries;
  size_t NumEntries;
  size_t EntriesCapacity;
  unsigned long long LoaderAdds;
  unsigned long long LoaderSubs;
  int HasLoaderGeneration;
  int Result;
} ProfileDumpAllContext;

static volatile int ProfileDumpAllInProgress;
static volatile int ProfileDumpAllProcess;

static void updateDumpAllStateForProcess(void) {
  int CurrentProcess = (int)getpid();
  for (;;) {
    int Process = __sync_val_compare_and_swap(&ProfileDumpAllProcess, 0, 0);
    if (Process == CurrentProcess)
      return;
    if (Process == -CurrentProcess)
      continue;
    if (__sync_val_compare_and_swap(&ProfileDumpAllProcess, Process,
                                    -CurrentProcess) != Process)
      continue;

    /* No parent thread survives in the child to release an inherited guard. */
    __sync_lock_release(&ProfileDumpAllInProgress);
    __sync_lock_test_and_set(&ProfileDumpAllProcess, CurrentProcess);
    return;
  }
}

static int getImageAddressRange(const struct dl_phdr_info *Info,
                                uintptr_t *AddressBegin,
                                uintptr_t *AddressEnd) {
  uintptr_t Begin = UINTPTR_MAX;
  uintptr_t End = 0;
  for (ElfW(Half) I = 0; I < Info->dlpi_phnum; ++I) {
    const ElfW(Phdr) *Header = &Info->dlpi_phdr[I];
    if (Header->p_type != PT_LOAD)
      continue;
    uintptr_t Start = (uintptr_t)Info->dlpi_addr + Header->p_vaddr;
    if (Header->p_memsz > UINTPTR_MAX - Start)
      return 0;
    uintptr_t Limit = Start + Header->p_memsz;
    if (Start < Begin)
      Begin = Start;
    if (Limit > End)
      End = Limit;
  }
  if (Begin >= End)
    return 0;
  *AddressBegin = Begin;
  *AddressEnd = End;
  return 1;
}

static int imageContainsAddress(const struct dl_phdr_info *Info,
                                const void *Address) {
  uintptr_t Target = (uintptr_t)Address;
  for (ElfW(Half) I = 0; I < Info->dlpi_phnum; ++I) {
    const ElfW(Phdr) *Header = &Info->dlpi_phdr[I];
    if (Header->p_type != PT_LOAD)
      continue;
    uintptr_t Start = (uintptr_t)Info->dlpi_addr + Header->p_vaddr;
    if (Target >= Start && Target - Start < Header->p_memsz)
      return 1;
  }
  return 0;
}

static int appendDumpEntry(ProfileDumpAllContext *Context, const char *Name,
                           uintptr_t AddressBegin, uintptr_t AddressEnd) {
  if (Context->NumEntries == Context->EntriesCapacity) {
    if (Context->EntriesCapacity > SIZE_MAX / 2)
      return -1;
    size_t NewCapacity =
        Context->EntriesCapacity ? 2 * Context->EntriesCapacity : 8;
    if (NewCapacity > SIZE_MAX / sizeof(*Context->Entries))
      return -1;
    ProfileDumpAllEntry *NewEntries = (ProfileDumpAllEntry *)realloc(
        Context->Entries, NewCapacity * sizeof(*NewEntries));
    if (!NewEntries)
      return -1;
    Context->Entries = NewEntries;
    Context->EntriesCapacity = NewCapacity;
  }

  char *NameCopy = NULL;
  if (Name) {
    size_t NameLength = strlen(Name) + 1;
    NameCopy = (char *)malloc(NameLength);
    if (!NameCopy)
      return -1;
    memcpy(NameCopy, Name, NameLength);
  }

  Context->Entries[Context->NumEntries].Name = NameCopy;
  Context->Entries[Context->NumEntries].AddressBegin = AddressBegin;
  Context->Entries[Context->NumEntries].AddressEnd = AddressEnd;
  Context->Entries[Context->NumEntries].Handle = NULL;
  Context->Entries[Context->NumEntries].Dump = NULL;
  ++Context->NumEntries;
  return 0;
}

static int collectLoadedImage(struct dl_phdr_info *Info, size_t Size,
                              void *Data) {
  ProfileDumpAllContext *Context = (ProfileDumpAllContext *)Data;

#if defined(__GLIBC__)
  if (Size >=
      offsetof(struct dl_phdr_info, dlpi_subs) + sizeof(Info->dlpi_subs)) {
    Context->LoaderAdds = Info->dlpi_adds;
    Context->LoaderSubs = Info->dlpi_subs;
    Context->HasLoaderGeneration = 1;
  }
#else
  (void)Size;
#endif

  /* Avoid rediscovering the image containing dump_all. The main executable has
   * an empty name; retain it when this API is called from a shared object. */
  if (imageContainsAddress(Info, Context->CurrentAddress))
    return 0;

  /*
   * Do not call dlopen(), dlsym(), or dlclose() from this callback. Those
   * operations can invert the loader lock against a concurrent dlclose().
   * Copy enough information to reacquire and verify the image after
   * dl_iterate_phdr() releases its lock.
   */
  const char *Name =
      Info->dlpi_name && Info->dlpi_name[0] ? Info->dlpi_name : NULL;
  uintptr_t AddressBegin;
  uintptr_t AddressEnd;
  if (!getImageAddressRange(Info, &AddressBegin, &AddressEnd))
    return 0;
  if (appendDumpEntry(Context, Name, AddressBegin, AddressEnd) &&
      !Context->Result)
    Context->Result = -1;
  return 0;
}

static void acquireSnapshotImage(ProfileDumpAllEntry *Entry) {
  /*
   * RTLD_NOLOAD pins an image that survived the snapshot. If it was unloaded
   * before this point, its existing unload handler wrote its profile.
   */
  void *Handle = Entry->Name ? dlopen(Entry->Name, RTLD_LAZY | RTLD_NOLOAD)
                             : dlopen(NULL, RTLD_LAZY);
  if (!Handle)
    return;
  Entry->Handle = Handle;

  dlerror();
  void *Symbol = dlsym(Handle, "__llvm_profile_dump_current_image");
  const char *Error = dlerror();

  /*
   * dlsym() may find the symbol in a dependency of an uninstrumented image.
   * It may also find a newly loaded image with the same name. Only invoke the
   * callback when the definition still belongs to the snapshotted image.
   */
  uintptr_t SymbolAddress = (uintptr_t)Symbol;
  if (!Error && Symbol && SymbolAddress >= Entry->AddressBegin &&
      SymbolAddress < Entry->AddressEnd) {
    union {
      void *Object;
      ProfileDumpFunction Function;
    } Convert;
    Convert.Object = Symbol;
    Entry->Dump = Convert.Function;
  }
}

typedef struct {
  unsigned long long Adds;
  unsigned long long Subs;
  int Available;
} LoaderGeneration;

static int collectLoaderGeneration(struct dl_phdr_info *Info, size_t Size,
                                   void *Data) {
  LoaderGeneration *Generation = (LoaderGeneration *)Data;
#if defined(__GLIBC__)
  if (Size >=
      offsetof(struct dl_phdr_info, dlpi_subs) + sizeof(Info->dlpi_subs)) {
    Generation->Adds = Info->dlpi_adds;
    Generation->Subs = Info->dlpi_subs;
    Generation->Available = 1;
  }
#else
  (void)Info;
  (void)Size;
  (void)Generation;
#endif
  return 1;
}

static int snapshotIsStable(const ProfileDumpAllContext *Context) {
  LoaderGeneration Current = {0, 0, 0};
  dl_iterate_phdr(collectLoaderGeneration, &Current);
  return !Context->HasLoaderGeneration || !Current.Available ||
         (Context->LoaderAdds == Current.Adds &&
          Context->LoaderSubs == Current.Subs);
}

static void releaseSnapshot(ProfileDumpAllContext *Context) {
  for (size_t I = 0; I < Context->NumEntries; ++I) {
    if (Context->Entries[I].Handle && dlclose(Context->Entries[I].Handle) &&
        !Context->Result)
      Context->Result = -1;
    free(Context->Entries[I].Name);
  }
  free(Context->Entries);
}

#endif

COMPILER_RT_VISIBILITY int __llvm_profile_dump_all(void) {
#if defined(__linux__)
  updateDumpAllStateForProcess();
  if (__sync_lock_test_and_set(&ProfileDumpAllInProgress, 1))
    return -1;
#endif

#if defined(__linux__)
  int Result = __llvm_profile_dump_current_image_impl();
#else
  int Result = __llvm_profile_dump();
#endif

#if defined(__linux__)
  enum { MaxSnapshotAttempts = 8 };
  for (int Attempt = 0; Attempt < MaxSnapshotAttempts; ++Attempt) {
    ProfileDumpAllContext Context = {
        (const void *)&__llvm_profile_dump_all, NULL, 0, 0, 0, 0, 0, Result};
    dl_iterate_phdr(collectLoadedImage, &Context);
    for (size_t I = 0; I < Context.NumEntries; ++I)
      acquireSnapshotImage(&Context.Entries[I]);

    int Stable = snapshotIsStable(&Context);
    if (Stable) {
      for (size_t I = 0; I < Context.NumEntries; ++I) {
        if (!Context.Entries[I].Dump)
          continue;
        int ImageResult = Context.Entries[I].Dump();
        if (ImageResult && !Context.Result)
          Context.Result = ImageResult;
      }
    }

    releaseSnapshot(&Context);
    Result = Context.Result;
    if (Stable)
      break;
    if (Attempt + 1 == MaxSnapshotAttempts && !Result)
      Result = -1;
  }
  __sync_lock_release(&ProfileDumpAllInProgress);
#endif

  return Result;
}

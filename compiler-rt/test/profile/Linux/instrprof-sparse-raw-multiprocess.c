// RUN: rm -rf %t.dir && mkdir -p %t.dir
// RUN: %clang_profgen %s -o %t.dir/app
// RUN: env LLVM_PROFILE_FILE=%t.dir/shared-%m.profraw %run %t.dir/app
// RUN: llvm-profdata merge -sparse %t.dir/shared-*.profraw \
// RUN:   -o %t.dir/shared.profdata
// RUN: llvm-profdata show --function=called --counts \
// RUN:   %t.dir/shared.profdata | FileCheck %s
//
// CHECK: called:
// CHECK: Function count: 234

#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

enum { NumChildren = 12 };

int __llvm_profile_write_file(void);

__attribute__((noinline)) void called(void) {}

int main(void) {
  int StartPipe[2];
  pid_t Children[NumChildren];

  if (pipe(StartPipe))
    return 1;

  for (int I = 0; I < NumChildren; ++I) {
    Children[I] = fork();
    if (Children[I] < 0)
      return 2;
    if (Children[I] == 0) {
      char Byte;
      close(StartPipe[1]);
      while (read(StartPipe[0], &Byte, 1) < 0 && errno == EINTR) {
      }
      close(StartPipe[0]);

      for (int J = 0; J < (I + 1) * 3; ++J)
        called();

      int Result = __llvm_profile_write_file();
      _exit(Result ? 3 : 0);
    }
  }

  close(StartPipe[0]);
  close(StartPipe[1]);

  for (int I = 0; I < NumChildren; ++I) {
    int Status;
    if (waitpid(Children[I], &Status, 0) != Children[I] || !WIFEXITED(Status) ||
        WEXITSTATUS(Status) != 0)
      return 4;
  }

  _exit(0);
}

// RUN: rm -rf %t.dir && mkdir -p %t.dir
// RUN: %clang_profgen %s -o %t.dir/app
// RUN: %run %t.dir/app %t.dir/file-object.profraw
// RUN: llvm-profdata show --all-functions %t.dir/file-object.profraw | \
// RUN:   FileCheck %s

// A user-supplied FILE may not support seeking or file-descriptor locking.
// Sparse append output must fall back to the dense writer for that API.

#define _GNU_SOURCE
#include <stdio.h>
#include <sys/types.h>

int __llvm_profile_dump(void);
int __llvm_profile_set_file_object(FILE *, int);

// CHECK: covered:
// CHECK: untouched:
__attribute__((noinline)) void covered(void) {}
__attribute__((noinline)) void untouched(void) {}

static ssize_t write_cookie(void *Cookie, const char *Data, size_t Size) {
  return fwrite(Data, 1, Size, (FILE *)Cookie);
}

static int seek_cookie(void *Cookie, off64_t *Offset, int Whence) {
  FILE *Output = (FILE *)Cookie;
  if (fseeko(Output, *Offset, Whence))
    return -1;
  *Offset = ftello(Output);
  return *Offset == -1 ? -1 : 0;
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 1;
  covered();

  FILE *Output = fopen(argv[1], "wb");
  if (!Output)
    return 2;
  cookie_io_functions_t Functions = {0};
  Functions.write = write_cookie;
  Functions.seek = seek_cookie;
  FILE *Cookie = fopencookie(Output, "wb", Functions);
  if (!Cookie || setvbuf(Cookie, NULL, _IONBF, 0) ||
      __llvm_profile_set_file_object(Cookie, 0))
    return 3;
  if (__llvm_profile_dump())
    return 4;
  if (fclose(Cookie) || fclose(Output))
    return 5;
  return 0;
}

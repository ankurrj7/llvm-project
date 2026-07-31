// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -fms-extensions -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefixes=O0,X64-O0
// RUN: %clang_cc1 -triple i686-pc-windows-msvc -fms-extensions -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefixes=O0,X86-O0
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -fms-extensions -O2 -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefix=X64-O2
// RUN: %clang_cc1 -triple i686-pc-windows-msvc -fms-extensions -O2 -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefix=X86-O2

extern int may_raise(int);
extern int filter_step(int);
extern int finally_step(int);

int seh_filter(int value) {
  __try {
    value += may_raise(value);
  } __except (filter_step(value + 1)) {
    value += may_raise(value + 2);
  }
  return value;
}

int seh_finally(int value) {
  __try {
    value += may_raise(value);
  } __finally {
    value += finally_step(value + 3);
  }
  return value;
}

// O0-DAG: @__profc_seh_filter = private global [4 x i64]
// O0-DAG: @__profc_seh_finally = private global [3 x i64]

// X64-O0-LABEL: define internal i32 @"?filt$0@0@seh_filter@@"
// X64-O0: call i32 @filter_step
// X64-O0-NEXT: load i64, ptr getelementptr inbounds ([4 x i64], ptr @__profc_seh_filter, i32 0, i32 2)

// X86-O0-LABEL: define internal i32 @"?filt$0@0@seh_filter@@"
// X86-O0: call i32 @filter_step
// X86-O0-NEXT: load i64, ptr getelementptr inbounds ([4 x i64], ptr @__profc_seh_filter, i32 0, i32 2)

// X64-O0-LABEL: define internal void @"?fin$0@0@seh_finally@@"
// X64-O0: call i32 @finally_step
// X64-O0-NEXT: load i64, ptr getelementptr inbounds ([3 x i64], ptr @__profc_seh_finally, i32 0, i32 2)

// X86-O0-LABEL: define internal void @"?fin$0@0@seh_finally@@"
// X86-O0: call i32 @finally_step
// X86-O0-NEXT: load i64, ptr getelementptr inbounds ([3 x i64], ptr @__profc_seh_finally, i32 0, i32 2)

// X64-O2-LABEL: define internal i32 @"?filt$0@0@seh_filter@@"
// X64-O2: call i32 @filter_step
// X64-O2-NEXT: load i64, ptr getelementptr inbounds nuw (i8, ptr @__profc_seh_filter, i64 16)

// X86-O2-LABEL: define internal i32 @"?filt$0@0@seh_filter@@"
// X86-O2: call i32 @filter_step
// X86-O2-NEXT: load i64, ptr getelementptr inbounds nuw (i8, ptr @__profc_seh_filter, i32 16)

// X64-O2-LABEL: define dso_local i32 @seh_finally(
// X64-O2: call i32 @finally_step
// X64-O2-NEXT: load i64, ptr getelementptr inbounds nuw (i8, ptr @__profc_seh_finally, i64 16)
// X64-O2: call i32 @finally_step
// X64-O2-NEXT: load i64, ptr getelementptr inbounds nuw (i8, ptr @__profc_seh_finally, i64 16)

// X86-O2-LABEL: define dso_local i32 @seh_finally(
// X86-O2: call i32 @finally_step
// X86-O2-NEXT: load i64, ptr getelementptr inbounds nuw (i8, ptr @__profc_seh_finally, i32 16)
// X86-O2: call i32 @finally_step
// X86-O2-NEXT: load i64, ptr getelementptr inbounds nuw (i8, ptr @__profc_seh_finally, i32 16)

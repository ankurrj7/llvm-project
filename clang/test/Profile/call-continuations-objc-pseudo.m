// RUN: %clang_cc1 -triple x86_64-apple-macosx10.15.0 -fobjc-runtime=macosx-10.15.0 -fobjc-arc -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefixes=ON,ARC
// RUN: %clang_cc1 -triple x86_64-apple-macosx10.15.0 -fobjc-runtime=macosx-10.15.0 -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -emit-llvm -o - %s | FileCheck %s --check-prefixes=ON,MRC
// RUN: %clang_cc1 -triple x86_64-apple-macosx10.15.0 -fobjc-runtime=macosx-10.15.0 -fobjc-arc -fprofile-instrument=clang -fcoverage-mapping -emit-llvm -o - %s | FileCheck %s --check-prefix=OFF
// RUN: %clang_cc1 -triple x86_64-apple-macosx10.15.0 -fobjc-runtime=macosx-10.15.0 -fprofile-instrument=clang -fcoverage-mapping -emit-llvm -o - %s | FileCheck %s --check-prefix=OFF
// RUN: %clang_cc1 -triple x86_64-apple-macosx10.15.0 -fobjc-runtime=macosx-10.15.0 -fobjc-arc -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -dump-coverage-mapping -emit-llvm-only -o - %s | FileCheck %s --check-prefix=MAP
// RUN: %clang_cc1 -triple x86_64-apple-macosx10.15.0 -fobjc-runtime=macosx-10.15.0 -fprofile-instrument=clang -fcoverage-mapping -fcoverage-call-continuations -dump-coverage-mapping -emit-llvm-only -o - %s | FileCheck %s --check-prefix=MAP

__attribute__((objc_root_class))
@interface Box
+ (instancetype)alloc;
- (instancetype)init;
@property(nonatomic) int value;
- (Box *)objectAtIndexedSubscript:(int)index;
- (void)setObject:(Box *)object atIndexedSubscript:(int)index;
- (int)explicitValue;
@end

int marker(int);
Box *make_box(int);

int property_read(Box *box) {
  return box.value;
}

void property_set(void) {
  make_box(1).value =
      marker(2);
  marker(3);
}

void property_compound(void) {
  make_box(4).value +=
      marker(5);
  marker(6);
}

void property_preincrement(void) {
  ++make_box(7).value;
  marker(8);
}

void property_postincrement(void) {
  make_box(9).value++;
  marker(10);
}

int subscript_get(void) {
  return make_box(11)[marker(12)].value;
}

void subscript_set(void) {
  make_box(13)[
      marker(14)] =
      make_box(15);
  marker(16);
}

void explicit_message_operand(Box *box) {
  make_box(
      [box explicitValue]).value =
      marker(17);
  marker(18);
}

Box *fused_alloc_init(void) {
  return [[Box alloc] init];
}

void fused_alloc_init_operand(void) {
  [[Box alloc] init].value =
      marker(19);
  marker(20);
}

// The checks below deliberately describe only written call boundaries and the
// single pseudo-object completion. Synthetic getter/setter messages must not
// reserve independent continuation counters.

// OFF-DAG: @__profc_property_read = private global [1 x i64]
// OFF-DAG: @__profc_property_set = private global [1 x i64]
// OFF-DAG: @__profc_property_compound = private global [1 x i64]
// OFF-DAG: @__profc_property_preincrement = private global [1 x i64]
// OFF-DAG: @__profc_property_postincrement = private global [1 x i64]
// OFF-DAG: @__profc_subscript_get = private global [1 x i64]
// OFF-DAG: @__profc_subscript_set = private global [1 x i64]
// OFF-DAG: @__profc_explicit_message_operand = private global [1 x i64]
// OFF-DAG: @__profc_fused_alloc_init = private global [1 x i64]
// OFF-DAG: @__profc_fused_alloc_init_operand = private global [1 x i64]

// ARC-DAG: @__profc_property_read = private global [2 x i64]
// ARC-DAG: @__profc_property_set = private global [6 x i64]
// ARC-DAG: @__profc_property_compound = private global [6 x i64]
// ARC-DAG: @__profc_property_preincrement = private global [5 x i64]
// ARC-DAG: @__profc_property_postincrement = private global [5 x i64]
// ARC-DAG: @__profc_subscript_get = private global [6 x i64]
// ARC-DAG: @__profc_subscript_set = private global [7 x i64]
// ARC-DAG: @__profc_explicit_message_operand = private global [7 x i64]
// ARC-DAG: @__profc_fused_alloc_init = private global [4 x i64]
// ARC-DAG: @__profc_fused_alloc_init_operand = private global [7 x i64]

// MRC-DAG: @__profc_property_read = private global [2 x i64]
// MRC-DAG: @__profc_property_set = private global [5 x i64]
// MRC-DAG: @__profc_property_compound = private global [5 x i64]
// MRC-DAG: @__profc_property_preincrement = private global [4 x i64]
// MRC-DAG: @__profc_property_postincrement = private global [4 x i64]
// MRC-DAG: @__profc_subscript_get = private global [5 x i64]
// MRC-DAG: @__profc_subscript_set = private global [6 x i64]
// MRC-DAG: @__profc_explicit_message_operand = private global [6 x i64]
// MRC-DAG: @__profc_fused_alloc_init = private global [3 x i64]
// MRC-DAG: @__profc_fused_alloc_init_operand = private global [6 x i64]

// A simple assignment evaluates and completes the RHS before it evaluates the
// receiver. The implicit setter has no independent counter; #3 is the
// pseudo-object completion.
// ON-LABEL: define{{.*}} void @property_set(
// ON: call i32 @marker(i32 noundef 2)
// ON: getelementptr inbounds ({{.*}}@__profc_property_set, i32 0, i32 1)
// ON: call ptr @make_box(i32 noundef 1)
// ON: getelementptr inbounds ({{.*}}@__profc_property_set, i32 0, i32 2)
// ON: call void @objc_msgSend
// ON-NEXT: {{.*}}getelementptr inbounds ({{.*}}@__profc_property_set, i32 0, i32 3)
// ON: call i32 @marker(i32 noundef 3)
// ON: getelementptr inbounds ({{.*}}@__profc_property_set, i32 0, i32 {{[45]}})

// A compound assignment evaluates the receiver before the RHS. Its synthetic
// getter and setter still share one pseudo-object completion.
// ON-LABEL: define{{.*}} void @property_compound(
// ON: call ptr @make_box(i32 noundef 4)
// ON: getelementptr inbounds ({{.*}}@__profc_property_compound, i32 0, i32 1)
// ON: call i32 @marker(i32 noundef 5)
// ON: getelementptr inbounds ({{.*}}@__profc_property_compound, i32 0, i32 2)
// ON: call i32 @objc_msgSend
// ON: call void @objc_msgSend
// ON-NEXT: {{.*}}getelementptr inbounds ({{.*}}@__profc_property_compound, i32 0, i32 3)

// ON-LABEL: define{{.*}} void @property_preincrement(
// ON: call ptr @make_box(i32 noundef 7)
// ON: getelementptr inbounds ({{.*}}@__profc_property_preincrement, i32 0, i32 1)
// ON: call i32 @objc_msgSend
// ON: call void @objc_msgSend
// ON-NEXT: {{.*}}getelementptr inbounds ({{.*}}@__profc_property_preincrement, i32 0, i32 2)

// ON-LABEL: define{{.*}} void @property_postincrement(
// ON: call ptr @make_box(i32 noundef 9)
// ON: getelementptr inbounds ({{.*}}@__profc_property_postincrement, i32 0, i32 1)
// ON: call i32 @objc_msgSend
// ON: call void @objc_msgSend
// ON-NEXT: {{.*}}getelementptr inbounds ({{.*}}@__profc_property_postincrement, i32 0, i32 2)

// The inner subscript and outer property each own one pseudo-object completion.
// ON-LABEL: define{{.*}} i32 @subscript_get(
// ON: call ptr @make_box(i32 noundef 11)
// ON: getelementptr inbounds ({{.*}}@__profc_subscript_get, i32 0, i32 1)
// ON: call i32 @marker(i32 noundef 12)
// ON: getelementptr inbounds ({{.*}}@__profc_subscript_get, i32 0, i32 2)
// ON: call ptr @objc_msgSend
// ON: getelementptr inbounds ({{.*}}@__profc_subscript_get, i32 0, i32 3)
// ON: call i32 @objc_msgSend
// ON-NEXT: {{.*}}getelementptr inbounds ({{.*}}@__profc_subscript_get, i32 0, i32 4)

// Subscript assignment evaluates the RHS, receiver, and index in that order.
// ON-LABEL: define{{.*}} void @subscript_set(
// ON: call ptr @make_box(i32 noundef 15)
// ON: getelementptr inbounds ({{.*}}@__profc_subscript_set, i32 0, i32 1)
// ON: call ptr @make_box(i32 noundef 13)
// ON: getelementptr inbounds ({{.*}}@__profc_subscript_set, i32 0, i32 2)
// ON: call i32 @marker(i32 noundef 14)
// ON: getelementptr inbounds ({{.*}}@__profc_subscript_set, i32 0, i32 3)
// ON: call void @objc_msgSend
// ON-NEXT: {{.*}}getelementptr inbounds ({{.*}}@__profc_subscript_set, i32 0, i32 4)

// The explicit written message in the receiver operand keeps its own counter
// (#3), while the synthetic setter is represented only by POE completion #4.
// ON-LABEL: define{{.*}} void @explicit_message_operand(
// ON: call i32 @marker(i32 noundef 17)
// ON: getelementptr inbounds ({{.*}}@__profc_explicit_message_operand, i32 0, i32 1)
// ON: call i32 @objc_msgSend
// ON-NEXT: {{.*}}getelementptr inbounds ({{.*}}@__profc_explicit_message_operand, i32 0, i32 3)
// ON: call ptr @make_box
// ON: getelementptr inbounds ({{.*}}@__profc_explicit_message_operand, i32 0, i32 2)
// ON: call void @objc_msgSend
// ON-NEXT: {{.*}}getelementptr inbounds ({{.*}}@__profc_explicit_message_operand, i32 0, i32 4)

// Combined alloc-init emits one runtime call but preserves both explicit
// message completions. The same remains true while it is a POE operand.
// ON-LABEL: define{{.*}} ptr @fused_alloc_init(
// ON: call ptr @objc_alloc_init
// ON-NEXT: {{.*}}getelementptr inbounds ({{.*}}@__profc_fused_alloc_init, i32 0, i32 1)
// ON: getelementptr inbounds ({{.*}}@__profc_fused_alloc_init, i32 0, i32 2)

// ON-LABEL: define{{.*}} void @fused_alloc_init_operand(
// ON: call i32 @marker(i32 noundef 19)
// ON: getelementptr inbounds ({{.*}}@__profc_fused_alloc_init_operand, i32 0, i32 1)
// ON: call ptr @objc_alloc_init
// ON-NEXT: {{.*}}getelementptr inbounds ({{.*}}@__profc_fused_alloc_init_operand, i32 0, i32 2)
// ON: getelementptr inbounds ({{.*}}@__profc_fused_alloc_init_operand, i32 0, i32 3)
// ON: call void @objc_msgSend
// ON-NEXT: {{.*}}getelementptr inbounds ({{.*}}@__profc_fused_alloc_init_operand, i32 0, i32 4)

// Mapping follows semantic execution order, not the written assignment tree.
// MAP-LABEL: property_set:
// MAP: File 0, {{[0-9]+}}:3 -> {{[0-9]+}}:14 = #1
// MAP: File 0, {{[0-9]+}}:3 -> {{[0-9]+}}:12 = #{{[34]}}

// MAP-LABEL: property_compound:
// MAP: File 0, {{[0-9]+}}:7 -> {{[0-9]+}}:16 = #1
// MAP: File 0, {{[0-9]+}}:3 -> {{[0-9]+}}:12 = #{{[34]}}

// MAP-LABEL: subscript_get:
// MAP: File 0, {{[0-9]+}}:23 -> {{[0-9]+}}:33 = #1

// MAP-LABEL: subscript_set:
// MAP: File 0, {{[0-9]+}}:3 -> {{[0-9]+}}:15 = #1
// MAP: File 0, {{[0-9]+}}:7 -> {{[0-9]+}}:17 = #2
// MAP: File 0, {{[0-9]+}}:3 -> {{[0-9]+}}:13 = #{{[45]}}

// MAP-LABEL: explicit_message_operand:
// MAP: File 0, {{[0-9]+}}:3 -> {{[0-9]+}}:26 = #1
// MAP: File 0, {{[0-9]+}}:3 -> {{[0-9]+}}:13 = #{{[45]}}

// MAP-LABEL: fused_alloc_init:
// MAP: File 0, {{[0-9]+}}:3 -> {{[0-9]+}}:2 = #{{[23]}}

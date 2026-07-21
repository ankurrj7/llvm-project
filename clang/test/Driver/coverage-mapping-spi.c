// RUN: %clang -### -c -fprofile-instr-generate -fcoverage-mapping \
// RUN:   -fcoverage-mapping-spi=%t.spi %s 2>&1 | FileCheck %s --check-prefix=EXPLICIT
// RUN: env LLVM_COV_MAPPING_DIR=%t.spi-dir %clang -### -c \
// RUN:   -fprofile-instr-generate -fcoverage-mapping \
// RUN:   -fcoverage-mapping-spi %s 2>&1 | FileCheck %s --check-prefix=DEFAULT
// RUN: %clang -### -fprofile-instr-generate -fcoverage-mapping \
// RUN:   -fcoverage-mapping-spi=%t.spi %s -o %t.exe 2>&1 \
// RUN:   | FileCheck %s --check-prefix=LINK
// RUN: not %clang -### -c -fprofile-instr-generate \
// RUN:   -fcoverage-mapping-spi=%t.spi %s 2>&1 | FileCheck %s --check-prefix=NEEDS-MAPPING
// RUN: not %clang -### -c -fcoverage-mapping \
// RUN:   -fcoverage-mapping-spi=%t.spi %s 2>&1 | FileCheck %s --check-prefix=NEEDS-PROFILE
// RUN: not env -u LLVM_COV_MAPPING_DIR %clang -### -c \
// RUN:   -fprofile-instr-generate -fcoverage-mapping \
// RUN:   -fcoverage-mapping-spi %s 2>&1 | FileCheck %s --check-prefix=NEEDS-PATH
// RUN: not %clang -### -c -fprofile-instr-generate -fcoverage-mapping \
// RUN:   -fcoverage-mapping-spi= %s 2>&1 | FileCheck %s --check-prefix=NEEDS-PATH

// EXPLICIT: "-fcoverage-mapping-spi={{.*}}.spi"
// EXPLICIT: "-fcoverage-mapping-spi-key={{.*}}coverage-mapping-spi.o"
// DEFAULT: "-fcoverage-mapping-spi={{.*}}spi-dir{{[/\\]}}cov.spi"
// DEFAULT: "-fcoverage-mapping-spi-key={{.*}}coverage-mapping-spi.o"
// LINK: "-fcoverage-mapping-spi-key=logical:{{[0-9a-f]+}}"
// NEEDS-MAPPING: '-fcoverage-mapping-spi=' only allowed with '-fcoverage-mapping'
// NEEDS-PROFILE: '-fcoverage-mapping' only allowed with '-fprofile-instr-generate'
// NEEDS-PROFILE: '-fcoverage-mapping-spi=' only allowed with '-fprofile-instr-generate'
// NEEDS-PATH: error: -fcoverage-mapping-spi requires a non-empty explicit path or the LLVM_COV_MAPPING_DIR environment variable

int main(void) { return 0; }

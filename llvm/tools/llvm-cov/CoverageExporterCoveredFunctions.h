//===- CoverageExporterCoveredFunctions.h - Streaming coverage export ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_COV_COVERAGEEXPORTERCOVEREDFUNCTIONS_H
#define LLVM_COV_COVERAGEEXPORTERCOVEREDFUNCTIONS_H

#include "CoverageFilters.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ProfileData/Coverage/CoverageMapping.h"
#include "llvm/Support/Error.h"
#include <memory>
#include <string>
#include <utility>

namespace llvm {

class raw_ostream;

struct CoveredFunctionsExportOptions {
  enum class Mode { Execution, Baseline } ExportMode = Mode::Execution;
  bool IncludeBranches = false;
  bool IncludeMCDC = false;
};

/// Streams evaluated coverage functions and renders the txtcvrg text format.
///
/// Function blocks contain code regions in their root source file only.
/// Regions in macro and include files are emitted in source-file fragments,
/// where identical coordinates are combined with saturating addition. Large
/// partitions are recursively repartitioned to bound memory. Optional branch
/// and MC/DC records retain an explicit source filename and expansion identity.
class CoverageExporterCoveredFunctions final
    : public coverage::CoverageMappingFunctionRecordConsumer {
  class Implementation;
  std::unique_ptr<Implementation> Impl;

public:
  CoverageExporterCoveredFunctions(
      raw_ostream &OS,
      ArrayRef<std::pair<std::string, std::string>> PathRemappings,
      const CoverageFilters &FilenameFilters,
      CoveredFunctionsExportOptions Options = {});
  ~CoverageExporterCoveredFunctions() override;

  Error consume(StringRef RawFunctionName, uint64_t FunctionHash,
                coverage::FunctionRecord &&Function) override;

  /// Complete source-region aggregation and render the report. Must be called
  /// exactly once.
  Error finish();
};

} // namespace llvm

#endif // LLVM_COV_COVERAGEEXPORTERCOVEREDFUNCTIONS_H

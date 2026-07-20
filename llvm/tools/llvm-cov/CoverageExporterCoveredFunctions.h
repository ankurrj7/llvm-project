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

/// Streams evaluated coverage functions into a bounded external sort and
/// renders the versioned, function-centric covered-functions text format.
///
/// Each `function` contains one or more `fragment` blocks. A root fragment is
/// part of the function's coverage root file; an expansion fragment contains
/// regions attributed to a macro or include file. Each expansion instance is
/// nested inside that fragment with an `expansion-at` source location. Coverage
/// totals count CodeRegions only. Other region kinds are emitted for source
/// attribution but do not affect the totals. Branch outcomes are intentionally
/// not represented because this format has one execution count per region.
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

  /// Complete sorting and render the report. Must be called exactly once.
  Error finish();
};

} // namespace llvm

#endif // LLVM_COV_COVERAGEEXPORTERCOVEREDFUNCTIONS_H

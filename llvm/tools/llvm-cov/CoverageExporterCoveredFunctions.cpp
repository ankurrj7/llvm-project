//===- CoverageExporterCoveredFunctions.cpp - Streaming coverage export --===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CoverageExporterCoveredFunctions.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <queue>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace llvm;
using namespace llvm::coverage;

namespace {

constexpr size_t SortMemoryLimit = 32 * 1024 * 1024;
constexpr size_t MergeFanIn = 16;
// Bound the size of an individual spool record. This matters for very large
// functions because a merge retains one current record per input run.
constexpr size_t MaxRegionsPerFragmentChunk = 16 * 1024;
// The report format uses stable one-based region kind values.
constexpr uint32_t ReportCodeRegionKind = 1;
constexpr uint32_t ReportBranchRegionKind = 5;
constexpr uint32_t ReportMCDCDecisionRegionKind = 6;
constexpr uint32_t ReportMCDCBranchRegionKind = 7;

std::optional<uint32_t>
getReportRegionKind(CounterMappingRegion::RegionKind Kind) {
  switch (Kind) {
  case CounterMappingRegion::CodeRegion:
    return ReportCodeRegionKind;
  case CounterMappingRegion::ExpansionRegion:
    return 2;
  case CounterMappingRegion::SkippedRegion:
    return 3;
  case CounterMappingRegion::GapRegion:
    return 4;
  case CounterMappingRegion::BranchRegion:
  case CounterMappingRegion::MCDCDecisionRegion:
  case CounterMappingRegion::MCDCBranchRegion:
    return std::nullopt;
  }
  llvm_unreachable("unknown coverage region kind");
}

struct FragmentRegion {
  uint32_t Kind;
  uint32_t LineStart;
  uint32_t ColumnStart;
  uint32_t LineEnd;
  uint32_t ColumnEnd;
  uint64_t ExecutionCount;
  uint64_t FalseExecutionCount = 0;
  int32_t ConditionID = -1;
  int32_t TrueConditionID = -1;
  int32_t FalseConditionID = -1;
  bool TrueFolded = false;
  bool FalseFolded = false;

  bool operator<(const FragmentRegion &Other) const {
    return std::tie(LineStart, ColumnStart, Kind, LineEnd, ColumnEnd,
                    ExecutionCount, FalseExecutionCount, ConditionID,
                    TrueConditionID, FalseConditionID, TrueFolded,
                    FalseFolded) <
           std::tie(Other.LineStart, Other.ColumnStart, Other.Kind,
                    Other.LineEnd, Other.ColumnEnd, Other.ExecutionCount,
                    Other.FalseExecutionCount, Other.ConditionID,
                    Other.TrueConditionID, Other.FalseConditionID,
                    Other.TrueFolded, Other.FalseFolded);
  }
};

struct FunctionFragment {
  std::string Filename;
  std::string CoverageRootFilename;
  std::string ExpansionSiteFilename;
  std::string DisplayName;
  std::string RawFunctionName;
  uint64_t FunctionHash = 0;
  uint64_t EntryCount = 0;
  uint64_t OverallTotalCodeRegions = 0;
  uint64_t OverallHitCodeRegions = 0;
  uint64_t FragmentTotalCodeRegions = 0;
  uint64_t FragmentHitCodeRegions = 0;
  uint64_t FragmentGroupTotalCodeRegions = 0;
  uint64_t FragmentGroupHitCodeRegions = 0;
  uint32_t CoverageBodyLine = 0;
  uint32_t CoverageBodyColumn = 0;
  uint32_t ExpansionSiteLine = 0;
  uint32_t ExpansionSiteColumn = 0;
  uint32_t FragmentOrdinal = 0;
  uint32_t SortLine = 0;
  uint32_t SortColumn = 0;
  uint32_t ChunkIndex = 0;
  bool IsRootFragment = false;
  std::vector<FragmentRegion> Regions;

  size_t memorySize() const {
    return sizeof(*this) + Filename.capacity() +
           CoverageRootFilename.capacity() + ExpansionSiteFilename.capacity() +
           DisplayName.capacity() + RawFunctionName.capacity() +
           Regions.capacity() * sizeof(FragmentRegion);
  }

  bool operator<(const FunctionFragment &Other) const {
    return std::tuple(CoverageRootFilename, CoverageBodyLine,
                      CoverageBodyColumn, RawFunctionName, FunctionHash,
                      Filename != CoverageRootFilename, !IsRootFragment,
                      Filename, ExpansionSiteFilename, ExpansionSiteLine,
                      ExpansionSiteColumn, FragmentOrdinal, SortLine,
                      SortColumn, ChunkIndex) <
           std::tuple(Other.CoverageRootFilename, Other.CoverageBodyLine,
                      Other.CoverageBodyColumn, Other.RawFunctionName,
                      Other.FunctionHash,
                      Other.Filename != Other.CoverageRootFilename,
                      !Other.IsRootFragment, Other.Filename,
                      Other.ExpansionSiteFilename, Other.ExpansionSiteLine,
                      Other.ExpansionSiteColumn, Other.FragmentOrdinal,
                      Other.SortLine, Other.SortColumn, Other.ChunkIndex);
  }
};

template <typename T> void writePod(raw_ostream &OS, const T &Value) {
  OS.write(reinterpret_cast<const char *>(&Value), sizeof(Value));
}

Error writeString(raw_ostream &OS, StringRef Value) {
  if (Value.size() > std::numeric_limits<uint32_t>::max())
    return createStringError(errc::file_too_large,
                             "covered-functions string is too large");
  uint32_t Size = Value.size();
  writePod(OS, Size);
  OS.write(Value.data(), Value.size());
  return Error::success();
}

Error writeFragment(raw_ostream &OS, const FunctionFragment &Fragment) {
  if (Error E = writeString(OS, Fragment.Filename))
    return E;
  if (Error E = writeString(OS, Fragment.CoverageRootFilename))
    return E;
  if (Error E = writeString(OS, Fragment.ExpansionSiteFilename))
    return E;
  if (Error E = writeString(OS, Fragment.DisplayName))
    return E;
  if (Error E = writeString(OS, Fragment.RawFunctionName))
    return E;
  writePod(OS, Fragment.FunctionHash);
  writePod(OS, Fragment.EntryCount);
  writePod(OS, Fragment.OverallTotalCodeRegions);
  writePod(OS, Fragment.OverallHitCodeRegions);
  writePod(OS, Fragment.FragmentTotalCodeRegions);
  writePod(OS, Fragment.FragmentHitCodeRegions);
  writePod(OS, Fragment.FragmentGroupTotalCodeRegions);
  writePod(OS, Fragment.FragmentGroupHitCodeRegions);
  writePod(OS, Fragment.CoverageBodyLine);
  writePod(OS, Fragment.CoverageBodyColumn);
  writePod(OS, Fragment.ExpansionSiteLine);
  writePod(OS, Fragment.ExpansionSiteColumn);
  writePod(OS, Fragment.FragmentOrdinal);
  writePod(OS, Fragment.SortLine);
  writePod(OS, Fragment.SortColumn);
  writePod(OS, Fragment.ChunkIndex);
  writePod(OS, Fragment.IsRootFragment);
  if (Fragment.Regions.size() > std::numeric_limits<uint32_t>::max())
    return createStringError(errc::file_too_large,
                             "covered-functions region list is too large");
  uint32_t NumRegions = Fragment.Regions.size();
  writePod(OS, NumRegions);
  for (const FragmentRegion &Region : Fragment.Regions) {
    writePod(OS, Region.Kind);
    writePod(OS, Region.LineStart);
    writePod(OS, Region.ColumnStart);
    writePod(OS, Region.LineEnd);
    writePod(OS, Region.ColumnEnd);
    writePod(OS, Region.ExecutionCount);
    writePod(OS, Region.FalseExecutionCount);
    writePod(OS, Region.ConditionID);
    writePod(OS, Region.TrueConditionID);
    writePod(OS, Region.FalseConditionID);
    writePod(OS, Region.TrueFolded);
    writePod(OS, Region.FalseFolded);
  }
  return Error::success();
}

class FragmentReader {
  sys::fs::file_t File;
  bool IsOpen = false;

  Expected<bool> readBytes(MutableArrayRef<char> Buffer, bool AllowEOF) {
    size_t Offset = 0;
    while (Offset < Buffer.size()) {
      Expected<size_t> Bytes =
          sys::fs::readNativeFile(File, Buffer.drop_front(Offset));
      if (!Bytes)
        return Bytes.takeError();
      if (*Bytes == 0) {
        if (AllowEOF && Offset == 0)
          return false;
        return createStringError(errc::io_error,
                                 "truncated covered-functions spool file");
      }
      Offset += *Bytes;
    }
    return true;
  }

  template <typename T> Expected<bool> readPod(T &Value, bool AllowEOF) {
    return readBytes(
        MutableArrayRef(reinterpret_cast<char *>(&Value), sizeof(Value)),
        AllowEOF);
  }

  Error readString(std::string &Value, uint32_t Size) {
    Value.resize(Size);
    if (Size == 0)
      return Error::success();
    Expected<bool> Read = readBytes(MutableArrayRef(Value.data(), Size), false);
    if (!Read)
      return Read.takeError();
    return Error::success();
  }

public:
  static Expected<std::unique_ptr<FragmentReader>> create(StringRef Path) {
    Expected<sys::fs::file_t> FileOrErr = sys::fs::openNativeFileForRead(Path);
    if (!FileOrErr)
      return FileOrErr.takeError();
    auto Reader = std::unique_ptr<FragmentReader>(new FragmentReader());
    Reader->File = *FileOrErr;
    Reader->IsOpen = true;
    return std::move(Reader);
  }

  ~FragmentReader() {
    if (IsOpen)
      consumeError(errorCodeToError(sys::fs::closeFile(File)));
  }

  Expected<bool> read(FunctionFragment &Fragment) {
    uint32_t FilenameSize = 0;
    Expected<bool> Read = readPod(FilenameSize, true);
    if (!Read || !*Read)
      return Read;
    if (Error E = readString(Fragment.Filename, FilenameSize))
      return std::move(E);

    uint32_t CoverageRootFilenameSize = 0;
    if (Expected<bool> R = readPod(CoverageRootFilenameSize, false); !R)
      return R.takeError();
    if (Error E =
            readString(Fragment.CoverageRootFilename, CoverageRootFilenameSize))
      return std::move(E);

    uint32_t ExpansionSiteFilenameSize = 0;
    if (Expected<bool> R = readPod(ExpansionSiteFilenameSize, false); !R)
      return R.takeError();
    if (Error E = readString(Fragment.ExpansionSiteFilename,
                             ExpansionSiteFilenameSize))
      return std::move(E);

    uint32_t DisplayNameSize = 0;
    if (Expected<bool> R = readPod(DisplayNameSize, false); !R)
      return R.takeError();
    if (Error E = readString(Fragment.DisplayName, DisplayNameSize))
      return std::move(E);

    uint32_t RawNameSize = 0;
    if (Expected<bool> R = readPod(RawNameSize, false); !R)
      return R.takeError();
    if (Error E = readString(Fragment.RawFunctionName, RawNameSize))
      return std::move(E);

    if (Expected<bool> R = readPod(Fragment.FunctionHash, false); !R)
      return R.takeError();
    if (Expected<bool> R = readPod(Fragment.EntryCount, false); !R)
      return R.takeError();
    if (Expected<bool> R = readPod(Fragment.OverallTotalCodeRegions, false); !R)
      return R.takeError();
    if (Expected<bool> R = readPod(Fragment.OverallHitCodeRegions, false); !R)
      return R.takeError();
    if (Expected<bool> R = readPod(Fragment.FragmentTotalCodeRegions, false);
        !R)
      return R.takeError();
    if (Expected<bool> R = readPod(Fragment.FragmentHitCodeRegions, false); !R)
      return R.takeError();
    if (Expected<bool> R =
            readPod(Fragment.FragmentGroupTotalCodeRegions, false);
        !R)
      return R.takeError();
    if (Expected<bool> R = readPod(Fragment.FragmentGroupHitCodeRegions, false);
        !R)
      return R.takeError();
    if (Expected<bool> R = readPod(Fragment.CoverageBodyLine, false); !R)
      return R.takeError();
    if (Expected<bool> R = readPod(Fragment.CoverageBodyColumn, false); !R)
      return R.takeError();
    if (Expected<bool> R = readPod(Fragment.ExpansionSiteLine, false); !R)
      return R.takeError();
    if (Expected<bool> R = readPod(Fragment.ExpansionSiteColumn, false); !R)
      return R.takeError();
    if (Expected<bool> R = readPod(Fragment.FragmentOrdinal, false); !R)
      return R.takeError();
    if (Expected<bool> R = readPod(Fragment.SortLine, false); !R)
      return R.takeError();
    if (Expected<bool> R = readPod(Fragment.SortColumn, false); !R)
      return R.takeError();
    if (Expected<bool> R = readPod(Fragment.ChunkIndex, false); !R)
      return R.takeError();
    if (Expected<bool> R = readPod(Fragment.IsRootFragment, false); !R)
      return R.takeError();

    uint32_t NumRegions = 0;
    if (Expected<bool> R = readPod(NumRegions, false); !R)
      return R.takeError();
    Fragment.Regions.resize(NumRegions);
    for (FragmentRegion &Region : Fragment.Regions) {
      if (Expected<bool> R = readPod(Region.Kind, false); !R)
        return R.takeError();
      if (Expected<bool> R = readPod(Region.LineStart, false); !R)
        return R.takeError();
      if (Expected<bool> R = readPod(Region.ColumnStart, false); !R)
        return R.takeError();
      if (Expected<bool> R = readPod(Region.LineEnd, false); !R)
        return R.takeError();
      if (Expected<bool> R = readPod(Region.ColumnEnd, false); !R)
        return R.takeError();
      if (Expected<bool> R = readPod(Region.ExecutionCount, false); !R)
        return R.takeError();
      if (Expected<bool> R = readPod(Region.FalseExecutionCount, false); !R)
        return R.takeError();
      if (Expected<bool> R = readPod(Region.ConditionID, false); !R)
        return R.takeError();
      if (Expected<bool> R = readPod(Region.TrueConditionID, false); !R)
        return R.takeError();
      if (Expected<bool> R = readPod(Region.FalseConditionID, false); !R)
        return R.takeError();
      if (Expected<bool> R = readPod(Region.TrueFolded, false); !R)
        return R.takeError();
      if (Expected<bool> R = readPod(Region.FalseFolded, false); !R)
        return R.takeError();
    }
    return true;
  }

private:
  FragmentReader() = default;
};

Expected<std::unique_ptr<ToolOutputFile>> createTemporaryOutput() {
  int FD = -1;
  SmallString<128> Path;
  if (std::error_code EC = sys::fs::createTemporaryFile(
          "llvm-cov-covered-functions", "tmp", FD, Path))
    return errorCodeToError(EC);
  return std::make_unique<ToolOutputFile>(Path, FD);
}

struct RunCursor {
  std::unique_ptr<FragmentReader> Reader;
  FunctionFragment Current;
  bool HasCurrent = false;
};

template <typename ConsumerT>
Error mergeRuns(ArrayRef<ToolOutputFile *> Runs, ConsumerT Consume) {
  std::vector<RunCursor> Cursors;
  Cursors.reserve(Runs.size());
  for (ToolOutputFile *Run : Runs) {
    Expected<std::unique_ptr<FragmentReader>> Reader =
        FragmentReader::create(Run->getFilename());
    if (!Reader)
      return Reader.takeError();
    RunCursor Cursor{std::move(*Reader), FunctionFragment(), false};
    Expected<bool> HasRecord = Cursor.Reader->read(Cursor.Current);
    if (!HasRecord)
      return HasRecord.takeError();
    Cursor.HasCurrent = *HasRecord;
    Cursors.push_back(std::move(Cursor));
  }

  auto Compare = [&](size_t LHS, size_t RHS) {
    return Cursors[RHS].Current < Cursors[LHS].Current;
  };
  std::priority_queue<size_t, std::vector<size_t>, decltype(Compare)> Queue(
      Compare);
  for (size_t I = 0; I < Cursors.size(); ++I)
    if (Cursors[I].HasCurrent)
      Queue.push(I);

  while (!Queue.empty()) {
    size_t I = Queue.top();
    Queue.pop();
    FunctionFragment Current = std::move(Cursors[I].Current);
    Cursors[I].Current = FunctionFragment();
    Cursors[I].HasCurrent = false;
    Expected<bool> HasRecord = Cursors[I].Reader->read(Cursors[I].Current);
    if (!HasRecord)
      return HasRecord.takeError();
    Cursors[I].HasCurrent = *HasRecord;
    if (Cursors[I].HasCurrent)
      Queue.push(I);
    if (Error E = Consume(Current))
      return E;
  }
  return Error::success();
}

} // namespace

class CoverageExporterCoveredFunctions::Implementation {
  struct PathRemapping {
    std::string From;
    std::string To;
  };

  raw_ostream &OS;
  const CoverageFilters &FilenameFilters;
  const CoveredFunctionsExportOptions Options;
  std::vector<PathRemapping> PathRemappings;
  std::unique_ptr<ToolOutputFile> Spool;
  bool Finished = false;

  static std::string normalizedPath(StringRef Path, bool TrailingSeparator) {
    SmallString<256> Native(Path);
    sys::path::native(Native);
    sys::path::remove_dots(Native, true);
    if (TrailingSeparator && !Native.empty() &&
        !sys::path::is_separator(Native.back()))
      Native += sys::path::get_separator();
    return Native.str().str();
  }

  std::string remapPath(StringRef Path) const {
    std::string Native = normalizedPath(Path, false);
    for (const PathRemapping &Remapping : PathRemappings) {
      if (StringRef(Native).starts_with(Remapping.From))
        return Remapping.To +
               StringRef(Native).drop_front(Remapping.From.size()).str();
    }
    return Native;
  }

  Error ensureSpool() {
    if (Spool)
      return Error::success();
    Expected<std::unique_ptr<ToolOutputFile>> Output = createTemporaryOutput();
    if (!Output)
      return Output.takeError();
    Spool = std::move(*Output);
    return Error::success();
  }

  Expected<std::unique_ptr<ToolOutputFile>>
  writeSortedRun(std::vector<FunctionFragment> &Fragments) {
    llvm::sort(Fragments);
    Expected<std::unique_ptr<ToolOutputFile>> Run = createTemporaryOutput();
    if (!Run)
      return Run.takeError();
    for (const FunctionFragment &Fragment : Fragments)
      if (Error E = writeFragment((*Run)->os(), Fragment))
        return std::move(E);
    (*Run)->os().close();
    if ((*Run)->os().has_error())
      return errorCodeToError((*Run)->os().error());
    return std::move(*Run);
  }

  Expected<std::vector<std::unique_ptr<ToolOutputFile>>> createSortedRuns() {
    std::vector<std::unique_ptr<ToolOutputFile>> Runs;
    if (!Spool)
      return Runs;
    Spool->os().close();
    if (Spool->os().has_error())
      return errorCodeToError(Spool->os().error());

    Expected<std::unique_ptr<FragmentReader>> Reader =
        FragmentReader::create(Spool->getFilename());
    if (!Reader)
      return Reader.takeError();

    std::vector<FunctionFragment> Fragments;
    size_t Memory = 0;
    for (;;) {
      FunctionFragment Fragment;
      Expected<bool> HasRecord = (*Reader)->read(Fragment);
      if (!HasRecord)
        return HasRecord.takeError();
      if (!*HasRecord)
        break;
      Memory += Fragment.memorySize();
      Fragments.push_back(std::move(Fragment));
      if (Memory < SortMemoryLimit)
        continue;
      Expected<std::unique_ptr<ToolOutputFile>> Run = writeSortedRun(Fragments);
      if (!Run)
        return Run.takeError();
      Runs.push_back(std::move(*Run));
      Fragments.clear();
      Memory = 0;
    }
    if (!Fragments.empty()) {
      Expected<std::unique_ptr<ToolOutputFile>> Run = writeSortedRun(Fragments);
      if (!Run)
        return Run.takeError();
      Runs.push_back(std::move(*Run));
    }
    return Runs;
  }

  Expected<std::vector<std::unique_ptr<ToolOutputFile>>>
  reduceRuns(std::vector<std::unique_ptr<ToolOutputFile>> Runs) {
    while (Runs.size() > MergeFanIn) {
      std::vector<std::unique_ptr<ToolOutputFile>> MergedRuns;
      for (size_t Begin = 0; Begin < Runs.size(); Begin += MergeFanIn) {
        size_t End = std::min(Runs.size(), Begin + MergeFanIn);
        Expected<std::unique_ptr<ToolOutputFile>> Output =
            createTemporaryOutput();
        if (!Output)
          return Output.takeError();
        SmallVector<ToolOutputFile *, MergeFanIn> Inputs;
        for (size_t I = Begin; I < End; ++I)
          Inputs.push_back(Runs[I].get());
        if (Error E = mergeRuns(Inputs, [&](const FunctionFragment &Fragment) {
              return writeFragment((*Output)->os(), Fragment);
            }))
          return std::move(E);
        (*Output)->os().close();
        if ((*Output)->os().has_error())
          return errorCodeToError((*Output)->os().error());
        MergedRuns.push_back(std::move(*Output));
      }
      Runs = std::move(MergedRuns);
    }
    return Runs;
  }

  Error renderRuns(ArrayRef<ToolOutputFile *> Runs) {
    OS << "covered-functions-format 3\nmode - "
       << (Options.ExportMode == CoveredFunctionsExportOptions::Mode::Baseline
               ? "baseline"
               : "execution")
       << "\nfeatures - branches=" << unsigned(Options.IncludeBranches)
       << " mcdc=" << unsigned(Options.IncludeMCDC) << '\n';
    std::string CurrentRootFilename;
    std::string CurrentRawFunctionName;
    uint64_t CurrentFunctionHash = 0;
    uint32_t CurrentBodyLine = 0;
    uint32_t CurrentBodyColumn = 0;
    bool HasCurrentFunction = false;
    std::string CurrentFragmentFilename;
    bool CurrentFragmentIsRoot = false;
    bool HasCurrentFragment = false;
    uint32_t CurrentExpansionOrdinal = 0;
    bool HasCurrentExpansion = false;

    Error E = mergeRuns(Runs, [&](const FunctionFragment &Fragment) -> Error {
      bool StartsFunction =
          !HasCurrentFunction ||
          std::tie(Fragment.CoverageRootFilename, Fragment.CoverageBodyLine,
                   Fragment.CoverageBodyColumn, Fragment.RawFunctionName,
                   Fragment.FunctionHash) !=
              std::tie(CurrentRootFilename, CurrentBodyLine, CurrentBodyColumn,
                       CurrentRawFunctionName, CurrentFunctionHash);
      if (StartsFunction) {
        if (HasCurrentExpansion) {
          OS << "end-expansion\n";
          HasCurrentExpansion = false;
        }
        if (HasCurrentFragment) {
          OS << "end-fragment\n";
          HasCurrentFragment = false;
        }
        if (HasCurrentFunction)
          OS << "end-function\n";
        HasCurrentFunction = true;
        CurrentRootFilename = Fragment.CoverageRootFilename;
        CurrentRawFunctionName = Fragment.RawFunctionName;
        CurrentFunctionHash = Fragment.FunctionHash;
        CurrentBodyLine = Fragment.CoverageBodyLine;
        CurrentBodyColumn = Fragment.CoverageBodyColumn;

        double OverallPercent = Fragment.OverallTotalCodeRegions
                                    ? 100.0 * Fragment.OverallHitCodeRegions /
                                          Fragment.OverallTotalCodeRegions
                                    : 0.0;
        OS << "\nfunction - \"";
        printEscapedString(Fragment.DisplayName, OS);
        OS << "\"\nfunction-id - \"";
        printEscapedString(Fragment.RawFunctionName, OS);
        OS << "\" " << format_hex(Fragment.FunctionHash, 18) << '\n';
        OS << "entry-count - " << Fragment.EntryCount << '\n';
        OS << "overall-coverage - " << Fragment.OverallTotalCodeRegions << ' '
           << Fragment.OverallHitCodeRegions << ' '
           << format("%.2f", OverallPercent) << '\n';
        OS << "coverage-body-at - \"";
        printEscapedString(Fragment.CoverageRootFilename, OS);
        OS << "\" " << Fragment.CoverageBodyLine << ' '
           << Fragment.CoverageBodyColumn << '\n';
      }

      bool StartsFragment = !HasCurrentFragment || StartsFunction ||
                            Fragment.Filename != CurrentFragmentFilename ||
                            Fragment.IsRootFragment != CurrentFragmentIsRoot;
      if (StartsFragment) {
        if (HasCurrentExpansion) {
          OS << "end-expansion\n";
          HasCurrentExpansion = false;
        }
        if (HasCurrentFragment)
          OS << "end-fragment\n";
        HasCurrentFragment = true;
        CurrentFragmentFilename = Fragment.Filename;
        CurrentFragmentIsRoot = Fragment.IsRootFragment;

        double Percent = Fragment.FragmentGroupTotalCodeRegions
                             ? 100.0 * Fragment.FragmentGroupHitCodeRegions /
                                   Fragment.FragmentGroupTotalCodeRegions
                             : 0.0;
        OS << "fragment - "
           << (Fragment.IsRootFragment ? "root \"" : "expansion \"");
        printEscapedString(Fragment.Filename, OS);
        OS << "\" " << Fragment.FragmentGroupTotalCodeRegions << ' '
           << Fragment.FragmentGroupHitCodeRegions << ' '
           << format("%.2f", Percent) << '\n';
      }

      if (!Fragment.IsRootFragment &&
          (!HasCurrentExpansion ||
           Fragment.FragmentOrdinal != CurrentExpansionOrdinal)) {
        if (HasCurrentExpansion)
          OS << "end-expansion\n";
        HasCurrentExpansion = true;
        CurrentExpansionOrdinal = Fragment.FragmentOrdinal;
        double Percent = Fragment.FragmentTotalCodeRegions
                             ? 100.0 * Fragment.FragmentHitCodeRegions /
                                   Fragment.FragmentTotalCodeRegions
                             : 0.0;
        OS << "expansion-at - \"";
        printEscapedString(Fragment.ExpansionSiteFilename, OS);
        OS << "\" " << Fragment.ExpansionSiteLine << ' '
           << Fragment.ExpansionSiteColumn << ' '
           << Fragment.FragmentTotalCodeRegions << ' '
           << Fragment.FragmentHitCodeRegions << ' ' << format("%.2f", Percent)
           << '\n';
      }

      for (const FragmentRegion &Region : Fragment.Regions) {
        if (Region.Kind <= 4) {
          OS << "region - " << Region.Kind << ' ' << Region.LineStart << ' '
             << Region.ColumnStart << ' ' << Region.LineEnd << ' '
             << Region.ColumnEnd << ' ' << Region.ExecutionCount << '\n';
          continue;
        }
        if (Region.Kind == ReportBranchRegionKind) {
          OS << "branch - " << Region.LineStart << ' ' << Region.ColumnStart
             << ' ' << Region.LineEnd << ' ' << Region.ColumnEnd << ' '
             << Region.ExecutionCount << ' ' << Region.FalseExecutionCount
             << ' ' << unsigned(Region.TrueFolded) << ' '
             << unsigned(Region.FalseFolded) << '\n';
          continue;
        }
        if (Region.Kind == ReportMCDCDecisionRegionKind) {
          OS << "mcdc-decision - " << Region.LineStart << ' '
             << Region.ColumnStart << ' ' << Region.LineEnd << ' '
             << Region.ColumnEnd << ' ' << Region.ExecutionCount << ' '
             << Region.FalseExecutionCount << ' ' << Region.ConditionID << ' '
             << Region.TrueConditionID << ' ' << Region.FalseConditionID
             << '\n';
          continue;
        }
        assert(Region.Kind == ReportMCDCBranchRegionKind);
        OS << "mcdc-branch - " << Region.LineStart << ' ' << Region.ColumnStart
           << ' ' << Region.LineEnd << ' ' << Region.ColumnEnd << ' '
           << Region.ExecutionCount << ' ' << Region.FalseExecutionCount << ' '
           << Region.ConditionID << ' ' << Region.TrueConditionID << ' '
           << Region.FalseConditionID << ' ' << unsigned(Region.TrueFolded)
           << ' ' << unsigned(Region.FalseFolded) << '\n';
      }
      return Error::success();
    });
    if (E)
      return E;
    if (HasCurrentExpansion)
      OS << "end-expansion\n";
    if (HasCurrentFragment)
      OS << "end-fragment\n";
    if (HasCurrentFunction)
      OS << "end-function\n";
    return Error::success();
  }

public:
  Implementation(raw_ostream &OS,
                 ArrayRef<std::pair<std::string, std::string>> Remappings,
                 const CoverageFilters &FilenameFilters,
                 CoveredFunctionsExportOptions Options)
      : OS(OS), FilenameFilters(FilenameFilters), Options(Options) {
    PathRemappings.reserve(Remappings.size());
    for (const auto &[From, To] : Remappings)
      PathRemappings.push_back(
          {normalizedPath(From, true), normalizedPath(To, true)});
  }

  Error consume(StringRef RawFunctionName, uint64_t FunctionHash,
                FunctionRecord &&Function) {
    if (Finished)
      return createStringError(errc::invalid_argument,
                               "covered-functions exporter is finished");
    if (Error E = ensureSpool())
      return E;

    struct ExpansionSite {
      unsigned ParentFileID;
      uint32_t Line;
      uint32_t Column;
    };

    BitVector UsedFileIDs(Function.Filenames.size());
    BitVector ExpandedFileIDs(Function.Filenames.size());
    std::vector<std::optional<ExpansionSite>> ExpansionSites(
        Function.Filenames.size());
    for (const CountedRegion &Region : Function.CountedRegions) {
      if (Region.FileID >= Function.Filenames.size())
        return make_error<CoverageMapError>(coveragemap_error::malformed,
                                            "invalid coverage FileID");
      UsedFileIDs.set(Region.FileID);
      if (Region.Kind != CounterMappingRegion::ExpansionRegion)
        continue;
      if (Region.ExpandedFileID >= Function.Filenames.size())
        return make_error<CoverageMapError>(coveragemap_error::malformed,
                                            "invalid expanded coverage FileID");
      if (ExpansionSites[Region.ExpandedFileID])
        return make_error<CoverageMapError>(
            coveragemap_error::malformed,
            "coverage FileID has multiple expansion sites");
      ExpansionSites[Region.ExpandedFileID] =
          ExpansionSite{Region.FileID, Region.LineStart, Region.ColumnStart};
      ExpandedFileIDs.set(Region.ExpandedFileID);
    }

    std::optional<unsigned> CoverageRootFileID;
    for (unsigned FileID = 0; FileID != Function.Filenames.size(); ++FileID) {
      if (UsedFileIDs.test(FileID) && !ExpandedFileIDs.test(FileID)) {
        CoverageRootFileID = FileID;
        break;
      }
    }
    if (!CoverageRootFileID)
      return make_error<CoverageMapError>(
          coveragemap_error::malformed,
          "coverage function has no non-expanded root file");

    const CountedRegion *CoverageBodyRegion = nullptr;
    const CountedRegion *FallbackBodyRegion = nullptr;
    for (const CountedRegion &Region : Function.CountedRegions) {
      if (Region.FileID != *CoverageRootFileID)
        continue;
      if (!FallbackBodyRegion)
        FallbackBodyRegion = &Region;
      if (Region.Kind == CounterMappingRegion::CodeRegion) {
        CoverageBodyRegion = &Region;
        break;
      }
    }
    if (!CoverageBodyRegion)
      CoverageBodyRegion = FallbackBodyRegion;
    if (!CoverageBodyRegion)
      return make_error<CoverageMapError>(
          coveragemap_error::malformed,
          "coverage root file has no reportable regions");

    std::string CoverageRootFilename =
        remapPath(Function.Filenames[*CoverageRootFileID]);
    uint64_t OverallTotalCodeRegions = 0;
    uint64_t OverallHitCodeRegions = 0;
    for (const CountedRegion &Region : Function.CountedRegions) {
      if (Region.Kind != CounterMappingRegion::CodeRegion)
        continue;
      ++OverallTotalCodeRegions;
      OverallHitCodeRegions += Region.ExecutionCount != 0;
    }

    std::vector<std::unique_ptr<FunctionFragment>> Fragments(
        Function.Filenames.size());
    auto getOrCreateFragment = [&](const CounterMappingRegion &Region)
        -> Expected<FunctionFragment *> {
      if (Region.FileID >= Function.Filenames.size())
        return make_error<CoverageMapError>(coveragemap_error::malformed,
                                            "invalid coverage FileID");
      StringRef Filename = Function.Filenames[Region.FileID];
      if (FilenameFilters.matchesFilename(Filename))
        return nullptr;
      std::unique_ptr<FunctionFragment> &FragmentStorage =
          Fragments[Region.FileID];
      if (!FragmentStorage) {
        FragmentStorage = std::make_unique<FunctionFragment>();
        FunctionFragment &Fragment = *FragmentStorage;
        Fragment.Filename = remapPath(Filename);
        Fragment.CoverageRootFilename = CoverageRootFilename;
        Fragment.DisplayName = Function.Name;
        Fragment.RawFunctionName = RawFunctionName.str();
        Fragment.FunctionHash = FunctionHash;
        Fragment.EntryCount = Function.ExecutionCount;
        Fragment.OverallTotalCodeRegions = OverallTotalCodeRegions;
        Fragment.OverallHitCodeRegions = OverallHitCodeRegions;
        Fragment.CoverageBodyLine = CoverageBodyRegion->LineStart;
        Fragment.CoverageBodyColumn = CoverageBodyRegion->ColumnStart;
        Fragment.FragmentOrdinal = Region.FileID;
        Fragment.IsRootFragment = !ExpandedFileIDs.test(Region.FileID);
        if (!Fragment.IsRootFragment) {
          const std::optional<ExpansionSite> &Site =
              ExpansionSites[Region.FileID];
          if (!Site)
            return make_error<CoverageMapError>(
                coveragemap_error::malformed,
                "expanded coverage FileID has no expansion site");
          Fragment.ExpansionSiteFilename =
              remapPath(Function.Filenames[Site->ParentFileID]);
          Fragment.ExpansionSiteLine = Site->Line;
          Fragment.ExpansionSiteColumn = Site->Column;
        }
      }
      return FragmentStorage.get();
    };

    for (const CountedRegion &Region : Function.CountedRegions) {
      std::optional<uint32_t> ReportKind = getReportRegionKind(Region.Kind);
      if (!ReportKind)
        continue;
      Expected<FunctionFragment *> FragmentOrErr = getOrCreateFragment(Region);
      if (!FragmentOrErr)
        return FragmentOrErr.takeError();
      FunctionFragment *Fragment = *FragmentOrErr;
      if (!Fragment)
        continue;
      Fragment->Regions.push_back({*ReportKind, Region.LineStart,
                                   Region.ColumnStart, Region.LineEnd,
                                   Region.ColumnEnd, Region.ExecutionCount});
      if (Region.Kind == CounterMappingRegion::CodeRegion &&
          (Fragment->SortLine == 0 ||
           std::tie(Region.LineStart, Region.ColumnStart) <
               std::tie(Fragment->SortLine, Fragment->SortColumn))) {
        Fragment->SortLine = Region.LineStart;
        Fragment->SortColumn = Region.ColumnStart;
      }
    }

    for (const CountedRegion &Region : Function.CountedBranchRegions) {
      Expected<FunctionFragment *> FragmentOrErr = getOrCreateFragment(Region);
      if (!FragmentOrErr)
        return FragmentOrErr.takeError();
      FunctionFragment *Fragment = *FragmentOrErr;
      if (!Fragment)
        continue;

      auto appendBranch = [&](uint32_t ReportKind) {
        FragmentRegion OutputRegion{ReportKind,         Region.LineStart,
                                    Region.ColumnStart, Region.LineEnd,
                                    Region.ColumnEnd,   Region.ExecutionCount};
        OutputRegion.FalseExecutionCount = Region.FalseExecutionCount;
        OutputRegion.TrueFolded = Region.TrueFolded;
        OutputRegion.FalseFolded = Region.FalseFolded;
        if (ReportKind == ReportMCDCBranchRegionKind) {
          const mcdc::BranchParameters &Params = Region.getBranchParams();
          OutputRegion.ConditionID = Params.ID;
          OutputRegion.FalseConditionID = Params.Conds[0];
          OutputRegion.TrueConditionID = Params.Conds[1];
        }
        Fragment->Regions.push_back(OutputRegion);
      };

      if (Options.IncludeBranches)
        appendBranch(ReportBranchRegionKind);
      if (Region.Kind == CounterMappingRegion::MCDCBranchRegion &&
          Options.IncludeMCDC)
        appendBranch(ReportMCDCBranchRegionKind);
    }

    if (Options.IncludeMCDC)
      for (MCDCRecord &Record : Function.MCDCRecords) {
        const CounterMappingRegion &Region = Record.getDecisionRegion();
        Expected<FunctionFragment *> FragmentOrErr =
            getOrCreateFragment(Region);
        if (!FragmentOrErr)
          return FragmentOrErr.takeError();
        FunctionFragment *Fragment = *FragmentOrErr;
        if (!Fragment)
          continue;

        auto [TrueDecisions, FalseDecisions] = Record.getDecisions();
        unsigned CoveredConditions = 0;
        unsigned FoldedConditions = 0;
        for (unsigned I = 0; I != Record.getNumConditions(); ++I) {
          if (Record.isCondFolded(I))
            ++FoldedConditions;
          else if (Record.isConditionIndependencePairCovered(I))
            ++CoveredConditions;
        }
        FragmentRegion OutputRegion{ReportMCDCDecisionRegionKind,
                                    Region.LineStart,
                                    Region.ColumnStart,
                                    Region.LineEnd,
                                    Region.ColumnEnd,
                                    TrueDecisions};
        OutputRegion.FalseExecutionCount = FalseDecisions;
        OutputRegion.ConditionID = Record.getNumConditions();
        OutputRegion.TrueConditionID = CoveredConditions;
        OutputRegion.FalseConditionID = FoldedConditions;
        Fragment->Regions.push_back(OutputRegion);
      }

    if (Options.ExportMode == CoveredFunctionsExportOptions::Mode::Execution)
      for (std::unique_ptr<FunctionFragment> &Fragment : Fragments) {
        if (!Fragment)
          continue;
        bool HasCoveredCodeRegion =
            llvm::any_of(Fragment->Regions, [](const FragmentRegion &Region) {
              return Region.Kind == ReportCodeRegionKind &&
                     Region.ExecutionCount != 0;
            });
        if (!HasCoveredCodeRegion)
          Fragment.reset();
      }
    if (llvm::none_of(Fragments,
                      [](const auto &Fragment) { return bool(Fragment); }))
      return Error::success();

    StringMap<std::pair<uint64_t, uint64_t>> RootGroupTotals;
    StringMap<std::pair<uint64_t, uint64_t>> ExpansionGroupTotals;
    for (std::unique_ptr<FunctionFragment> &FragmentStorage : Fragments) {
      if (!FragmentStorage)
        continue;
      FunctionFragment &Fragment = *FragmentStorage;
      llvm::sort(Fragment.Regions);
      for (const FragmentRegion &Region : Fragment.Regions) {
        if (Region.Kind != ReportCodeRegionKind)
          continue;
        ++Fragment.FragmentTotalCodeRegions;
        Fragment.FragmentHitCodeRegions += Region.ExecutionCount != 0;
      }
      auto &GroupTotals =
          Fragment.IsRootFragment ? RootGroupTotals : ExpansionGroupTotals;
      auto &Totals = GroupTotals[Fragment.Filename];
      Totals.first += Fragment.FragmentTotalCodeRegions;
      Totals.second += Fragment.FragmentHitCodeRegions;
    }

    for (std::unique_ptr<FunctionFragment> &FragmentStorage : Fragments) {
      if (!FragmentStorage)
        continue;
      FunctionFragment &Fragment = *FragmentStorage;
      auto &GroupTotals =
          Fragment.IsRootFragment ? RootGroupTotals : ExpansionGroupTotals;
      const auto &Totals = GroupTotals.find(Fragment.Filename)->second;
      Fragment.FragmentGroupTotalCodeRegions = Totals.first;
      Fragment.FragmentGroupHitCodeRegions = Totals.second;
      std::vector<FragmentRegion> Regions = std::move(Fragment.Regions);
      for (size_t Begin = 0; Begin < Regions.size();
           Begin += MaxRegionsPerFragmentChunk) {
        size_t End =
            std::min(Regions.size(), Begin + MaxRegionsPerFragmentChunk);
        size_t ChunkIndex = Begin / MaxRegionsPerFragmentChunk;
        if (ChunkIndex > std::numeric_limits<uint32_t>::max())
          return createStringError(errc::file_too_large,
                                   "covered-functions fragment is too large");
        Fragment.ChunkIndex = ChunkIndex;
        Fragment.Regions.assign(Regions.begin() + Begin, Regions.begin() + End);
        if (Error E = writeFragment(Spool->os(), Fragment))
          return E;
      }
    }
    return Error::success();
  }

  Error finish() {
    if (Finished)
      return createStringError(errc::invalid_argument,
                               "covered-functions exporter finished twice");
    Finished = true;
    Expected<std::vector<std::unique_ptr<ToolOutputFile>>> RunsOrErr =
        createSortedRuns();
    if (!RunsOrErr)
      return RunsOrErr.takeError();
    Expected<std::vector<std::unique_ptr<ToolOutputFile>>> ReducedOrErr =
        reduceRuns(std::move(*RunsOrErr));
    if (!ReducedOrErr)
      return ReducedOrErr.takeError();
    SmallVector<ToolOutputFile *, MergeFanIn> Runs;
    for (const std::unique_ptr<ToolOutputFile> &Run : *ReducedOrErr)
      Runs.push_back(Run.get());
    return renderRuns(Runs);
  }
};

CoverageExporterCoveredFunctions::CoverageExporterCoveredFunctions(
    raw_ostream &OS,
    ArrayRef<std::pair<std::string, std::string>> PathRemappings,
    const CoverageFilters &FilenameFilters,
    CoveredFunctionsExportOptions Options)
    : Impl(std::make_unique<Implementation>(OS, PathRemappings, FilenameFilters,
                                            Options)) {}

CoverageExporterCoveredFunctions::~CoverageExporterCoveredFunctions() = default;

Error CoverageExporterCoveredFunctions::consume(StringRef RawFunctionName,
                                                uint64_t FunctionHash,
                                                FunctionRecord &&Function) {
  return Impl->consume(RawFunctionName, FunctionHash, std::move(Function));
}

Error CoverageExporterCoveredFunctions::finish() { return Impl->finish(); }

//===- CoverageExporterCoveredFunctions.cpp - Text coverage export -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CoverageExporterCoveredFunctions.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace llvm;
using namespace llvm::coverage;

namespace {

constexpr unsigned NumSourcePartitions = 128;
constexpr size_t ReadBufferSize = 64 * 1024;
constexpr size_t SourcePartitionMemoryLimit = 64 * 1024 * 1024;
constexpr unsigned MaxSourcePartitionDepth = 8;

// Report and spool records use stable one-based region kind values.
// Non-countable skipped and gap regions are intentionally omitted.
constexpr uint32_t ReportCodeRegionKind = 1;
constexpr uint32_t ReportExpansionRegionKind = 2;

struct StructuralRegion {
  CounterMappingRegion::RegionKind Kind;
  LineColPair Start;
  LineColPair End;
  Counter StaticCounter;
  uint64_t ExecutionCount;
  unsigned MappingOrdinal;
  bool HasMultipleCounters = false;
};

struct StructuralSegment {
  LineColPair Start;
  LineColPair End;
  Counter StaticCounter;
  uint64_t ExecutionCount;
  bool HasMultipleCounters;
};

unsigned structuralKindPrecedence(CounterMappingRegion::RegionKind Kind) {
  switch (Kind) {
  case CounterMappingRegion::CodeRegion:
    return 0;
  case CounterMappingRegion::SkippedRegion:
    return 1;
  case CounterMappingRegion::GapRegion:
    return 2;
  case CounterMappingRegion::ExpansionRegion:
  case CounterMappingRegion::BranchRegion:
  case CounterMappingRegion::MCDCDecisionRegion:
  case CounterMappingRegion::MCDCBranchRegion:
    llvm_unreachable("non-structural coverage region");
  }
  llvm_unreachable("unknown coverage region kind");
}

bool structuralRegionLess(const StructuralRegion &LHS,
                          const StructuralRegion &RHS) {
  if (LHS.Start != RHS.Start)
    return LHS.Start < RHS.Start;
  if (LHS.End != RHS.End)
    return RHS.End < LHS.End;
  if (LHS.Kind != RHS.Kind)
    return structuralKindPrecedence(LHS.Kind) <
           structuralKindPrecedence(RHS.Kind);
  return LHS.MappingOrdinal < RHS.MappingOrdinal;
}

bool sameStructuralCoordinates(const StructuralRegion &LHS,
                               const StructuralRegion &RHS) {
  return LHS.Start == RHS.Start && LHS.End == RHS.End;
}

bool haveSameStaticCounter(const StructuralRegion &LHS,
                           const StructuralRegion &RHS) {
  return !LHS.HasMultipleCounters && !RHS.HasMultipleCounters &&
         LHS.Kind == CounterMappingRegion::CodeRegion &&
         RHS.Kind == CounterMappingRegion::CodeRegion &&
         LHS.StaticCounter == RHS.StaticCounter;
}

std::optional<std::vector<StructuralSegment>>
buildNestedStructuralSegments(ArrayRef<StructuralRegion> Regions) {
  SmallVector<LineColPair, 8> EndStack;
  for (const StructuralRegion &Region : Regions) {
    while (!EndStack.empty() && EndStack.back() <= Region.Start)
      EndStack.pop_back();
    if (!EndStack.empty() && EndStack.back() < Region.End)
      return std::nullopt;
    EndStack.push_back(Region.End);
  }

  SmallVector<unsigned, 8> Active;
  size_t StartIndex = 0;
  std::optional<LineColPair> PreviousLoc;
  std::optional<unsigned> LastOwner;
  std::vector<StructuralSegment> Segments;
  while (StartIndex != Regions.size() || !Active.empty()) {
    std::optional<LineColPair> EventLoc;
    if (StartIndex != Regions.size())
      EventLoc = Regions[StartIndex].Start;
    if (!Active.empty())
      EventLoc = EventLoc ? std::min(*EventLoc, Regions[Active.back()].End)
                          : Regions[Active.back()].End;

    if (PreviousLoc && *PreviousLoc < *EventLoc && !Active.empty()) {
      unsigned OwnerIndex = Active.back();
      const StructuralRegion &Owner = Regions[OwnerIndex];
      if (Owner.Kind == CounterMappingRegion::CodeRegion) {
        if (LastOwner && haveSameStaticCounter(Regions[*LastOwner], Owner) &&
            !Segments.empty() && Segments.back().End == *PreviousLoc)
          Segments.back().End = *EventLoc;
        else
          Segments.push_back({*PreviousLoc, *EventLoc, Owner.StaticCounter,
                              Owner.ExecutionCount, Owner.HasMultipleCounters});
        LastOwner = OwnerIndex;
      } else {
        LastOwner.reset();
      }
    }

    while (!Active.empty() && Regions[Active.back()].End == *EventLoc)
      Active.pop_back();
    while (StartIndex != Regions.size() &&
           Regions[StartIndex].Start == *EventLoc)
      Active.push_back(StartIndex++);
    PreviousLoc = *EventLoc;
  }
  return Segments;
}

std::vector<StructuralSegment>
buildStructuralSegments(std::vector<StructuralRegion> Regions) {
  // A macro-generated function can have no source-width executable segment in
  // its root file. Its code regions are still backed by counters and are the
  // only way txtcvrg can report whether the function executed. Keep one
  // fallback point per code region when normalization produces no ordinary
  // executable segment. Using the original start location preserves stable
  // identity without reintroducing overlapping ranges.
  if (!llvm::is_sorted(Regions, structuralRegionLess))
    llvm::sort(Regions, structuralRegionLess);
  std::vector<StructuralSegment> FallbackCounterPoints;
  for (const StructuralRegion &Region : Regions) {
    if (Region.Kind == CounterMappingRegion::CodeRegion)
      FallbackCounterPoints.push_back(
          {Region.Start, Region.Start, Region.StaticCounter,
           Region.ExecutionCount, Region.HasMultipleCounters});
  }

  llvm::erase_if(Regions, [](const StructuralRegion &Region) {
    return !(Region.Start < Region.End);
  });

  std::vector<StructuralRegion> Combined;
  Combined.reserve(Regions.size());
  for (StructuralRegion &Region : Regions) {
    if (!Combined.empty() &&
        sameStructuralCoordinates(Combined.back(), Region)) {
      // A gap or skipped region masks an executable region at the same exact
      // coordinates. Among duplicates of the selected kind, aggregate counts
      // to match LLVM's source-rendering semantics.
      if (Combined.back().Kind == Region.Kind) {
        Combined.back().ExecutionCount = SaturatingAdd(
            Combined.back().ExecutionCount, Region.ExecutionCount);
        if (Combined.back().StaticCounter != Region.StaticCounter)
          Combined.back().HasMultipleCounters = true;
      } else if (structuralKindPrecedence(Combined.back().Kind) <
                 structuralKindPrecedence(Region.Kind)) {
        Combined.back() = Region;
      }
      continue;
    }
    Combined.push_back(Region);
  }

  if (auto Segments = buildNestedStructuralSegments(Combined)) {
    if (Segments->empty())
      return FallbackCounterPoints;
    return std::move(*Segments);
  }

  // Coverage mappings are normally properly nested. Keep a general ordered
  // active-set fallback so unusual crossing ranges still normalize safely.
  std::vector<unsigned> Ends;
  Ends.reserve(Combined.size());
  for (unsigned I = 0; I != Combined.size(); ++I) {
    Ends.push_back(I);
  }
  llvm::sort(Ends, [&](unsigned LHS, unsigned RHS) {
    if (Combined[LHS].End != Combined[RHS].End)
      return Combined[LHS].End < Combined[RHS].End;
    return structuralRegionLess(Combined[LHS], Combined[RHS]);
  });

  struct ActiveRegionLess {
    const std::vector<StructuralRegion> *Regions;

    bool operator()(unsigned LHS, unsigned RHS) const {
      const StructuralRegion &L = (*Regions)[LHS];
      const StructuralRegion &R = (*Regions)[RHS];
      if (L.Start != R.Start)
        return R.Start < L.Start;
      if (L.End != R.End)
        return L.End < R.End;
      if (L.Kind != R.Kind)
        return structuralKindPrecedence(L.Kind) <
               structuralKindPrecedence(R.Kind);
      return L.MappingOrdinal < R.MappingOrdinal;
    }
  };

  std::set<unsigned, ActiveRegionLess> Active(ActiveRegionLess{&Combined});
  size_t StartIndex = 0;
  size_t EndIndex = 0;
  std::vector<StructuralSegment> Segments;
  std::optional<unsigned> LastOwner;
  while (StartIndex != Combined.size() || EndIndex != Ends.size()) {
    LineColPair Loc;
    if (StartIndex == Combined.size())
      Loc = Combined[Ends[EndIndex]].End;
    else if (EndIndex == Ends.size())
      Loc = Combined[StartIndex].Start;
    else
      Loc = std::min(Combined[StartIndex].Start, Combined[Ends[EndIndex]].End);
    while (EndIndex != Ends.size() && Combined[Ends[EndIndex]].End <= Loc) {
      Active.erase(Ends[EndIndex]);
      ++EndIndex;
    }
    while (StartIndex != Combined.size() && Combined[StartIndex].Start == Loc) {
      Active.insert(StartIndex);
      ++StartIndex;
    }

    std::optional<LineColPair> NextLoc;
    if (StartIndex != Combined.size())
      NextLoc = Combined[StartIndex].Start;
    if (EndIndex != Ends.size())
      NextLoc = NextLoc ? std::min(*NextLoc, Combined[Ends[EndIndex]].End)
                        : Combined[Ends[EndIndex]].End;
    if (!NextLoc)
      break;
    if (Active.empty()) {
      LastOwner.reset();
      continue;
    }
    unsigned OwnerIndex = *Active.begin();
    const StructuralRegion &Owner = Combined[OwnerIndex];
    if (Owner.Kind != CounterMappingRegion::CodeRegion) {
      LastOwner.reset();
      continue;
    }
    if (LastOwner && haveSameStaticCounter(Combined[*LastOwner], Owner) &&
        !Segments.empty() && Segments.back().End == Loc)
      Segments.back().End = *NextLoc;
    else
      Segments.push_back({Loc, *NextLoc, Owner.StaticCounter,
                          Owner.ExecutionCount, Owner.HasMultipleCounters});
    LastOwner = OwnerIndex;
  }
  if (Segments.empty())
    return FallbackCounterPoints;
  return Segments;
}

void retainOneSegmentPerCounter(std::vector<StructuralSegment> &Segments) {
  std::set<Counter> SeenCounters;
  std::optional<LineColPair> PreviousEnd;
  llvm::erase_if(Segments, [&](const StructuralSegment &Segment) {
    // Fallback counter points represent distinct raw code records. Keep them
    // all so function totals and hit state are not changed by deduplication.
    if (Segment.Start == Segment.End)
      return false;

    // Counter expressions may legitimately recur in a later source range.
    // Only deduplicate within one uninterrupted normalization run, where a
    // broad region is split and then resumes around a nested region (A-B-A).
    if (!PreviousEnd || *PreviousEnd != Segment.Start)
      SeenCounters.clear();
    PreviousEnd = Segment.End;

    // A constant-zero counter does not identify a unique executable block:
    // unrelated unreachable regions all use it. Exact-overlap aggregation can
    // also represent several counters, so keep those structural ranges.
    if (Segment.StaticCounter.isZero() || Segment.HasMultipleCounters)
      return false;
    return !SeenCounters.insert(Segment.StaticCounter).second;
  });
}

std::optional<uint32_t>
getReportRegionKind(CounterMappingRegion::RegionKind Kind) {
  switch (Kind) {
  case CounterMappingRegion::CodeRegion:
    return ReportCodeRegionKind;
  case CounterMappingRegion::ExpansionRegion:
    return ReportExpansionRegionKind;
  case CounterMappingRegion::SkippedRegion:
  case CounterMappingRegion::GapRegion:
  case CounterMappingRegion::BranchRegion:
  case CounterMappingRegion::MCDCDecisionRegion:
  case CounterMappingRegion::MCDCBranchRegion:
    return std::nullopt;
  }
  llvm_unreachable("unknown coverage region kind");
}

template <typename T> void writePod(raw_ostream &OS, const T &Value) {
  OS.write(reinterpret_cast<const char *>(&Value), sizeof(Value));
}

struct SourceRegion {
  uint32_t FilenameID;
  uint32_t Kind;
  uint32_t LineStart;
  uint32_t ColumnStart;
  uint32_t LineEnd;
  uint32_t ColumnEnd;
  uint64_t ExecutionCount;
};

std::vector<SourceRegion>
buildAggregatedSourceSegments(ArrayRef<SourceRegion> Regions) {
  struct Event {
    LineColPair Loc;
    unsigned RegionIndex;
    bool IsStart;
  };

  std::vector<Event> Events;
  Events.reserve(2 * Regions.size());
  for (unsigned I = 0; I != Regions.size(); ++I) {
    const SourceRegion &Region = Regions[I];
    LineColPair Start{Region.LineStart, Region.ColumnStart};
    LineColPair End{Region.LineEnd, Region.ColumnEnd};
    if (!(Start < End))
      continue;
    Events.push_back({Start, I, true});
    Events.push_back({End, I, false});
  }
  llvm::sort(Events, [](const Event &LHS, const Event &RHS) {
    return std::tie(LHS.Loc, LHS.IsStart, LHS.RegionIndex) <
           std::tie(RHS.Loc, RHS.IsStart, RHS.RegionIndex);
  });

  uint64_t CountLow = 0;
  uint64_t CountHigh = 0;
  size_t ActiveRegions = 0;
  std::vector<SourceRegion> Segments;
  for (size_t I = 0; I != Events.size();) {
    LineColPair Loc = Events[I].Loc;
    size_t NextEvent = I;
    while (NextEvent != Events.size() && Events[NextEvent].Loc == Loc) {
      const Event &Current = Events[NextEvent++];
      uint64_t Count = Regions[Current.RegionIndex].ExecutionCount;
      uint64_t OldLow = CountLow;
      if (Current.IsStart) {
        CountLow += Count;
        CountHigh += CountLow < OldLow;
        ++ActiveRegions;
      } else {
        CountLow -= Count;
        CountHigh -= OldLow < Count;
        --ActiveRegions;
      }
    }
    if (NextEvent == Events.size() || ActiveRegions == 0) {
      I = NextEvent;
      continue;
    }
    LineColPair End = Events[NextEvent].Loc;
    uint64_t ExecutionCount =
        CountHigh ? std::numeric_limits<uint64_t>::max() : CountLow;
    Segments.push_back({Regions.front().FilenameID, ReportCodeRegionKind,
                        Loc.first, Loc.second, End.first, End.second,
                        ExecutionCount});
    I = NextEvent;
  }
  return Segments;
}

struct SourceFileRegions {
  std::unordered_map<std::string, SourceRegion> Regions;
};

Error writeSourceRegion(raw_ostream &OS, const SourceRegion &Region,
                        bool IsBaseline) {
  writePod(OS, Region.FilenameID);
  writePod(OS, Region.Kind);
  writePod(OS, Region.LineStart);
  writePod(OS, Region.ColumnStart);
  writePod(OS, Region.LineEnd);
  writePod(OS, Region.ColumnEnd);
  if (!IsBaseline)
    writePod(OS, Region.ExecutionCount);
  return Error::success();
}

class SourceRegionReader {
  sys::fs::file_t File;
  bool IsOpen = false;
  std::array<char, ReadBufferSize> ReadBuffer;
  size_t ReadBufferOffset = 0;
  size_t ReadBufferEnd = 0;

  Expected<bool> readBytes(MutableArrayRef<char> Buffer, bool AllowEOF) {
    size_t Offset = 0;
    while (Offset < Buffer.size()) {
      if (ReadBufferOffset == ReadBufferEnd) {
        Expected<size_t> Bytes = sys::fs::readNativeFile(File, ReadBuffer);
        if (!Bytes)
          return Bytes.takeError();
        if (*Bytes == 0) {
          if (AllowEOF && Offset == 0)
            return false;
          return createStringError(errc::io_error,
                                   "truncated text coverage spool file");
        }
        ReadBufferOffset = 0;
        ReadBufferEnd = *Bytes;
      }
      size_t BytesToCopy =
          std::min(Buffer.size() - Offset, ReadBufferEnd - ReadBufferOffset);
      std::memcpy(Buffer.data() + Offset, ReadBuffer.data() + ReadBufferOffset,
                  BytesToCopy);
      Offset += BytesToCopy;
      ReadBufferOffset += BytesToCopy;
    }
    return true;
  }

  template <typename T> Expected<bool> readPod(T &Value, bool AllowEOF) {
    return readBytes(
        MutableArrayRef(reinterpret_cast<char *>(&Value), sizeof(Value)),
        AllowEOF);
  }

  SourceRegionReader() = default;

public:
  static Expected<std::unique_ptr<SourceRegionReader>> create(StringRef Path) {
    Expected<sys::fs::file_t> FileOrErr = sys::fs::openNativeFileForRead(Path);
    if (!FileOrErr)
      return FileOrErr.takeError();
    auto Reader = std::unique_ptr<SourceRegionReader>(new SourceRegionReader());
    Reader->File = *FileOrErr;
    Reader->IsOpen = true;
    return std::move(Reader);
  }

  ~SourceRegionReader() {
    if (IsOpen)
      consumeError(errorCodeToError(sys::fs::closeFile(File)));
  }

  Expected<bool> read(SourceRegion &Region, size_t NumSourceFilenames,
                      bool IsBaseline) {
    Expected<bool> Read = readPod(Region.FilenameID, true);
    if (!Read || !*Read)
      return Read;
    if (Region.FilenameID >= NumSourceFilenames)
      return createStringError(errc::io_error,
                               "invalid text coverage source filename ID");
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
    if (IsBaseline)
      Region.ExecutionCount = 0;
    else if (Expected<bool> R = readPod(Region.ExecutionCount, false); !R)
      return R.takeError();
    return true;
  }
};

Expected<std::unique_ptr<ToolOutputFile>> createTemporaryOutput() {
  int FD = -1;
  SmallString<128> Path;
  if (std::error_code EC =
          sys::fs::createTemporaryFile("llvm-cov-txtcvrg", "tmp", FD, Path))
    return errorCodeToError(EC);
  return std::make_unique<ToolOutputFile>(Path, FD);
}

void appendKeyPart(std::string &Key, uint32_t Value) {
  Key.append(reinterpret_cast<const char *>(&Value), sizeof(Value));
}

std::string sourceRegionKey(const SourceRegion &Region) {
  std::string Key;
  Key.reserve(5 * sizeof(uint32_t));
  appendKeyPart(Key, Region.Kind);
  appendKeyPart(Key, Region.LineStart);
  appendKeyPart(Key, Region.ColumnStart);
  appendKeyPart(Key, Region.LineEnd);
  appendKeyPart(Key, Region.ColumnEnd);
  return Key;
}

bool isSyntheticExpandedSourceCodeRegion(const SourceRegion &Region,
                                         bool IsExpandedFile) {
  // Expanded source files can contain bookkeeping code regions that start at
  // the synthetic beginning of the expanded file rather than at a token in the
  // physical source. Do not print those as source-owned txtcvrg rows; they
  // produce misleading ranges such as "1.1 -> <macro end>" in headers. Keep
  // this limited to non-root source aggregation, and keep expansion records and
  // real macro body regions intact.
  return IsExpandedFile && Region.Kind == ReportCodeRegionKind &&
         Region.LineStart == 1 && Region.ColumnStart == 1;
}

} // namespace

class CoverageExporterCoveredFunctions::Implementation {
  struct PathRemapping {
    std::string From;
    std::string To;
  };

  struct FunctionFileRegions {
    unsigned FileID;
    std::string Filename;
    bool IsRoot = false;
    std::vector<StructuralRegion> Regions;
    std::vector<StructuralSegment> Segments;
  };

  struct FunctionExpansion {
    unsigned FileID;
    std::string Filename;
    LineColPair Start;
    LineColPair End;
    std::string ExpandedFilename;
    uint64_t ExecutionCount;
    unsigned MappingOrdinal;
  };

  raw_ostream &OS;
  const CoverageFilters &FilenameFilters;
  const CoveredFunctionsExportOptions Options;
  std::vector<PathRemapping> PathRemappings;
  StringMap<std::string> RemappedPathCache;
  StringMap<uint32_t> SourceFilenameIDs;
  std::vector<std::string> SourceFilenames;
  std::array<std::unique_ptr<ToolOutputFile>, NumSourcePartitions>
      SourcePartitions;
  DenseMap<uint32_t, SourceFileRegions> PendingSourceRegions;
  size_t PendingSourceRegionMemory = 0;
  std::string CurrentFunctionFile;
  bool FunctionFileOpen = false;
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
    for (const PathRemapping &Remapping : PathRemappings)
      if (StringRef(Native).starts_with(Remapping.From))
        return Remapping.To +
               StringRef(Native).drop_front(Remapping.From.size()).str();
    return Native;
  }

  StringRef remapPathCached(StringRef Path) {
    auto [I, Inserted] = RemappedPathCache.try_emplace(Path);
    if (Inserted)
      I->second = remapPath(Path);
    return I->second;
  }

  Expected<uint32_t> getSourceFilenameID(StringRef Filename) {
    auto I = SourceFilenameIDs.find(Filename);
    if (I != SourceFilenameIDs.end())
      return I->second;
    if (SourceFilenames.size() == std::numeric_limits<uint32_t>::max())
      return createStringError(errc::file_too_large,
                               "too many text coverage source filenames");
    uint32_t ID = SourceFilenames.size();
    SourceFilenames.push_back(Filename.str());
    SourceFilenameIDs.insert({SourceFilenames.back(), ID});
    return ID;
  }

  static size_t aggregateSourceRegionInFile(SourceFileRegions &File,
                                            SourceRegion Region) {
    uint64_t ExecutionCount = Region.ExecutionCount;
    std::string Key = sourceRegionKey(Region);
    auto [I, Inserted] =
        File.Regions.try_emplace(std::move(Key), std::move(Region));
    if (!Inserted) {
      I->second.ExecutionCount =
          SaturatingAdd(I->second.ExecutionCount, ExecutionCount);
      return 0;
    }
    // Include conservative estimates for the string object, hash-table node,
    // and bucket storage. The limit is intentionally approximate, but it must
    // overestimate normal libstdc++ and libc++ unordered_map entries so a
    // partition is split before its aggregate becomes unexpectedly large.
    return sizeof(SourceRegion) + sizeof(std::string) + I->first.capacity() +
           64;
  }

  static size_t
  aggregateSourceRegion(DenseMap<uint32_t, SourceFileRegions> &Files,
                        SourceRegion Region) {
    auto [File, Inserted] = Files.try_emplace(Region.FilenameID);
    size_t AddedMemory = Inserted ? sizeof(SourceFileRegions) + 128 : 0;
    return AddedMemory +
           aggregateSourceRegionInFile(File->second, std::move(Region));
  }

  Error writeInitialSourceRegion(const SourceRegion &Region) {
    // Partition by filename rather than region coordinates. That produces one
    // source-file block per file unless recursive repartitioning is needed.
    unsigned Partition = Region.FilenameID % NumSourcePartitions;
    std::unique_ptr<ToolOutputFile> &Output = SourcePartitions[Partition];
    if (!Output) {
      Expected<std::unique_ptr<ToolOutputFile>> OutputOrErr =
          createTemporaryOutput();
      if (!OutputOrErr)
        return OutputOrErr.takeError();
      Output = std::move(*OutputOrErr);
    }
    return writeSourceRegion(Output->os(), Region,
                             Options.ExportMode ==
                                 CoveredFunctionsExportOptions::Mode::Baseline);
  }

  Error flushPendingSourceRegions() {
    for (const auto &File : PendingSourceRegions)
      for (const auto &Entry : File.second.Regions)
        if (Error E = writeInitialSourceRegion(Entry.second))
          return E;
    DenseMap<uint32_t, SourceFileRegions> EmptyRegions;
    PendingSourceRegions.swap(EmptyRegions);
    PendingSourceRegionMemory = 0;
    return Error::success();
  }

  Error addSourceRegion(SourceRegion Region) {
    PendingSourceRegionMemory +=
        aggregateSourceRegion(PendingSourceRegions, std::move(Region));
    if (PendingSourceRegionMemory <= SourcePartitionMemoryLimit)
      return Error::success();
    return flushPendingSourceRegions();
  }

  static unsigned getSourceRegionPartition(const SourceRegion &Region,
                                           unsigned Depth) {
    return static_cast<size_t>(hash_combine(
               Depth, Region.FilenameID, Region.Kind, Region.LineStart,
               Region.ColumnStart, Region.LineEnd, Region.ColumnEnd)) %
           NumSourcePartitions;
  }

  Error addPartitionedSourceRegion(std::array<std::unique_ptr<ToolOutputFile>,
                                              NumSourcePartitions> &Partitions,
                                   const SourceRegion &Region, unsigned Depth) {
    unsigned Partition = getSourceRegionPartition(Region, Depth);
    std::unique_ptr<ToolOutputFile> &Output = Partitions[Partition];
    if (!Output) {
      Expected<std::unique_ptr<ToolOutputFile>> OutputOrErr =
          createTemporaryOutput();
      if (!OutputOrErr)
        return OutputOrErr.takeError();
      Output = std::move(*OutputOrErr);
    }
    return writeSourceRegion(Output->os(), Region,
                             Options.ExportMode ==
                                 CoveredFunctionsExportOptions::Mode::Baseline);
  }

  void renderFunction(StringRef RootFilename, StringRef DisplayName,
                      uint64_t TotalCodeRegions, uint64_t HitCodeRegions) {
    if (!FunctionFileOpen || CurrentFunctionFile != RootFilename) {
      CurrentFunctionFile = RootFilename;
      FunctionFileOpen = true;
      OS << "\nfile\t\"";
      printEscapedString(RootFilename, OS);
      OS << "\"\n";
    }
    double Percent =
        TotalCodeRegions ? 100.0 * HitCodeRegions / TotalCodeRegions : 0.0;
    OS << "function\t\"";
    printEscapedString(DisplayName, OS);
    OS << "\"\t" << TotalCodeRegions << '\t' << HitCodeRegions << '\t'
       << format("%.2f", Percent) << '\n';
  }

  void renderSourceFiles(DenseMap<uint32_t, SourceFileRegions> &Files) {
    SmallVector<uint32_t, 8> FileIDs;
    FileIDs.reserve(Files.size());
    for (const auto &File : Files)
      FileIDs.push_back(File.first);
    if (Options.ExportView == CoveredFunctionsExportOptions::View::Segments)
      llvm::sort(FileIDs, [&](uint32_t LHS, uint32_t RHS) {
        return SourceFilenames[LHS] < SourceFilenames[RHS];
      });

    for (uint32_t FileID : FileIDs) {
      assert(FileID < SourceFilenames.size() && "invalid source filename ID");
      OS << "\nfile\t\"";
      printEscapedString(SourceFilenames[FileID], OS);
      OS << "\"\n";
      SmallVector<SourceRegion, 8> Regions;
      if (Options.ExportView == CoveredFunctionsExportOptions::View::Segments) {
        SmallVector<SourceRegion, 8> CodeRegions;
        for (const auto &Entry : Files.find(FileID)->second.Regions) {
          const SourceRegion &Region = Entry.second;
          if (Region.Kind != ReportCodeRegionKind) {
            Regions.push_back(Region);
            continue;
          }
          CodeRegions.push_back(Region);
        }
        llvm::append_range(Regions, buildAggregatedSourceSegments(CodeRegions));
        llvm::sort(Regions,
                   [](const SourceRegion &LHS, const SourceRegion &RHS) {
                     return std::tie(LHS.LineStart, LHS.ColumnStart,
                                     LHS.LineEnd, LHS.ColumnEnd, LHS.Kind) <
                            std::tie(RHS.LineStart, RHS.ColumnStart,
                                     RHS.LineEnd, RHS.ColumnEnd, RHS.Kind);
                   });
      } else {
        for (const auto &Entry : Files.find(FileID)->second.Regions)
          Regions.push_back(Entry.second);
      }
      for (const SourceRegion &Region : Regions) {
        OS << Region.Kind << '\t' << Region.LineStart << '.'
           << Region.ColumnStart << '\t' << Region.LineEnd << '.'
           << Region.ColumnEnd << '\t' << Region.ExecutionCount << '\n';
      }
    }
  }

  Error renderSourcePartition(StringRef Path, unsigned Depth) {
    Expected<std::unique_ptr<SourceRegionReader>> ReaderOrErr =
        SourceRegionReader::create(Path);
    if (!ReaderOrErr)
      return ReaderOrErr.takeError();
    std::unique_ptr<SourceRegionReader> Reader = std::move(*ReaderOrErr);

    DenseMap<uint32_t, SourceFileRegions> Files;
    size_t MemoryUsage = 0;
    std::array<std::unique_ptr<ToolOutputFile>, NumSourcePartitions>
        ChildPartitions;
    bool Repartitioned = false;
    while (true) {
      SourceRegion Region;
      Expected<bool> HasRegion = Reader->read(
          Region, SourceFilenames.size(),
          Options.ExportMode == CoveredFunctionsExportOptions::Mode::Baseline);
      if (!HasRegion)
        return HasRegion.takeError();
      if (!*HasRegion)
        break;

      if (Repartitioned) {
        if (Error E =
                addPartitionedSourceRegion(ChildPartitions, Region, Depth + 1))
          return E;
        continue;
      }

      MemoryUsage += aggregateSourceRegion(Files, std::move(Region));
      if (MemoryUsage <= SourcePartitionMemoryLimit)
        continue;
      if (Depth >= MaxSourcePartitionDepth)
        return createStringError(
            errc::not_enough_memory,
            "text coverage source partition exceeds the memory limit after "
            "recursive repartitioning");

      for (const auto &File : Files)
        for (const auto &Entry : File.second.Regions)
          if (Error E = addPartitionedSourceRegion(ChildPartitions,
                                                   Entry.second, Depth + 1))
            return E;
      DenseMap<uint32_t, SourceFileRegions> EmptyFiles;
      Files.swap(EmptyFiles);
      MemoryUsage = 0;
      Repartitioned = true;
    }

    if (!Repartitioned) {
      renderSourceFiles(Files);
      return Error::success();
    }

    Reader.reset();
    for (const std::unique_ptr<ToolOutputFile> &Output : ChildPartitions) {
      if (!Output)
        continue;
      Output->os().close();
      if (Output->os().has_error())
        return errorCodeToError(Output->os().error());
    }
    for (const std::unique_ptr<ToolOutputFile> &Output : ChildPartitions) {
      if (!Output)
        continue;
      if (Error E = renderSourcePartition(Output->getFilename(), Depth + 1))
        return E;
    }
    return Error::success();
  }

  Error renderSourcePartitions() {
    for (const std::unique_ptr<ToolOutputFile> &Output : SourcePartitions) {
      if (!Output)
        continue;
      if (Error E = renderSourcePartition(Output->getFilename(), 0))
        return E;
    }
    return Error::success();
  }

  Expected<bool> renderSegmentFunction(StringRef RawFunctionName,
                                       FunctionRecord &Function,
                                       unsigned RootFileID,
                                       const SmallBitVector &ExpandedFileIDs) {
    std::vector<FunctionFileRegions> Files;
    DenseMap<unsigned, unsigned> FileIndices;
    auto GetFile = [&](unsigned FileID,
                       StringRef Filename) -> FunctionFileRegions & {
      auto [I, Inserted] = FileIndices.try_emplace(FileID, Files.size());
      if (Inserted)
        Files.push_back({FileID, Filename.str(), FileID == RootFileID, {}, {}});
      return Files[I->second];
    };

    StringRef RootFilename = remapPathCached(Function.Filenames[RootFileID]);
    GetFile(RootFileID, RootFilename);
    std::vector<FunctionExpansion> Expansions;
    for (unsigned Ordinal = 0; Ordinal != Function.CountedRegions.size();
         ++Ordinal) {
      const CountedRegion &Region = Function.CountedRegions[Ordinal];
      StringRef Filename = Function.Filenames[Region.FileID];
      if (FilenameFilters.matchesFilename(Filename))
        continue;
      StringRef RemappedFilename = remapPathCached(Filename);

      if (Region.Kind == CounterMappingRegion::ExpansionRegion) {
        StringRef ExpandedFilename = Function.Filenames[Region.ExpandedFileID];
        if (FilenameFilters.matchesFilename(ExpandedFilename) ||
            !(Region.startLoc() < Region.endLoc()))
          continue;
        Expansions.push_back({Region.FileID, RemappedFilename.str(),
                              Region.startLoc(), Region.endLoc(),
                              remapPathCached(ExpandedFilename).str(),
                              Region.ExecutionCount, Ordinal});
        continue;
      }
      if (Region.Kind != CounterMappingRegion::CodeRegion &&
          Region.Kind != CounterMappingRegion::GapRegion &&
          Region.Kind != CounterMappingRegion::SkippedRegion)
        continue;
      if (ExpandedFileIDs.test(Region.FileID) &&
          Region.Kind == CounterMappingRegion::CodeRegion &&
          Region.LineStart == 1 && Region.ColumnStart == 1)
        continue;
      GetFile(Region.FileID, RemappedFilename)
          .Regions.push_back({Region.Kind, Region.startLoc(), Region.endLoc(),
                              Region.Count, Region.ExecutionCount, Ordinal});
    }

    llvm::sort(Files, [](const FunctionFileRegions &LHS,
                         const FunctionFileRegions &RHS) {
      if (LHS.IsRoot != RHS.IsRoot)
        return LHS.IsRoot;
      return std::tie(LHS.Filename, LHS.FileID) <
             std::tie(RHS.Filename, RHS.FileID);
    });

    FunctionFileRegions *RootFile = nullptr;
    for (FunctionFileRegions &File : Files) {
      File.Segments = buildStructuralSegments(std::move(File.Regions));
      retainOneSegmentPerCounter(File.Segments);
      if (File.IsRoot)
        RootFile = &File;
    }
    assert(RootFile && "missing root coverage file");
    uint64_t TotalSegments = RootFile->Segments.size();
    uint64_t HitSegments =
        llvm::count_if(RootFile->Segments, [](const auto &Segment) {
          return Segment.ExecutionCount != 0;
        });
    if (TotalSegments == 0 ||
        (Options.ExportMode == CoveredFunctionsExportOptions::Mode::Execution &&
         HitSegments == 0))
      return false;

    llvm::sort(Expansions,
               [](const FunctionExpansion &LHS, const FunctionExpansion &RHS) {
                 return std::tie(LHS.Filename, LHS.FileID, LHS.Start, LHS.End,
                                 LHS.ExpandedFilename, LHS.MappingOrdinal) <
                        std::tie(RHS.Filename, RHS.FileID, RHS.Start, RHS.End,
                                 RHS.ExpandedFilename, RHS.MappingOrdinal);
               });
    std::vector<FunctionExpansion> CombinedExpansions;
    CombinedExpansions.reserve(Expansions.size());
    for (FunctionExpansion &Expansion : Expansions) {
      if (!CombinedExpansions.empty()) {
        FunctionExpansion &Last = CombinedExpansions.back();
        if (std::tie(Last.Filename, Last.FileID, Last.Start, Last.End,
                     Last.ExpandedFilename) ==
            std::tie(Expansion.Filename, Expansion.FileID, Expansion.Start,
                     Expansion.End, Expansion.ExpandedFilename)) {
          Last.ExecutionCount =
              SaturatingAdd(Last.ExecutionCount, Expansion.ExecutionCount);
          continue;
        }
      }
      CombinedExpansions.push_back(std::move(Expansion));
    }

    std::string DisplayName =
        Function.Name.empty() ? RawFunctionName.str() : Function.Name;
    if (DisplayName.empty())
      DisplayName = "<global-init@" + RootFilename.str() + ">";
    renderFunction(RootFilename, DisplayName, TotalSegments, HitSegments);
    SmallVector<SourceRegion, 8> RootOutputRegions;
    for (const StructuralSegment &Segment : RootFile->Segments)
      RootOutputRegions.push_back({0, ReportCodeRegionKind, Segment.Start.first,
                                   Segment.Start.second, Segment.End.first,
                                   Segment.End.second, Segment.ExecutionCount});

    for (const FunctionExpansion &Expansion : CombinedExpansions) {
      if (Expansion.FileID != RootFileID)
        continue;
      RootOutputRegions.push_back(
          {0, ReportExpansionRegionKind, Expansion.Start.first,
           Expansion.Start.second, Expansion.End.first, Expansion.End.second,
           Expansion.ExecutionCount});
    }
    llvm::sort(RootOutputRegions,
               [](const SourceRegion &LHS, const SourceRegion &RHS) {
                 return std::tie(LHS.LineStart, LHS.ColumnStart, LHS.LineEnd,
                                 LHS.ColumnEnd, LHS.Kind) <
                        std::tie(RHS.LineStart, RHS.ColumnStart, RHS.LineEnd,
                                 RHS.ColumnEnd, RHS.Kind);
               });
    for (const SourceRegion &Region : RootOutputRegions)
      OS << Region.Kind << '\t' << Region.LineStart << '.' << Region.ColumnStart
         << '\t' << Region.LineEnd << '.' << Region.ColumnEnd << '\t'
         << Region.ExecutionCount << '\n';

    for (const FunctionFileRegions &File : Files) {
      if (File.IsRoot)
        continue;
      Expected<uint32_t> FilenameID = getSourceFilenameID(File.Filename);
      if (!FilenameID)
        return FilenameID.takeError();
      for (const StructuralSegment &Segment : File.Segments)
        if (Error E = addSourceRegion(
                {*FilenameID, ReportCodeRegionKind, Segment.Start.first,
                 Segment.Start.second, Segment.End.first, Segment.End.second,
                 Segment.ExecutionCount}))
          return std::move(E);
    }
    for (const FunctionExpansion &Expansion : CombinedExpansions) {
      if (Expansion.FileID == RootFileID)
        continue;
      Expected<uint32_t> FilenameID = getSourceFilenameID(Expansion.Filename);
      if (!FilenameID)
        return FilenameID.takeError();
      if (Error E = addSourceRegion(
              {*FilenameID, ReportExpansionRegionKind, Expansion.Start.first,
               Expansion.Start.second, Expansion.End.first,
               Expansion.End.second, Expansion.ExecutionCount}))
        return std::move(E);
    }
    return true;
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
    OS << "txtcvrg\t"
       << (Options.ExportView == CoveredFunctionsExportOptions::View::Segments
               ? 4
               : 3)
       << '\t'
       << (Options.ExportMode == CoveredFunctionsExportOptions::Mode::Baseline
               ? "baseline"
               : "execution")
       << (Options.ExportView == CoveredFunctionsExportOptions::View::Segments
               ? "\tview=segments"
               : "")
       << "\tbranches=" << unsigned(Options.IncludeBranches)
       << "\tmcdc=" << unsigned(Options.IncludeMCDC) << '\n';
  }

  Error consume(StringRef RawFunctionName, uint64_t,
                FunctionRecord &&Function) {
    if (Finished)
      return createStringError(errc::invalid_argument,
                               "text coverage exporter is finished");

    SmallBitVector UsedFileIDs(Function.Filenames.size());
    SmallBitVector ExpandedFileIDs(Function.Filenames.size());
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
      ExpandedFileIDs.set(Region.ExpandedFileID);
    }

    std::optional<unsigned> RootFileID;
    for (unsigned FileID = 0; FileID != Function.Filenames.size(); ++FileID)
      if (UsedFileIDs.test(FileID) && !ExpandedFileIDs.test(FileID)) {
        RootFileID = FileID;
        break;
      }
    if (!RootFileID)
      return make_error<CoverageMapError>(
          coveragemap_error::malformed,
          "coverage function has no non-expanded root file");
    if (FilenameFilters.matchesFilename(Function.Filenames[*RootFileID]))
      return Error::success();

    if (Options.ExportView == CoveredFunctionsExportOptions::View::Segments) {
      Expected<bool> Rendered = renderSegmentFunction(
          RawFunctionName, Function, *RootFileID, ExpandedFileIDs);
      if (!Rendered)
        return Rendered.takeError();
      if (!*Rendered)
        return Error::success();
    } else {
      uint64_t TotalCodeRegions = 0;
      uint64_t HitCodeRegions = 0;
      bool HasReportableRootRegion = false;
      for (const CountedRegion &Region : Function.CountedRegions) {
        if (Region.FileID == *RootFileID) {
          HasReportableRootRegion |= bool(getReportRegionKind(Region.Kind));
          if (Region.Kind == CounterMappingRegion::CodeRegion) {
            ++TotalCodeRegions;
            HitCodeRegions += Region.ExecutionCount != 0;
          }
        }
      }
      if (Options.ExportMode ==
              CoveredFunctionsExportOptions::Mode::Execution &&
          HitCodeRegions == 0)
        return Error::success();
      if (!HasReportableRootRegion)
        return Error::success();

      StringRef RootFilename = remapPathCached(Function.Filenames[*RootFileID]);
      std::string DisplayName =
          Function.Name.empty() ? RawFunctionName.str() : Function.Name;
      if (DisplayName.empty())
        DisplayName = "<global-init@" + RootFilename.str() + ">";
      renderFunction(RootFilename, DisplayName, TotalCodeRegions,
                     HitCodeRegions);
      for (const CountedRegion &Region : Function.CountedRegions) {
        std::optional<uint32_t> ReportKind = getReportRegionKind(Region.Kind);
        if (!ReportKind)
          continue;
        if (Region.FileID >= Function.Filenames.size())
          return make_error<CoverageMapError>(coveragemap_error::malformed,
                                              "invalid coverage FileID");
        StringRef Filename = Function.Filenames[Region.FileID];
        if (FilenameFilters.matchesFilename(Filename))
          continue;
        if (Region.FileID == *RootFileID) {
          OS << *ReportKind << '\t' << Region.LineStart << '.'
             << Region.ColumnStart << '\t' << Region.LineEnd << '.'
             << Region.ColumnEnd << '\t' << Region.ExecutionCount << '\n';
          continue;
        }
        StringRef RemappedFilename = remapPathCached(Filename);
        Expected<uint32_t> FilenameID = getSourceFilenameID(RemappedFilename);
        if (!FilenameID)
          return FilenameID.takeError();
        SourceRegion OutputRegion{*FilenameID,          *ReportKind,
                                  Region.LineStart,     Region.ColumnStart,
                                  Region.LineEnd,       Region.ColumnEnd,
                                  Region.ExecutionCount};
        if (isSyntheticExpandedSourceCodeRegion(
                OutputRegion, ExpandedFileIDs.test(Region.FileID)))
          continue;
        if (Error E = addSourceRegion(std::move(OutputRegion)))
          return E;
      }
    }

    for (const CountedRegion &Region : Function.CountedBranchRegions) {
      if (Region.FileID >= Function.Filenames.size())
        return make_error<CoverageMapError>(coveragemap_error::malformed,
                                            "invalid branch coverage FileID");
      StringRef Filename = Function.Filenames[Region.FileID];
      if (FilenameFilters.matchesFilename(Filename))
        continue;
      StringRef RemappedFilename = remapPathCached(Filename);
      if (Options.IncludeBranches) {
        OS << "branch\t\"";
        printEscapedString(RemappedFilename, OS);
        OS << "\"\t" << Region.LineStart << '.' << Region.ColumnStart << '\t'
           << Region.LineEnd << '.' << Region.ColumnEnd << '\t'
           << Region.ExecutionCount << '\t' << Region.FalseExecutionCount
           << '\t' << unsigned(Region.TrueFolded) << '\t'
           << unsigned(Region.FalseFolded) << '\n';
      }
      if (Options.IncludeMCDC &&
          Region.Kind == CounterMappingRegion::MCDCBranchRegion) {
        const mcdc::BranchParameters &Params = Region.getBranchParams();
        OS << "mcdc-branch\t\"";
        printEscapedString(RemappedFilename, OS);
        OS << "\"\t" << Region.LineStart << '.' << Region.ColumnStart << '\t'
           << Region.LineEnd << '.' << Region.ColumnEnd << '\t'
           << Region.ExecutionCount << '\t' << Region.FalseExecutionCount
           << '\t' << Params.ID << '\t' << Params.Conds[1] << '\t'
           << Params.Conds[0] << '\t' << unsigned(Region.TrueFolded) << '\t'
           << unsigned(Region.FalseFolded) << '\n';
      }
    }

    if (Options.IncludeMCDC)
      for (MCDCRecord &Record : Function.MCDCRecords) {
        const CounterMappingRegion &Region = Record.getDecisionRegion();
        if (Region.FileID >= Function.Filenames.size())
          return make_error<CoverageMapError>(
              coveragemap_error::malformed,
              "invalid MC/DC decision coverage FileID");
        StringRef Filename = Function.Filenames[Region.FileID];
        if (FilenameFilters.matchesFilename(Filename))
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
        OS << "mcdc-decision\t\"";
        printEscapedString(remapPathCached(Filename), OS);
        OS << "\"\t" << Region.LineStart << '.' << Region.ColumnStart << '\t'
           << Region.LineEnd << '.' << Region.ColumnEnd << '\t' << TrueDecisions
           << '\t' << FalseDecisions << '\t' << Record.getNumConditions()
           << '\t' << CoveredConditions << '\t' << FoldedConditions << '\n';
      }
    return Error::success();
  }

  Error finish() {
    if (Finished)
      return createStringError(errc::invalid_argument,
                               "text coverage exporter finished twice");
    Finished = true;
    if (FunctionFileOpen)
      FunctionFileOpen = false;
    if (Error E = flushPendingSourceRegions())
      return E;
    for (const std::unique_ptr<ToolOutputFile> &Output : SourcePartitions) {
      if (!Output)
        continue;
      Output->os().close();
      if (Output->os().has_error())
        return errorCodeToError(Output->os().error());
    }
    if (Error E = renderSourcePartitions())
      return E;
    OS << "end\n";
    return Error::success();
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

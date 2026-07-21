//===- CoverageMappingSPITest.cpp - Coverage mapping SPI tests ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ProfileData/Coverage/CoverageMappingSPI.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Testing/Support/Error.h"
#include "gtest/gtest.h"
#include <thread>

using namespace llvm;
using namespace llvm::coverage;

namespace {

TEST(CoverageMappingSPI, RecordRoundTripUsesInputViews) {
  CoverageMappingSPIRecord Record{"/out/a.o", "/src/a.c", "mapping"};
  auto EncodedRecord = createCoverageMappingSPIRecord(Record);
  ASSERT_THAT_EXPECTED(EncodedRecord, Succeeded());

  std::string File = createCoverageMappingSPIHeader();
  File += *EncodedRecord;
  auto Contents = readCoverageMappingSPI(File);
  ASSERT_THAT_EXPECTED(Contents, Succeeded());
  ASSERT_FALSE(Contents->HasIncompleteTrailingRecord);
  EXPECT_EQ(File.size(), Contents->ValidSize);
  ASSERT_EQ(1U, Contents->Records.size());
  EXPECT_EQ(Record.Key, Contents->Records[0].Key);
  EXPECT_EQ(Record.Metadata, Contents->Records[0].Metadata);
  EXPECT_EQ(Record.Payload, Contents->Records[0].Payload);
  EXPECT_GE(Contents->Records[0].Key.data(), File.data());
  EXPECT_LT(Contents->Records[0].Payload.end(), File.data() + File.size());
}

TEST(CoverageMappingSPI, PayloadRoundTripPreservesTargetProperties) {
  for (const auto &[BytesInAddress, Endian] :
       {std::pair<uint8_t, llvm::endianness>{4, llvm::endianness::big},
        std::pair<uint8_t, llvm::endianness>{8, llvm::endianness::little}}) {
    CoverageMappingSPIPayload Payload{"profile names",    "coverage mapping",
                                      "coverage records", 0x1234,
                                      BytesInAddress,     Endian};
    auto EncodedPayload = createCoverageMappingSPIPayload(Payload);
    ASSERT_THAT_EXPECTED(EncodedPayload, Succeeded());

    auto DecodedPayload = readCoverageMappingSPIPayload(*EncodedPayload);
    ASSERT_THAT_EXPECTED(DecodedPayload, Succeeded());
    EXPECT_EQ(Payload.ProfileNames, DecodedPayload->ProfileNames);
    EXPECT_EQ(Payload.CoverageMapping, DecodedPayload->CoverageMapping);
    EXPECT_EQ(Payload.CoverageRecords, DecodedPayload->CoverageRecords);
    EXPECT_EQ(Payload.ProfileNamesAddress, DecodedPayload->ProfileNamesAddress);
    EXPECT_EQ(Payload.BytesInAddress, DecodedPayload->BytesInAddress);
    EXPECT_EQ(Payload.Endian, DecodedPayload->Endian);
    EXPECT_EQ(0U, reinterpret_cast<uintptr_t>(
                      DecodedPayload->CoverageMapping.data()) %
                      8);
    EXPECT_EQ(0U, reinterpret_cast<uintptr_t>(
                      DecodedPayload->CoverageRecords.data()) %
                      8);
  }
}

TEST(CoverageMappingSPI, RejectsUnsupportedTargetProperties) {
  CoverageMappingSPIPayload Payload{
      "names", "mapping", "records", 0, 16, llvm::endianness::little};
  EXPECT_THAT_ERROR(createCoverageMappingSPIPayload(Payload).takeError(),
                    Failed());

  Payload.BytesInAddress = 8;
  auto EncodedPayload = createCoverageMappingSPIPayload(Payload);
  ASSERT_THAT_EXPECTED(EncodedPayload, Succeeded());
  (*EncodedPayload)[49] = 0;
  EXPECT_THAT_ERROR(readCoverageMappingSPIPayload(*EncodedPayload).takeError(),
                    Failed());
}

TEST(CoverageMappingSPI, ReportsIncompleteTrailingRecord) {
  CoverageMappingSPIRecord Record{"/out/a.o", "/src/a.c", "mapping"};
  auto EncodedRecord = createCoverageMappingSPIRecord(Record);
  ASSERT_THAT_EXPECTED(EncodedRecord, Succeeded());

  std::string File = createCoverageMappingSPIHeader();
  File += *EncodedRecord;
  uint64_t ValidSize = File.size();
  File.append(EncodedRecord->data(), EncodedRecord->size() / 2);
  auto Contents = readCoverageMappingSPI(File);
  ASSERT_THAT_EXPECTED(Contents, Succeeded());
  EXPECT_TRUE(Contents->HasIncompleteTrailingRecord);
  EXPECT_EQ(ValidSize, Contents->ValidSize);
  ASSERT_EQ(1U, Contents->Records.size());
  EXPECT_EQ(Record.Key, Contents->Records[0].Key);
}

TEST(CoverageMappingSPI, RejectsCorruptFinalRecord) {
  CoverageMappingSPIRecord Record{"/out/a.o", "/src/a.c", "mapping"};
  auto EncodedRecord = createCoverageMappingSPIRecord(Record);
  ASSERT_THAT_EXPECTED(EncodedRecord, Succeeded());

  std::string File = createCoverageMappingSPIHeader();
  File += *EncodedRecord;
  File.back() ^= 1;
  EXPECT_THAT_ERROR(readCoverageMappingSPI(File).takeError(), Failed());
}

TEST(CoverageMappingSPI, AppenderRepairsIncompleteTail) {
  SmallString<128> Path;
  ASSERT_FALSE(sys::fs::createTemporaryFile("coverage-mapping", "spi", Path));
  FileRemover Cleanup(Path);

  CoverageMappingSPIRecord First{"/out/a.o", "/src/a.c", "first"};
  ASSERT_THAT_ERROR(appendCoverageMappingSPIRecord(Path, First), Succeeded());
  auto PartialRecord = createCoverageMappingSPIRecord(
      CoverageMappingSPIRecord{"/out/b.o", "/src/b.c", "partial"});
  ASSERT_THAT_EXPECTED(PartialRecord, Succeeded());
  std::error_code EC;
  {
    raw_fd_ostream OS(Path, EC, sys::fs::CD_OpenExisting, sys::fs::FA_Write,
                      sys::fs::OF_Append);
    ASSERT_FALSE(EC);
    OS.write(PartialRecord->data(), PartialRecord->size() / 2);
  }

  CoverageMappingSPIRecord Second{"/out/c.o", "/src/c.c", "second"};
  ASSERT_THAT_ERROR(appendCoverageMappingSPIRecord(Path, Second), Succeeded());
  auto Buffer = MemoryBuffer::getFile(Path, /*IsText=*/false,
                                      /*RequiresNullTerminator=*/false);
  ASSERT_TRUE(Buffer);
  auto Contents = readCoverageMappingSPI((*Buffer)->getBuffer());
  ASSERT_THAT_EXPECTED(Contents, Succeeded());
  ASSERT_FALSE(Contents->HasIncompleteTrailingRecord);
  ASSERT_EQ(2U, Contents->Records.size());
  EXPECT_EQ("first", Contents->Records[0].Payload);
  EXPECT_EQ("second", Contents->Records[1].Payload);
}

TEST(CoverageMappingSPI, AppenderRejectsCommittedCorruption) {
  SmallString<128> Path;
  ASSERT_FALSE(sys::fs::createTemporaryFile("coverage-mapping", "spi", Path));
  FileRemover Cleanup(Path);

  CoverageMappingSPIRecord First{"/out/a.o", "/src/a.c", "first"};
  ASSERT_THAT_ERROR(appendCoverageMappingSPIRecord(Path, First), Succeeded());

  std::error_code EC;
  {
    raw_fd_ostream OS(Path, EC, sys::fs::CD_OpenExisting, sys::fs::FA_Write,
                      sys::fs::OF_None);
    ASSERT_FALSE(EC);
    OS.seek(24 + 48);
    OS.write(uint8_t('X'));
  }

  CoverageMappingSPIRecord Second{"/out/b.o", "/src/b.c", "second"};
  EXPECT_THAT_ERROR(appendCoverageMappingSPIRecord(Path, Second), Failed());

  auto Buffer = MemoryBuffer::getFile(Path, /*IsText=*/false,
                                      /*RequiresNullTerminator=*/false);
  ASSERT_TRUE(Buffer);
  EXPECT_THAT_ERROR(readCoverageMappingSPI((*Buffer)->getBuffer()).takeError(),
                    Failed());
}

TEST(CoverageMappingSPI, AppenderRepairsIncompleteHeader) {
  SmallString<128> Path;
  ASSERT_FALSE(sys::fs::createTemporaryFile("coverage-mapping", "spi", Path));
  FileRemover Cleanup(Path);

  std::string Header = createCoverageMappingSPIHeader();
  std::error_code EC;
  {
    raw_fd_ostream OS(Path, EC, sys::fs::CD_OpenExisting, sys::fs::FA_Write,
                      sys::fs::OF_None);
    ASSERT_FALSE(EC);
    OS.write(Header.data(), Header.size() / 2);
  }

  CoverageMappingSPIRecord Record{"/out/a.o", "/src/a.c", "mapping"};
  ASSERT_THAT_ERROR(appendCoverageMappingSPIRecord(Path, Record), Succeeded());
  auto Buffer = MemoryBuffer::getFile(Path, /*IsText=*/false,
                                      /*RequiresNullTerminator=*/false);
  ASSERT_TRUE(Buffer);
  auto Contents = readCoverageMappingSPI((*Buffer)->getBuffer());
  ASSERT_THAT_EXPECTED(Contents, Succeeded());
  EXPECT_EQ(1U, Contents->Records.size());
}

TEST(CoverageMappingSPI, AppenderDoesNotRepairInvalidIncompleteHeader) {
  SmallString<128> Path;
  ASSERT_FALSE(sys::fs::createTemporaryFile("coverage-mapping", "spi", Path));
  FileRemover Cleanup(Path);

  std::error_code EC;
  {
    raw_fd_ostream OS(Path, EC, sys::fs::CD_OpenExisting, sys::fs::FA_Write,
                      sys::fs::OF_None);
    ASSERT_FALSE(EC);
    OS << "not-an-spi";
  }

  CoverageMappingSPIRecord Record{"/out/a.o", "/src/a.c", "mapping"};
  EXPECT_THAT_ERROR(appendCoverageMappingSPIRecord(Path, Record), Failed());

  auto Buffer = MemoryBuffer::getFile(Path, /*IsText=*/false,
                                      /*RequiresNullTerminator=*/false);
  ASSERT_TRUE(Buffer);
  EXPECT_EQ("not-an-spi", (*Buffer)->getBuffer());
}

TEST(CoverageMappingSPI, ConcurrentAppendsAreComplete) {
  SmallString<128> Path;
  ASSERT_FALSE(sys::fs::createTemporaryFile("coverage-mapping", "spi", Path));
  FileRemover Cleanup(Path);

  constexpr unsigned NumWriters = 8;
  std::string Errors[NumWriters];
  std::thread Writers[NumWriters];
  for (unsigned I = 0; I != NumWriters; ++I) {
    Writers[I] = std::thread([&, I] {
      std::string Key = (Twine("/out/") + Twine(I) + ".o").str();
      std::string Metadata = (Twine("/src/") + Twine(I) + ".c").str();
      std::string Payload = (Twine("mapping-") + Twine(I)).str();
      if (Error E = appendCoverageMappingSPIRecord(
              Path, CoverageMappingSPIRecord{Key, Metadata, Payload}))
        Errors[I] = toString(std::move(E));
    });
  }
  for (std::thread &Writer : Writers)
    Writer.join();
  for (const std::string &Error : Errors)
    ASSERT_TRUE(Error.empty()) << Error;

  auto Buffer = MemoryBuffer::getFile(Path, /*IsText=*/false,
                                      /*RequiresNullTerminator=*/false);
  ASSERT_TRUE(Buffer);
  auto Contents = readCoverageMappingSPI((*Buffer)->getBuffer());
  ASSERT_THAT_EXPECTED(Contents, Succeeded());
  ASSERT_FALSE(Contents->HasIncompleteTrailingRecord);
  EXPECT_EQ(NumWriters, Contents->Records.size());
}

} // end anonymous namespace

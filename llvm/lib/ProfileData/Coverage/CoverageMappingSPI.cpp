//===- CoverageMappingSPI.cpp - Coverage mapping SPI container ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ProfileData/Coverage/CoverageMappingSPI.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>

using namespace llvm;
using namespace llvm::coverage;

namespace {

constexpr uint32_t SPIRecordMagic = 0x52495053; // "SPIR" in little-endian.
constexpr uint32_t SPIRecordTrailerMagic =
    0x54495053; // "SPIT" in little-endian.
constexpr uint64_t SPIPayloadMagic = 0x6d6970736d766c6c; // "llvmspim".
constexpr uint16_t SPIFileHeaderSize = 24;
constexpr uint16_t SPIRecordHeaderSize = 48;
constexpr uint16_t SPIRecordTrailerSize = 16;
constexpr uint16_t SPIPayloadHeaderSize = 56;
constexpr uint64_t SPIAlignment = 8;

enum class SPIEndianness : uint8_t { Little = 1, Big = 2 };

uint64_t checksum(StringRef SerializedRecord) {
  assert(SerializedRecord.size() >= SPIRecordHeaderSize);
  std::array<char, SPIRecordHeaderSize> Header;
  std::memcpy(Header.data(), SerializedRecord.data(), Header.size());
  std::memset(Header.data() + 36, 0, sizeof(uint64_t));
  uint64_t HeaderHash = xxh3_64bits(StringRef(Header.data(), Header.size()));
  uint64_t BodyHash =
      xxh3_64bits(SerializedRecord.drop_front(SPIRecordHeaderSize));
  return HeaderHash ^ (BodyHash + 0x9e3779b97f4a7c15ULL + (HeaderHash << 6) +
                       (HeaderHash >> 2));
}

Error malformed(const Twine &Message) {
  return createStringError(inconvertibleErrorCode(),
                           Twine("malformed coverage mapping SPI: ") + Message);
}

Expected<uint64_t> checkedAdd(uint64_t LHS, uint64_t RHS,
                              StringRef Description) {
  if (LHS > std::numeric_limits<uint64_t>::max() - RHS)
    return malformed(Twine(Description) + " is too large");
  return LHS + RHS;
}

Expected<uint64_t> checkedAlign(uint64_t Value, StringRef Description) {
  auto WithPadding = checkedAdd(Value, SPIAlignment - 1, Description);
  if (!WithPadding)
    return WithPadding.takeError();
  return alignDown(*WithPadding, SPIAlignment);
}

Error validateHeader(StringRef Data) {
  if (Data.size() < SPIFileHeaderSize)
    return malformed("file header is truncated");

  if (support::endian::read64le(Data.data()) != CoverageMappingSPIMagic)
    return malformed("invalid file magic");
  if (support::endian::read16le(Data.data() + 8) != CoverageMappingSPIVersion)
    return malformed("unsupported file version");
  if (support::endian::read16le(Data.data() + 10) != SPIFileHeaderSize ||
      support::endian::read32le(Data.data() + 12) != 0 ||
      support::endian::read64le(Data.data() + 16) != 0)
    return malformed("unsupported file header");
  return Error::success();
}

Error checkZeroPadding(StringRef Data, uint64_t Begin, uint64_t End,
                       StringRef Description) {
  if (Data.slice(Begin, End).find_first_not_of('\0') != StringRef::npos)
    return malformed(Twine("non-zero ") + Description + " padding");
  return Error::success();
}

Expected<uint64_t> getRecordPayloadOffset(uint64_t KeySize,
                                          uint64_t MetadataSize) {
  auto EndOfKey = checkedAdd(SPIRecordHeaderSize, KeySize, "record");
  if (!EndOfKey)
    return EndOfKey.takeError();
  auto EndOfMetadata = checkedAdd(*EndOfKey, MetadataSize, "record");
  if (!EndOfMetadata)
    return EndOfMetadata.takeError();
  return checkedAlign(*EndOfMetadata, "record");
}

Expected<uint64_t> getRecordTrailerOffset(uint64_t KeySize,
                                          uint64_t MetadataSize,
                                          uint64_t PayloadSize) {
  auto PayloadOffset = getRecordPayloadOffset(KeySize, MetadataSize);
  if (!PayloadOffset)
    return PayloadOffset.takeError();
  auto EndOfPayload = checkedAdd(*PayloadOffset, PayloadSize, "record");
  if (!EndOfPayload)
    return EndOfPayload.takeError();
  return checkedAlign(*EndOfPayload, "record");
}

Expected<uint64_t> getRecordSize(uint64_t KeySize, uint64_t MetadataSize,
                                 uint64_t PayloadSize) {
  auto TrailerOffset =
      getRecordTrailerOffset(KeySize, MetadataSize, PayloadSize);
  if (!TrailerOffset)
    return TrailerOffset.takeError();
  return checkedAdd(*TrailerOffset, SPIRecordTrailerSize, "record");
}

struct ParsedSPIRecord {
  CoverageMappingSPIRecord Record;
  uint64_t Size;
};

Expected<std::optional<ParsedSPIRecord>> parseRecord(StringRef Data) {
  if (Data.size() < SPIRecordHeaderSize)
    return std::optional<ParsedSPIRecord>();

  uint32_t Magic = support::endian::read32le(Data.data());
  uint16_t Version = support::endian::read16le(Data.data() + 4);
  uint16_t HeaderSize = support::endian::read16le(Data.data() + 6);
  uint32_t Flags = support::endian::read32le(Data.data() + 8);
  uint64_t RecordSize = support::endian::read64le(Data.data() + 12);
  uint32_t KeySize = support::endian::read32le(Data.data() + 20);
  uint32_t MetadataSize = support::endian::read32le(Data.data() + 24);
  uint64_t PayloadSize = support::endian::read64le(Data.data() + 28);
  uint64_t Checksum = support::endian::read64le(Data.data() + 36);
  uint32_t Reserved = support::endian::read32le(Data.data() + 44);

  if (Magic != SPIRecordMagic || Version != CoverageMappingSPIVersion ||
      HeaderSize != SPIRecordHeaderSize || Flags != 0 || Reserved != 0)
    return malformed("unsupported record header");
  if (KeySize == 0)
    return malformed("record key is empty");

  auto PayloadOffset = getRecordPayloadOffset(KeySize, MetadataSize);
  if (!PayloadOffset)
    return PayloadOffset.takeError();
  auto TrailerOffset =
      getRecordTrailerOffset(KeySize, MetadataSize, PayloadSize);
  if (!TrailerOffset)
    return TrailerOffset.takeError();
  auto ExpectedRecordSize = getRecordSize(KeySize, MetadataSize, PayloadSize);
  if (!ExpectedRecordSize)
    return ExpectedRecordSize.takeError();
  if (RecordSize != *ExpectedRecordSize)
    return malformed("record payload sizes do not match record size");
  if (RecordSize > Data.size())
    return std::optional<ParsedSPIRecord>();

  StringRef SerializedRecord = Data.take_front(RecordSize);
  if (checksum(SerializedRecord) != Checksum)
    return malformed("record checksum does not match");
  StringRef Trailer =
      SerializedRecord.slice(*TrailerOffset, *ExpectedRecordSize);
  if (support::endian::read32le(Trailer.data()) != SPIRecordTrailerMagic ||
      support::endian::read16le(Trailer.data() + 4) !=
          CoverageMappingSPIVersion ||
      support::endian::read16le(Trailer.data() + 6) != SPIRecordTrailerSize ||
      support::endian::read64le(Trailer.data() + 8) != RecordSize)
    return malformed("invalid record trailer");

  uint64_t KeyOffset = SPIRecordHeaderSize;
  uint64_t MetadataOffset = KeyOffset + KeySize;
  uint64_t EndOfMetadata = MetadataOffset + MetadataSize;
  uint64_t EndOfPayload = *PayloadOffset + PayloadSize;
  if (Error E = checkZeroPadding(SerializedRecord, EndOfMetadata,
                                 *PayloadOffset, "record"))
    return std::move(E);
  if (Error E = checkZeroPadding(SerializedRecord, EndOfPayload, *TrailerOffset,
                                 "record"))
    return std::move(E);

  return std::optional<ParsedSPIRecord>(
      ParsedSPIRecord{CoverageMappingSPIRecord{
                          SerializedRecord.slice(KeyOffset, MetadataOffset),
                          SerializedRecord.slice(MetadataOffset, EndOfMetadata),
                          SerializedRecord.slice(*PayloadOffset, EndOfPayload)},
                      RecordSize});
}

struct SPIPayloadLayout {
  uint64_t CoverageMappingOffset;
  uint64_t CoverageRecordsOffset;
  uint64_t Size;
};

Expected<SPIPayloadLayout> getPayloadLayout(uint64_t ProfileNamesSize,
                                            uint64_t CoverageMappingSize,
                                            uint64_t CoverageRecordsSize) {
  auto EndOfProfileNames =
      checkedAdd(SPIPayloadHeaderSize, ProfileNamesSize, "payload");
  if (!EndOfProfileNames)
    return EndOfProfileNames.takeError();
  auto CoverageMappingOffset = checkedAlign(*EndOfProfileNames, "payload");
  if (!CoverageMappingOffset)
    return CoverageMappingOffset.takeError();
  auto EndOfCoverageMapping =
      checkedAdd(*CoverageMappingOffset, CoverageMappingSize, "payload");
  if (!EndOfCoverageMapping)
    return EndOfCoverageMapping.takeError();
  auto CoverageRecordsOffset = checkedAlign(*EndOfCoverageMapping, "payload");
  if (!CoverageRecordsOffset)
    return CoverageRecordsOffset.takeError();
  auto EndOfCoverageRecords =
      checkedAdd(*CoverageRecordsOffset, CoverageRecordsSize, "payload");
  if (!EndOfCoverageRecords)
    return EndOfCoverageRecords.takeError();
  auto Size = checkedAlign(*EndOfCoverageRecords, "payload");
  if (!Size)
    return Size.takeError();
  return SPIPayloadLayout{*CoverageMappingOffset, *CoverageRecordsOffset,
                          *Size};
}

Expected<SPIEndianness> encodeEndianness(llvm::endianness Endian) {
  if (Endian == llvm::endianness::little)
    return SPIEndianness::Little;
  if (Endian == llvm::endianness::big)
    return SPIEndianness::Big;
  return malformed("unsupported target endianness");
}

Expected<llvm::endianness> decodeEndianness(uint8_t Endian) {
  if (Endian == static_cast<uint8_t>(SPIEndianness::Little))
    return llvm::endianness::little;
  if (Endian == static_cast<uint8_t>(SPIEndianness::Big))
    return llvm::endianness::big;
  return malformed("unsupported target endianness");
}

std::optional<uint64_t> readRecordTrailer(StringRef Data) {
  if (Data.size() != SPIRecordTrailerSize ||
      support::endian::read32le(Data.data()) != SPIRecordTrailerMagic ||
      support::endian::read16le(Data.data() + 4) != CoverageMappingSPIVersion ||
      support::endian::read16le(Data.data() + 6) != SPIRecordTrailerSize)
    return std::nullopt;
  return support::endian::read64le(Data.data() + 8);
}

} // end anonymous namespace

bool llvm::coverage::isCoverageMappingSPI(StringRef Data) {
  return Data.size() >= sizeof(uint64_t) &&
         support::endian::read64le(Data.data()) == CoverageMappingSPIMagic;
}

std::string llvm::coverage::createCoverageMappingSPIHeader() {
  std::string Data;
  raw_string_ostream OS(Data);
  support::endian::Writer Writer(OS, endianness::little);
  Writer.write<uint64_t>(CoverageMappingSPIMagic);
  Writer.write<uint16_t>(CoverageMappingSPIVersion);
  Writer.write<uint16_t>(SPIFileHeaderSize);
  Writer.write<uint32_t>(0); // Flags reserved for future extensions.
  Writer.write<uint64_t>(0); // Reserved.
  return Data;
}

Expected<std::string> llvm::coverage::createCoverageMappingSPIRecord(
    const CoverageMappingSPIRecord &Record) {
  if (Record.Key.empty())
    return malformed("record key is empty");
  if (Record.Key.size() > std::numeric_limits<uint32_t>::max())
    return malformed("record key is too large");
  if (Record.Metadata.size() > std::numeric_limits<uint32_t>::max())
    return malformed("record metadata is too large");

  auto PayloadOffset =
      getRecordPayloadOffset(Record.Key.size(), Record.Metadata.size());
  if (!PayloadOffset)
    return PayloadOffset.takeError();
  auto TrailerOffset = getRecordTrailerOffset(
      Record.Key.size(), Record.Metadata.size(), Record.Payload.size());
  if (!TrailerOffset)
    return TrailerOffset.takeError();
  auto RecordSize = getRecordSize(Record.Key.size(), Record.Metadata.size(),
                                  Record.Payload.size());
  if (!RecordSize)
    return RecordSize.takeError();
  if (*RecordSize > std::string().max_size())
    return malformed("record is too large for this host");

  std::string Data;
  Data.reserve(static_cast<size_t>(*RecordSize));
  {
    raw_string_ostream OS(Data);
    support::endian::Writer Writer(OS, endianness::little);
    Writer.write<uint32_t>(SPIRecordMagic);
    Writer.write<uint16_t>(CoverageMappingSPIVersion);
    Writer.write<uint16_t>(SPIRecordHeaderSize);
    Writer.write<uint32_t>(0); // Flags reserved for future extensions.
    Writer.write<uint64_t>(*RecordSize);
    Writer.write<uint32_t>(Record.Key.size());
    Writer.write<uint32_t>(Record.Metadata.size());
    Writer.write<uint64_t>(Record.Payload.size());
    Writer.write<uint64_t>(0); // Filled after the record body is serialized.
    Writer.write<uint32_t>(0); // Reserved.
    OS << Record.Key << Record.Metadata;
  }
  Data.resize(static_cast<size_t>(*PayloadOffset), '\0');
  Data.append(Record.Payload.data(), Record.Payload.size());
  Data.resize(static_cast<size_t>(*TrailerOffset), '\0');
  {
    raw_string_ostream OS(Data);
    support::endian::Writer Writer(OS, endianness::little);
    Writer.write<uint32_t>(SPIRecordTrailerMagic);
    Writer.write<uint16_t>(CoverageMappingSPIVersion);
    Writer.write<uint16_t>(SPIRecordTrailerSize);
    Writer.write<uint64_t>(*RecordSize);
  }
  assert(Data.size() == *RecordSize);

  uint64_t Checksum = checksum(Data);
  support::endian::write64le(Data.data() + 36, Checksum);
  return Data;
}

Expected<std::string> llvm::coverage::createCoverageMappingSPIPayload(
    const CoverageMappingSPIPayload &Payload) {
  if (Payload.BytesInAddress != 4 && Payload.BytesInAddress != 8)
    return malformed("unsupported target address width");
  auto EncodedEndian = encodeEndianness(Payload.Endian);
  if (!EncodedEndian)
    return EncodedEndian.takeError();
  auto Layout = getPayloadLayout(Payload.ProfileNames.size(),
                                 Payload.CoverageMapping.size(),
                                 Payload.CoverageRecords.size());
  if (!Layout)
    return Layout.takeError();
  if (Layout->Size > std::string().max_size())
    return malformed("payload is too large for this host");

  std::string Data;
  Data.reserve(static_cast<size_t>(Layout->Size));
  {
    raw_string_ostream OS(Data);
    support::endian::Writer Writer(OS, endianness::little);
    Writer.write<uint64_t>(SPIPayloadMagic);
    Writer.write<uint16_t>(CoverageMappingSPIVersion);
    Writer.write<uint16_t>(SPIPayloadHeaderSize);
    Writer.write<uint32_t>(0); // Flags reserved for future extensions.
    Writer.write<uint64_t>(Payload.ProfileNames.size());
    Writer.write<uint64_t>(Payload.CoverageMapping.size());
    Writer.write<uint64_t>(Payload.CoverageRecords.size());
    Writer.write<uint64_t>(Payload.ProfileNamesAddress);
    Writer.write<uint8_t>(Payload.BytesInAddress);
    Writer.write<uint8_t>(static_cast<uint8_t>(*EncodedEndian));
    Writer.write<uint16_t>(0); // Reserved.
    Writer.write<uint32_t>(0); // Reserved.
    OS << Payload.ProfileNames;
  }
  Data.resize(static_cast<size_t>(Layout->CoverageMappingOffset), '\0');
  Data.append(Payload.CoverageMapping.data(), Payload.CoverageMapping.size());
  Data.resize(static_cast<size_t>(Layout->CoverageRecordsOffset), '\0');
  Data.append(Payload.CoverageRecords.data(), Payload.CoverageRecords.size());
  Data.resize(static_cast<size_t>(Layout->Size), '\0');
  return Data;
}

Expected<CoverageMappingSPIPayload>
llvm::coverage::readCoverageMappingSPIPayload(StringRef Data) {
  if (Data.size() < SPIPayloadHeaderSize)
    return malformed("payload header is truncated");
  if (support::endian::read64le(Data.data()) != SPIPayloadMagic)
    return malformed("invalid payload magic");
  if (support::endian::read16le(Data.data() + 8) != CoverageMappingSPIVersion)
    return malformed("unsupported payload version");
  if (support::endian::read16le(Data.data() + 10) != SPIPayloadHeaderSize ||
      support::endian::read32le(Data.data() + 12) != 0 ||
      support::endian::read16le(Data.data() + 50) != 0 ||
      support::endian::read32le(Data.data() + 52) != 0)
    return malformed("unsupported payload header");

  uint64_t ProfileNamesSize = support::endian::read64le(Data.data() + 16);
  uint64_t CoverageMappingSize = support::endian::read64le(Data.data() + 24);
  uint64_t CoverageRecordsSize = support::endian::read64le(Data.data() + 32);
  uint64_t ProfileNamesAddress = support::endian::read64le(Data.data() + 40);
  uint8_t BytesInAddress = static_cast<uint8_t>(Data[48]);
  if (BytesInAddress != 4 && BytesInAddress != 8)
    return malformed("unsupported target address width");
  auto Endian = decodeEndianness(static_cast<uint8_t>(Data[49]));
  if (!Endian)
    return Endian.takeError();

  auto Layout = getPayloadLayout(ProfileNamesSize, CoverageMappingSize,
                                 CoverageRecordsSize);
  if (!Layout)
    return Layout.takeError();
  if (Layout->Size != Data.size())
    return malformed("payload sizes do not match payload size");

  uint64_t EndOfProfileNames = SPIPayloadHeaderSize + ProfileNamesSize;
  uint64_t EndOfCoverageMapping =
      Layout->CoverageMappingOffset + CoverageMappingSize;
  uint64_t EndOfCoverageRecords =
      Layout->CoverageRecordsOffset + CoverageRecordsSize;
  if (Error E = checkZeroPadding(Data, EndOfProfileNames,
                                 Layout->CoverageMappingOffset, "payload"))
    return std::move(E);
  if (Error E = checkZeroPadding(Data, EndOfCoverageMapping,
                                 Layout->CoverageRecordsOffset, "payload"))
    return std::move(E);
  if (Error E =
          checkZeroPadding(Data, EndOfCoverageRecords, Layout->Size, "payload"))
    return std::move(E);

  return CoverageMappingSPIPayload{
      Data.slice(SPIPayloadHeaderSize, EndOfProfileNames),
      Data.slice(Layout->CoverageMappingOffset, EndOfCoverageMapping),
      Data.slice(Layout->CoverageRecordsOffset, EndOfCoverageRecords),
      ProfileNamesAddress,
      BytesInAddress,
      *Endian};
}

Error llvm::coverage::appendCoverageMappingSPIRecord(
    StringRef Filename, const CoverageMappingSPIRecord &Record) {
  auto RecordData = createCoverageMappingSPIRecord(Record);
  if (!RecordData)
    return RecordData.takeError();

  // POSIX record locks coordinate processes, but threads in one process share
  // them. Serialize the complete repair-and-append transaction locally too.
  static std::mutex LocalAppendMutex;
  std::lock_guard<std::mutex> LocalLock(LocalAppendMutex);

  std::error_code EC;
  int FD = -1;
  EC = sys::fs::openFileForReadWrite(Filename, FD, sys::fs::CD_OpenAlways,
                                     sys::fs::OF_Append);
  if (EC)
    return createStringError(EC,
                             "could not open coverage mapping SPI file "
                             "'%s'",
                             Filename.str().c_str());
  raw_fd_ostream OS(FD, /*ShouldClose=*/true);

  auto Locked = OS.lock();
  if (!Locked)
    return createStringError("could not lock coverage mapping SPI file '" +
                             Filename + "': " + toString(Locked.takeError()));

  uint64_t FileSize = 0;
  sys::fs::file_status Status;
  if ((EC = sys::fs::status(FD, Status)))
    return createStringError(EC,
                             "could not stat coverage mapping SPI file "
                             "'%s'",
                             Filename.str().c_str());
  FileSize = Status.getSize();

  uint64_t ValidSize = FileSize;
  if (FileSize != 0) {
    uint64_t HeaderSize = std::min<uint64_t>(FileSize, SPIFileHeaderSize);
    auto HeaderOrErr =
        MemoryBuffer::getOpenFileSlice(FD, Filename, HeaderSize, /*Offset=*/0,
                                       /*IsVolatile=*/true);
    if (!HeaderOrErr)
      return createStringError(HeaderOrErr.getError(),
                               "could not read coverage mapping SPI file '%s'",
                               Filename.str().c_str());
    StringRef Header = (*HeaderOrErr)->getBuffer();
    if (Header.size() < SPIFileHeaderSize) {
      std::string ExpectedHeader = createCoverageMappingSPIHeader();
      if (!StringRef(ExpectedHeader).starts_with((*HeaderOrErr)->getBuffer()))
        return malformed("invalid incomplete file header");
      ValidSize = 0;
    } else {
      if (Error E = validateHeader(Header))
        return E;

      bool ValidFinalRecord = FileSize == SPIFileHeaderSize;
      if (!ValidFinalRecord &&
          FileSize >= SPIFileHeaderSize + SPIRecordTrailerSize) {
        auto TrailerOrErr = MemoryBuffer::getOpenFileSlice(
            FD, Filename, SPIRecordTrailerSize, FileSize - SPIRecordTrailerSize,
            /*IsVolatile=*/true);
        if (!TrailerOrErr)
          return createStringError(
              TrailerOrErr.getError(),
              "could not read coverage mapping SPI file '%s'",
              Filename.str().c_str());
        if (auto RecordSize = readRecordTrailer((*TrailerOrErr)->getBuffer())) {
          if (*RecordSize >= SPIRecordHeaderSize + SPIRecordTrailerSize &&
              *RecordSize <= FileSize - SPIFileHeaderSize) {
            auto RecordOrErr = MemoryBuffer::getOpenFileSlice(
                FD, Filename, *RecordSize, FileSize - *RecordSize,
                /*IsVolatile=*/true);
            if (!RecordOrErr)
              return createStringError(
                  RecordOrErr.getError(),
                  "could not read coverage mapping SPI file '%s'",
                  Filename.str().c_str());
            auto Parsed = parseRecord((*RecordOrErr)->getBuffer());
            if (!Parsed)
              return Parsed.takeError();
            if (!*Parsed || (*Parsed)->Size != *RecordSize)
              return malformed("invalid final record");
            ValidFinalRecord = true;
          }
        }
      }

      // The normal path validates only the final record, making a sequence of
      // N appends linear in the amount of data written. A missing final trailer
      // indicates an interrupted append; only that recovery path scans the
      // complete file to distinguish truncation from corruption.
      if (!ValidFinalRecord) {
        auto BufferOrErr = MemoryBuffer::getOpenFile(
            FD, Filename, FileSize,
            /*RequiresNullTerminator=*/false, /*IsVolatile=*/true);
        if (!BufferOrErr)
          return createStringError(
              BufferOrErr.getError(),
              "could not read coverage mapping SPI file '%s'",
              Filename.str().c_str());
        auto Contents = readCoverageMappingSPI((*BufferOrErr)->getBuffer());
        if (!Contents)
          return Contents.takeError();
        ValidSize = Contents->ValidSize;
      }
    }
  }

  if (ValidSize != FileSize) {
    if ((EC = sys::fs::resize_file(FD, ValidSize)))
      return createStringError(EC,
                               "could not repair coverage mapping SPI file "
                               "'%s'",
                               Filename.str().c_str());
    OS.seek(ValidSize);
    if (OS.has_error())
      return createStringError(OS.error(),
                               "could not seek coverage mapping SPI file '%s'",
                               Filename.str().c_str());
  }

  if (ValidSize == 0)
    OS << createCoverageMappingSPIHeader();
  OS << *RecordData;
  OS.flush();
  if (OS.has_error())
    return createStringError(OS.error(),
                             "could not write coverage mapping SPI file '%s'",
                             Filename.str().c_str());
  return Error::success();
}

Expected<CoverageMappingSPIContents>
llvm::coverage::readCoverageMappingSPI(StringRef Data) {
  if (Error E = validateHeader(Data))
    return std::move(E);

  CoverageMappingSPIContents Contents;
  uint64_t Offset = SPIFileHeaderSize;
  Contents.ValidSize = Offset;
  while (Offset != Data.size()) {
    StringRef Remaining = Data.drop_front(Offset);
    auto Parsed = parseRecord(Remaining);
    if (!Parsed)
      return Parsed.takeError();
    if (!*Parsed) {
      Contents.HasIncompleteTrailingRecord = true;
      break;
    }
    Contents.Records.push_back((*Parsed)->Record);
    Offset += (*Parsed)->Size;
    Contents.ValidSize = Offset;
  }
  return Contents;
}

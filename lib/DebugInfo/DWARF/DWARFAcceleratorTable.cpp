//===- DWARFAcceleratorTable.cpp ------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/DebugInfo/DWARF/DWARFAcceleratorTable.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/DebugInfo/DWARF/DWARFRelocMap.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/DJB.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/ScopedPrinter.h"
#include "llvm/Support/raw_ostream.h"
#include <cstddef>
#include <cstdint>
#include <utility>

using namespace llvm;

namespace {
struct DwarfConstant {
  StringRef (*StringFn)(unsigned);
  StringRef Type;
  unsigned Value;
};

static raw_ostream &operator<<(raw_ostream &OS, const DwarfConstant &C) {
  StringRef Str = C.StringFn(C.Value);
  if (!Str.empty())
    return OS << Str;
  return OS << "DW_" << C.Type << "_Unknown_0x" << format("%x", C.Value);
}
} // namespace

static DwarfConstant formatTag(unsigned Tag) {
  return {dwarf::TagString, "TAG", Tag};
}

static DwarfConstant formatForm(unsigned Form) {
  return {dwarf::FormEncodingString, "FORM", Form};
}

static DwarfConstant formatIndex(unsigned Idx) {
  return {dwarf::IndexString, "IDX", Idx};
}

static DwarfConstant formatAtom(unsigned Atom) {
  return {dwarf::AtomTypeString, "ATOM", Atom};
}

DWARFAcceleratorTable::~DWARFAcceleratorTable() = default;

llvm::Error AppleAcceleratorTable::extract() {
  uint32_t Offset = 0;

  // Check that we can at least read the header.
  if (!AccelSection.isValidOffset(offsetof(Header, HeaderDataLength)+4))
    return make_error<StringError>("Section too small: cannot read header.",
                                   inconvertibleErrorCode());

  Hdr.Magic = AccelSection.getU32(&Offset);
  Hdr.Version = AccelSection.getU16(&Offset);
  Hdr.HashFunction = AccelSection.getU16(&Offset);
  Hdr.BucketCount = AccelSection.getU32(&Offset);
  Hdr.HashCount = AccelSection.getU32(&Offset);
  Hdr.HeaderDataLength = AccelSection.getU32(&Offset);

  // Check that we can read all the hashes and offsets from the
  // section (see SourceLevelDebugging.rst for the structure of the index).
  // We need to substract one because we're checking for an *offset* which is
  // equal to the size for an empty table and hence pointer after the section.
  if (!AccelSection.isValidOffset(sizeof(Hdr) + Hdr.HeaderDataLength +
                                  Hdr.BucketCount * 4 + Hdr.HashCount * 8 - 1))
    return make_error<StringError>(
        "Section too small: cannot read buckets and hashes.",
        inconvertibleErrorCode());

  HdrData.DIEOffsetBase = AccelSection.getU32(&Offset);
  uint32_t NumAtoms = AccelSection.getU32(&Offset);

  for (unsigned i = 0; i < NumAtoms; ++i) {
    uint16_t AtomType = AccelSection.getU16(&Offset);
    auto AtomForm = static_cast<dwarf::Form>(AccelSection.getU16(&Offset));
    HdrData.Atoms.push_back(std::make_pair(AtomType, AtomForm));
  }

  IsValid = true;
  return Error::success();
}

uint32_t AppleAcceleratorTable::getNumBuckets() { return Hdr.BucketCount; }
uint32_t AppleAcceleratorTable::getNumHashes() { return Hdr.HashCount; }
uint32_t AppleAcceleratorTable::getSizeHdr() { return sizeof(Hdr); }
uint32_t AppleAcceleratorTable::getHeaderDataLength() {
  return Hdr.HeaderDataLength;
}

ArrayRef<std::pair<AppleAcceleratorTable::HeaderData::AtomType,
                   AppleAcceleratorTable::HeaderData::Form>>
AppleAcceleratorTable::getAtomsDesc() {
  return HdrData.Atoms;
}

bool AppleAcceleratorTable::validateForms() {
  for (auto Atom : getAtomsDesc()) {
    DWARFFormValue FormValue(Atom.second);
    switch (Atom.first) {
    case dwarf::DW_ATOM_die_offset:
    case dwarf::DW_ATOM_die_tag:
    case dwarf::DW_ATOM_type_flags:
      if ((!FormValue.isFormClass(DWARFFormValue::FC_Constant) &&
           !FormValue.isFormClass(DWARFFormValue::FC_Flag)) ||
          FormValue.getForm() == dwarf::DW_FORM_sdata)
        return false;
      break;
    default:
      break;
    }
  }
  return true;
}

std::pair<uint32_t, dwarf::Tag>
AppleAcceleratorTable::readAtoms(uint32_t &HashDataOffset) {
  uint32_t DieOffset = dwarf::DW_INVALID_OFFSET;
  dwarf::Tag DieTag = dwarf::DW_TAG_null;
  DWARFFormParams FormParams = {Hdr.Version, 0, dwarf::DwarfFormat::DWARF32};

  for (auto Atom : getAtomsDesc()) {
    DWARFFormValue FormValue(Atom.second);
    FormValue.extractValue(AccelSection, &HashDataOffset, FormParams);
    switch (Atom.first) {
    case dwarf::DW_ATOM_die_offset:
      DieOffset = *FormValue.getAsUnsignedConstant();
      break;
    case dwarf::DW_ATOM_die_tag:
      DieTag = (dwarf::Tag)*FormValue.getAsUnsignedConstant();
      break;
    default:
      break;
    }
  }
  return {DieOffset, DieTag};
}

void AppleAcceleratorTable::Header::dump(ScopedPrinter &W) const {
  DictScope HeaderScope(W, "Header");
  W.printHex("Magic", Magic);
  W.printHex("Version", Version);
  W.printHex("Hash function", HashFunction);
  W.printNumber("Bucket count", BucketCount);
  W.printNumber("Hashes count", HashCount);
  W.printNumber("HeaderData length", HeaderDataLength);
}

bool AppleAcceleratorTable::dumpName(ScopedPrinter &W,
                                     SmallVectorImpl<DWARFFormValue> &AtomForms,
                                     uint32_t *DataOffset) const {
  DWARFFormParams FormParams = {Hdr.Version, 0, dwarf::DwarfFormat::DWARF32};
  uint32_t NameOffset = *DataOffset;
  if (!AccelSection.isValidOffsetForDataOfSize(*DataOffset, 4)) {
    W.printString("Incorrectly terminated list.");
    return false;
  }
  unsigned StringOffset = AccelSection.getRelocatedValue(4, DataOffset);
  if (!StringOffset)
    return false; // End of list

  DictScope NameScope(W, ("Name@0x" + Twine::utohexstr(NameOffset)).str());
  W.startLine() << format("String: 0x%08x", StringOffset);
  W.getOStream() << " \"" << StringSection.getCStr(&StringOffset) << "\"\n";

  unsigned NumData = AccelSection.getU32(DataOffset);
  for (unsigned Data = 0; Data < NumData; ++Data) {
    ListScope DataScope(W, ("Data " + Twine(Data)).str());
    unsigned i = 0;
    for (auto &Atom : AtomForms) {
      W.startLine() << format("Atom[%d]: ", i++);
      if (Atom.extractValue(AccelSection, DataOffset, FormParams))
        Atom.dump(W.getOStream());
      else
        W.getOStream() << "Error extracting the value";
      W.getOStream() << "\n";
    }
  }
  return true; // more entries follow
}

LLVM_DUMP_METHOD void AppleAcceleratorTable::dump(raw_ostream &OS) const {
  if (!IsValid)
    return;

  ScopedPrinter W(OS);

  Hdr.dump(W);

  W.printNumber("DIE offset base", HdrData.DIEOffsetBase);
  W.printNumber("Number of atoms", uint64_t(HdrData.Atoms.size()));
  SmallVector<DWARFFormValue, 3> AtomForms;
  {
    ListScope AtomsScope(W, "Atoms");
    unsigned i = 0;
    for (const auto &Atom : HdrData.Atoms) {
      DictScope AtomScope(W, ("Atom " + Twine(i++)).str());
      W.startLine() << "Type: " << formatAtom(Atom.first) << '\n';
      W.startLine() << "Form: " << formatForm(Atom.second) << '\n';
      AtomForms.push_back(DWARFFormValue(Atom.second));
    }
  }

  // Now go through the actual tables and dump them.
  uint32_t Offset = sizeof(Hdr) + Hdr.HeaderDataLength;
  unsigned HashesBase = Offset + Hdr.BucketCount * 4;
  unsigned OffsetsBase = HashesBase + Hdr.HashCount * 4;

  for (unsigned Bucket = 0; Bucket < Hdr.BucketCount; ++Bucket) {
    unsigned Index = AccelSection.getU32(&Offset);

    ListScope BucketScope(W, ("Bucket " + Twine(Bucket)).str());
    if (Index == UINT32_MAX) {
      W.printString("EMPTY");
      continue;
    }

    for (unsigned HashIdx = Index; HashIdx < Hdr.HashCount; ++HashIdx) {
      unsigned HashOffset = HashesBase + HashIdx*4;
      unsigned OffsetsOffset = OffsetsBase + HashIdx*4;
      uint32_t Hash = AccelSection.getU32(&HashOffset);

      if (Hash % Hdr.BucketCount != Bucket)
        break;

      unsigned DataOffset = AccelSection.getU32(&OffsetsOffset);
      ListScope HashScope(W, ("Hash 0x" + Twine::utohexstr(Hash)).str());
      if (!AccelSection.isValidOffset(DataOffset)) {
        W.printString("Invalid section offset");
        continue;
      }
      while (dumpName(W, AtomForms, &DataOffset))
        /*empty*/;
    }
  }
}

AppleAcceleratorTable::ValueIterator::ValueIterator(
    const AppleAcceleratorTable &AccelTable, unsigned Offset)
    : AccelTable(&AccelTable), DataOffset(Offset) {
  if (!AccelTable.AccelSection.isValidOffsetForDataOfSize(DataOffset, 4))
    return;

  for (const auto &Atom : AccelTable.HdrData.Atoms)
    AtomForms.push_back(DWARFFormValue(Atom.second));

  // Read the first entry.
  NumData = AccelTable.AccelSection.getU32(&DataOffset);
  Next();
}

void AppleAcceleratorTable::ValueIterator::Next() {
  assert(NumData > 0 && "attempted to increment iterator past the end");
  auto &AccelSection = AccelTable->AccelSection;
  if (Data >= NumData ||
      !AccelSection.isValidOffsetForDataOfSize(DataOffset, 4)) {
    NumData = 0;
    return;
  }
  DWARFFormParams FormParams = {AccelTable->Hdr.Version, 0,
                                dwarf::DwarfFormat::DWARF32};
  for (auto &Atom : AtomForms)
    Atom.extractValue(AccelSection, &DataOffset, FormParams);
  ++Data;
}

iterator_range<AppleAcceleratorTable::ValueIterator>
AppleAcceleratorTable::equal_range(StringRef Key) const {
  if (!IsValid)
    return make_range(ValueIterator(), ValueIterator());

  // Find the bucket.
  unsigned HashValue = djbHash(Key);
  unsigned Bucket = HashValue % Hdr.BucketCount;
  unsigned BucketBase = sizeof(Hdr) + Hdr.HeaderDataLength;
  unsigned HashesBase = BucketBase + Hdr.BucketCount * 4;
  unsigned OffsetsBase = HashesBase + Hdr.HashCount * 4;

  unsigned BucketOffset = BucketBase + Bucket * 4;
  unsigned Index = AccelSection.getU32(&BucketOffset);

  // Search through all hashes in the bucket.
  for (unsigned HashIdx = Index; HashIdx < Hdr.HashCount; ++HashIdx) {
    unsigned HashOffset = HashesBase + HashIdx * 4;
    unsigned OffsetsOffset = OffsetsBase + HashIdx * 4;
    uint32_t Hash = AccelSection.getU32(&HashOffset);

    if (Hash % Hdr.BucketCount != Bucket)
      // We are already in the next bucket.
      break;

    unsigned DataOffset = AccelSection.getU32(&OffsetsOffset);
    unsigned StringOffset = AccelSection.getRelocatedValue(4, &DataOffset);
    if (!StringOffset)
      break;

    // Finally, compare the key.
    if (Key == StringSection.getCStr(&StringOffset))
      return make_range({*this, DataOffset}, ValueIterator());
  }
  return make_range(ValueIterator(), ValueIterator());
}

void DWARFDebugNames::Header::dump(ScopedPrinter &W) const {
  DictScope HeaderScope(W, "Header");
  W.printHex("Length", UnitLength);
  W.printNumber("Version", Version);
  W.printHex("Padding", Padding);
  W.printNumber("CU count", CompUnitCount);
  W.printNumber("Local TU count", LocalTypeUnitCount);
  W.printNumber("Foreign TU count", ForeignTypeUnitCount);
  W.printNumber("Bucket count", BucketCount);
  W.printNumber("Name count", NameCount);
  W.printHex("Abbreviations table size", AbbrevTableSize);
  W.startLine() << "Augmentation: '" << AugmentationString << "'\n";
}

llvm::Error DWARFDebugNames::Header::extract(const DWARFDataExtractor &AS,
                                             uint32_t *Offset) {
  // Check that we can read the fixed-size part.
  if (!AS.isValidOffset(*Offset + sizeof(HeaderPOD) - 1))
    return make_error<StringError>("Section too small: cannot read header.",
                                   inconvertibleErrorCode());

  UnitLength = AS.getU32(Offset);
  Version = AS.getU16(Offset);
  Padding = AS.getU16(Offset);
  CompUnitCount = AS.getU32(Offset);
  LocalTypeUnitCount = AS.getU32(Offset);
  ForeignTypeUnitCount = AS.getU32(Offset);
  BucketCount = AS.getU32(Offset);
  NameCount = AS.getU32(Offset);
  AbbrevTableSize = AS.getU32(Offset);
  AugmentationStringSize = AS.getU32(Offset);

  if (!AS.isValidOffsetForDataOfSize(*Offset, AugmentationStringSize))
    return make_error<StringError>(
        "Section too small: cannot read header augmentation.",
        inconvertibleErrorCode());
  AugmentationString.resize(AugmentationStringSize);
  AS.getU8(Offset, reinterpret_cast<uint8_t *>(AugmentationString.data()),
           AugmentationStringSize);
  *Offset = alignTo(*Offset, 4);
  return Error::success();
}

void DWARFDebugNames::Abbrev::dump(ScopedPrinter &W) const {
  DictScope AbbrevScope(W, ("Abbreviation 0x" + Twine::utohexstr(Code)).str());
  W.startLine() << "Tag: " << formatTag(Tag) << '\n';

  for (const auto &Attr : Attributes) {
    W.startLine() << formatIndex(Attr.Index) << ": " << formatForm(Attr.Form)
                  << '\n';
  }
}

static constexpr DWARFDebugNames::AttributeEncoding sentinelAttrEnc() {
  return {dwarf::Index(0), dwarf::Form(0)};
}

static bool isSentinel(const DWARFDebugNames::AttributeEncoding &AE) {
  return AE == sentinelAttrEnc();
}

static DWARFDebugNames::Abbrev sentinelAbbrev() {
  return DWARFDebugNames::Abbrev(0, dwarf::Tag(0), {});
}

static bool isSentinel(const DWARFDebugNames::Abbrev &Abbr) {
  return Abbr.Code == 0;
}

DWARFDebugNames::Abbrev DWARFDebugNames::AbbrevMapInfo::getEmptyKey() {
  return sentinelAbbrev();
}

DWARFDebugNames::Abbrev DWARFDebugNames::AbbrevMapInfo::getTombstoneKey() {
  return DWARFDebugNames::Abbrev(~0, dwarf::Tag(0), {});
}

Expected<DWARFDebugNames::AttributeEncoding>
DWARFDebugNames::NameIndex::extractAttributeEncoding(uint32_t *Offset) {
  if (*Offset >= EntriesBase) {
    return make_error<StringError>("Incorrectly terminated abbreviation table.",
                                   inconvertibleErrorCode());
  }

  uint32_t Index = Section.AccelSection.getULEB128(Offset);
  uint32_t Form = Section.AccelSection.getULEB128(Offset);
  return AttributeEncoding(dwarf::Index(Index), dwarf::Form(Form));
}

Expected<std::vector<DWARFDebugNames::AttributeEncoding>>
DWARFDebugNames::NameIndex::extractAttributeEncodings(uint32_t *Offset) {
  std::vector<AttributeEncoding> Result;
  for (;;) {
    auto AttrEncOr = extractAttributeEncoding(Offset);
    if (!AttrEncOr)
      return AttrEncOr.takeError();
    if (isSentinel(*AttrEncOr))
      return std::move(Result);

    Result.emplace_back(*AttrEncOr);
  }
}

Expected<DWARFDebugNames::Abbrev>
DWARFDebugNames::NameIndex::extractAbbrev(uint32_t *Offset) {
  if (*Offset >= EntriesBase) {
    return make_error<StringError>("Incorrectly terminated abbreviation table.",
                                   inconvertibleErrorCode());
  }

  uint32_t Code = Section.AccelSection.getULEB128(Offset);
  if (Code == 0)
    return sentinelAbbrev();

  uint32_t Tag = Section.AccelSection.getULEB128(Offset);
  auto AttrEncOr = extractAttributeEncodings(Offset);
  if (!AttrEncOr)
    return AttrEncOr.takeError();
  return Abbrev(Code, dwarf::Tag(Tag), std::move(*AttrEncOr));
}

Error DWARFDebugNames::NameIndex::extract() {
  const DWARFDataExtractor &AS = Section.AccelSection;
  uint32_t Offset = Base;
  if (Error E = Hdr.extract(AS, &Offset))
    return E;

  CUsBase = Offset;
  Offset += Hdr.CompUnitCount * 4;
  Offset += Hdr.LocalTypeUnitCount * 4;
  Offset += Hdr.ForeignTypeUnitCount * 8;
  BucketsBase = Offset;
  Offset += Hdr.BucketCount * 4;
  HashesBase = Offset;
  if (Hdr.BucketCount > 0)
    Offset += Hdr.NameCount * 4;
  StringOffsetsBase = Offset;
  Offset += Hdr.NameCount * 4;
  EntryOffsetsBase = Offset;
  Offset += Hdr.NameCount * 4;

  if (!AS.isValidOffsetForDataOfSize(Offset, Hdr.AbbrevTableSize))
    return make_error<StringError>(
        "Section too small: cannot read abbreviations.",
        inconvertibleErrorCode());

  EntriesBase = Offset + Hdr.AbbrevTableSize;

  for (;;) {
    auto AbbrevOr = extractAbbrev(&Offset);
    if (!AbbrevOr)
      return AbbrevOr.takeError();
    if (isSentinel(*AbbrevOr))
      return Error::success();

    if (!Abbrevs.insert(std::move(*AbbrevOr)).second) {
      return make_error<StringError>("Duplicate abbreviation code.",
                                     inconvertibleErrorCode());
    }
  }
}

DWARFDebugNames::Entry::Entry(const Abbrev &Abbr) : Abbr(Abbr) {
  // This merely creates form values. It is up to the caller
  // (NameIndex::getEntry) to populate them.
  Values.reserve(Abbr.Attributes.size());
  for (const auto &Attr : Abbr.Attributes)
    Values.emplace_back(Attr.Form);
}

void DWARFDebugNames::Entry::dump(ScopedPrinter &W) const {
  W.printHex("Abbrev", Abbr.Code);
  W.startLine() << "Tag: " << formatTag(Abbr.Tag) << "\n";

  assert(Abbr.Attributes.size() == Values.size());
  for (uint32_t I = 0, E = Values.size(); I < E; ++I) {
    W.startLine() << formatIndex(Abbr.Attributes[I].Index) << ": ";
    Values[I].dump(W.getOStream());
    W.getOStream() << '\n';
  }
}

char DWARFDebugNames::SentinelError::ID;
std::error_code DWARFDebugNames::SentinelError::convertToErrorCode() const {
  return inconvertibleErrorCode();
}

uint32_t DWARFDebugNames::NameIndex::getCUOffset(uint32_t CU) const {
  assert(CU < Hdr.CompUnitCount);
  uint32_t Offset = CUsBase + 4 * CU;
  return Section.AccelSection.getRelocatedValue(4, &Offset);
}

uint32_t DWARFDebugNames::NameIndex::getLocalTUOffset(uint32_t TU) const {
  assert(TU < Hdr.LocalTypeUnitCount);
  uint32_t Offset = CUsBase + Hdr.CompUnitCount * 4;
  return Section.AccelSection.getRelocatedValue(4, &Offset);
}

uint64_t DWARFDebugNames::NameIndex::getForeignTUOffset(uint32_t TU) const {
  assert(TU < Hdr.ForeignTypeUnitCount);
  uint32_t Offset = CUsBase + (Hdr.CompUnitCount + Hdr.LocalTypeUnitCount) * 4;
  return Section.AccelSection.getU64(&Offset);
}

Expected<DWARFDebugNames::Entry>
DWARFDebugNames::NameIndex::getEntry(uint32_t *Offset) const {
  const DWARFDataExtractor &AS = Section.AccelSection;
  if (!AS.isValidOffset(*Offset))
    return make_error<StringError>("Incorrectly terminated entry list",
                                   inconvertibleErrorCode());

  uint32_t AbbrevCode = AS.getULEB128(Offset);
  if (AbbrevCode == 0)
    return make_error<SentinelError>();

  const auto AbbrevIt = Abbrevs.find_as(AbbrevCode);
  if (AbbrevIt == Abbrevs.end())
    return make_error<StringError>("Invalid abbreviation",
                                   inconvertibleErrorCode());

  Entry E(*AbbrevIt);

  DWARFFormParams FormParams = {Hdr.Version, 0, dwarf::DwarfFormat::DWARF32};
  for (auto &Value : E.Values) {
    if (!Value.extractValue(AS, Offset, FormParams))
      return make_error<StringError>("Error extracting index attribute values",
                                     inconvertibleErrorCode());
  }
  return std::move(E);
}

DWARFDebugNames::NameTableEntry
DWARFDebugNames::NameIndex::getNameTableEntry(uint32_t Index) const {
  assert(0 < Index && Index <= Hdr.NameCount);
  uint32_t StringOffsetOffset = StringOffsetsBase + 4 * (Index - 1);
  uint32_t EntryOffsetOffset = EntryOffsetsBase + 4 * (Index - 1);
  const DWARFDataExtractor &AS = Section.AccelSection;

  uint32_t StringOffset = AS.getRelocatedValue(4, &StringOffsetOffset);
  uint32_t EntryOffset = AS.getU32(&EntryOffsetOffset);
  EntryOffset += EntriesBase;
  return {StringOffset, EntryOffset};
}

uint32_t
DWARFDebugNames::NameIndex::getBucketArrayEntry(uint32_t Bucket) const {
  assert(Bucket < Hdr.BucketCount);
  uint32_t BucketOffset = BucketsBase + 4 * Bucket;
  return Section.AccelSection.getU32(&BucketOffset);
}

uint32_t DWARFDebugNames::NameIndex::getHashArrayEntry(uint32_t Index) const {
  assert(0 < Index && Index <= Hdr.NameCount);
  uint32_t HashOffset = HashesBase + 4 * (Index - 1);
  return Section.AccelSection.getU32(&HashOffset);
}

// Returns true if we should continue scanning for entries, false if this is the
// last (sentinel) entry). In case of a parsing error we also return false, as
// it's not possible to recover this entry list (but the other lists may still
// parse OK).
bool DWARFDebugNames::NameIndex::dumpEntry(ScopedPrinter &W,
                                           uint32_t *Offset) const {
  uint32_t EntryId = *Offset;
  auto EntryOr = getEntry(Offset);
  if (!EntryOr) {
    handleAllErrors(EntryOr.takeError(), [](const SentinelError &) {},
                    [&W](const ErrorInfoBase &EI) { EI.log(W.startLine()); });
    return false;
  }

  DictScope EntryScope(W, ("Entry @ 0x" + Twine::utohexstr(EntryId)).str());
  EntryOr->dump(W);
  return true;
}

void DWARFDebugNames::NameIndex::dumpName(ScopedPrinter &W, uint32_t Index,
                                          Optional<uint32_t> Hash) const {
  const DataExtractor &SS = Section.StringSection;
  NameTableEntry NTE = getNameTableEntry(Index);

  DictScope NameScope(W, ("Name " + Twine(Index)).str());
  if (Hash)
    W.printHex("Hash", *Hash);

  W.startLine() << format("String: 0x%08x", NTE.StringOffset);
  W.getOStream() << " \"" << SS.getCStr(&NTE.StringOffset) << "\"\n";

  while (dumpEntry(W, &NTE.EntryOffset))
    /*empty*/;
}

void DWARFDebugNames::NameIndex::dumpCUs(ScopedPrinter &W) const {
  ListScope CUScope(W, "Compilation Unit offsets");
  for (uint32_t CU = 0; CU < Hdr.CompUnitCount; ++CU)
    W.startLine() << format("CU[%u]: 0x%08x\n", CU, getCUOffset(CU));
}

void DWARFDebugNames::NameIndex::dumpLocalTUs(ScopedPrinter &W) const {
  if (Hdr.LocalTypeUnitCount == 0)
    return;

  ListScope TUScope(W, "Local Type Unit offsets");
  for (uint32_t TU = 0; TU < Hdr.LocalTypeUnitCount; ++TU)
    W.startLine() << format("LocalTU[%u]: 0x%08x\n", TU, getLocalTUOffset(TU));
}

void DWARFDebugNames::NameIndex::dumpForeignTUs(ScopedPrinter &W) const {
  if (Hdr.ForeignTypeUnitCount == 0)
    return;

  ListScope TUScope(W, "Foreign Type Unit signatures");
  for (uint32_t TU = 0; TU < Hdr.ForeignTypeUnitCount; ++TU) {
    W.startLine() << format("ForeignTU[%u]: 0x%016" PRIx64 "\n", TU,
                            getForeignTUOffset(TU));
  }
}

void DWARFDebugNames::NameIndex::dumpAbbreviations(ScopedPrinter &W) const {
  ListScope AbbrevsScope(W, "Abbreviations");
  for (const auto &Abbr : Abbrevs)
    Abbr.dump(W);
}

void DWARFDebugNames::NameIndex::dumpBucket(ScopedPrinter &W,
                                            uint32_t Bucket) const {
  ListScope BucketScope(W, ("Bucket " + Twine(Bucket)).str());
  uint32_t Index = getBucketArrayEntry(Bucket);
  if (Index == 0) {
    W.printString("EMPTY");
    return;
  }
  if (Index > Hdr.NameCount) {
    W.printString("Name index is invalid");
    return;
  }

  for (; Index <= Hdr.NameCount; ++Index) {
    uint32_t Hash = getHashArrayEntry(Index);
    if (Hash % Hdr.BucketCount != Bucket)
      break;

    dumpName(W, Index, Hash);
  }
}

LLVM_DUMP_METHOD void DWARFDebugNames::NameIndex::dump(ScopedPrinter &W) const {
  DictScope UnitScope(W, ("Name Index @ 0x" + Twine::utohexstr(Base)).str());
  Hdr.dump(W);
  dumpCUs(W);
  dumpLocalTUs(W);
  dumpForeignTUs(W);
  dumpAbbreviations(W);

  if (Hdr.BucketCount > 0) {
    for (uint32_t Bucket = 0; Bucket < Hdr.BucketCount; ++Bucket)
      dumpBucket(W, Bucket);
    return;
  }

  W.startLine() << "Hash table not present\n";
  for (uint32_t Index = 1; Index <= Hdr.NameCount; ++Index)
    dumpName(W, Index, None);
}

llvm::Error DWARFDebugNames::extract() {
  uint32_t Offset = 0;
  while (AccelSection.isValidOffset(Offset)) {
    NameIndex Next(*this, Offset);
    if (llvm::Error E = Next.extract())
      return E;
    Offset = Next.getNextUnitOffset();
    NameIndices.push_back(std::move(Next));
  }
  return Error::success();
}

LLVM_DUMP_METHOD void DWARFDebugNames::dump(raw_ostream &OS) const {
  ScopedPrinter W(OS);
  for (const NameIndex &NI : NameIndices)
    NI.dump(W);
}

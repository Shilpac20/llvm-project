//===- DebugTypes.cpp -----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DebugTypes.h"
#include "Chunks.h"
#include "Driver.h"
#include "InputFiles.h"
#include "PDB.h"
#include "TypeMerger.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "lld/Common/Timer.h"
#include "llvm/DebugInfo/CodeView/TypeIndexDiscovery.h"
#include "llvm/DebugInfo/CodeView/TypeRecord.h"
#include "llvm/DebugInfo/CodeView/TypeRecordHelpers.h"
#include "llvm/DebugInfo/CodeView/TypeStreamMerger.h"
#include "llvm/DebugInfo/PDB/GenericError.h"
#include "llvm/DebugInfo/PDB/Native/InfoStream.h"
#include "llvm/DebugInfo/PDB/Native/NativeSession.h"
#include "llvm/DebugInfo/PDB/Native/PDBFile.h"
#include "llvm/DebugInfo/PDB/Native/TpiHashing.h"
#include "llvm/DebugInfo/PDB/Native/TpiStream.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/Path.h"

using namespace llvm;
using namespace llvm::codeview;
using namespace lld;
using namespace lld::coff;

namespace {
class TypeServerIpiSource;

// The TypeServerSource class represents a PDB type server, a file referenced by
// OBJ files compiled with MSVC /Zi. A single PDB can be shared by several OBJ
// files, therefore there must be only once instance per OBJ lot. The file path
// is discovered from the dependent OBJ's debug type stream. The
// TypeServerSource object is then queued and loaded by the COFF Driver. The
// debug type stream for such PDB files will be merged first in the final PDB,
// before any dependent OBJ.
class TypeServerSource : public TpiSource {
public:
  explicit TypeServerSource(PDBInputFile *f)
      : TpiSource(PDB, nullptr), pdbInputFile(f) {
    if (f->loadErr && *f->loadErr)
      return;
    pdb::PDBFile &file = f->session->getPDBFile();
    auto expectedInfo = file.getPDBInfoStream();
    if (!expectedInfo)
      return;
    auto it = mappings.emplace(expectedInfo->getGuid(), this);
    assert(it.second);
    (void)it;
  }

  Error mergeDebugT(TypeMerger *m) override;

  void loadGHashes() override;
  void remapTpiWithGHashes(GHashState *g) override;

  bool isDependency() const override { return true; }

  PDBInputFile *pdbInputFile = nullptr;

  // TpiSource for IPI stream.
  TypeServerIpiSource *ipiSrc = nullptr;

  static std::map<codeview::GUID, TypeServerSource *> mappings;
};

// Companion to TypeServerSource. Stores the index map for the IPI stream in the
// PDB. Modeling PDBs with two sources for TPI and IPI helps establish the
// invariant of one type index space per source.
class TypeServerIpiSource : public TpiSource {
public:
  explicit TypeServerIpiSource() : TpiSource(PDBIpi, nullptr) {}

  friend class TypeServerSource;

  // All of the TpiSource methods are no-ops. The parent TypeServerSource
  // handles both TPI and IPI.
  Error mergeDebugT(TypeMerger *m) override { return Error::success(); }
  void loadGHashes() override {}
  void remapTpiWithGHashes(GHashState *g) override {}
  bool isDependency() const override { return true; }
};

// This class represents the debug type stream of an OBJ file that depends on a
// PDB type server (see TypeServerSource).
class UseTypeServerSource : public TpiSource {
  Expected<TypeServerSource *> getTypeServerSource();

public:
  UseTypeServerSource(ObjFile *f, TypeServer2Record ts)
      : TpiSource(UsingPDB, f), typeServerDependency(ts) {}

  Error mergeDebugT(TypeMerger *m) override;

  // No need to load ghashes from /Zi objects.
  void loadGHashes() override {}
  void remapTpiWithGHashes(GHashState *g) override;

  // Information about the PDB type server dependency, that needs to be loaded
  // in before merging this OBJ.
  TypeServer2Record typeServerDependency;
};

// This class represents the debug type stream of a Microsoft precompiled
// headers OBJ (PCH OBJ). This OBJ kind needs to be merged first in the output
// PDB, before any other OBJs that depend on this. Note that only MSVC generate
// such files, clang does not.
class PrecompSource : public TpiSource {
public:
  PrecompSource(ObjFile *f) : TpiSource(PCH, f) {
    if (!f->pchSignature || !*f->pchSignature)
      fatal(toString(f) +
            " claims to be a PCH object, but does not have a valid signature");
    auto it = mappings.emplace(*f->pchSignature, this);
    if (!it.second)
      fatal("a PCH object with the same signature has already been provided (" +
            toString(it.first->second->file) + " and " + toString(file) + ")");
  }

  void loadGHashes() override;

  bool isDependency() const override { return true; }

  static std::map<uint32_t, PrecompSource *> mappings;
};

// This class represents the debug type stream of an OBJ file that depends on a
// Microsoft precompiled headers OBJ (see PrecompSource).
class UsePrecompSource : public TpiSource {
public:
  UsePrecompSource(ObjFile *f, PrecompRecord precomp)
      : TpiSource(UsingPCH, f), precompDependency(precomp) {}

  Error mergeDebugT(TypeMerger *m) override;

  void loadGHashes() override;
  void remapTpiWithGHashes(GHashState *g) override;

private:
  Error mergeInPrecompHeaderObj();

public:
  // Information about the Precomp OBJ dependency, that needs to be loaded in
  // before merging this OBJ.
  PrecompRecord precompDependency;
};
} // namespace

std::vector<TpiSource *> TpiSource::instances;
ArrayRef<TpiSource *> TpiSource::dependencySources;
ArrayRef<TpiSource *> TpiSource::objectSources;

TpiSource::TpiSource(TpiKind k, ObjFile *f)
    : kind(k), tpiSrcIdx(instances.size()), file(f) {
  instances.push_back(this);
}

// Vtable key method.
TpiSource::~TpiSource() {
  // Silence any assertions about unchecked errors.
  consumeError(std::move(typeMergingError));
}

void TpiSource::sortDependencies() {
  // Order dependencies first, but preserve the existing order.
  std::vector<TpiSource *> deps;
  std::vector<TpiSource *> objs;
  for (TpiSource *s : instances)
    (s->isDependency() ? deps : objs).push_back(s);
  uint32_t numDeps = deps.size();
  uint32_t numObjs = objs.size();
  instances = std::move(deps);
  instances.insert(instances.end(), objs.begin(), objs.end());
  for (uint32_t i = 0, e = instances.size(); i < e; ++i)
    instances[i]->tpiSrcIdx = i;
  dependencySources = makeArrayRef(instances.data(), numDeps);
  objectSources = makeArrayRef(instances.data() + numDeps, numObjs);
}

TpiSource *lld::coff::makeTpiSource(ObjFile *file) {
  return make<TpiSource>(TpiSource::Regular, file);
}

TpiSource *lld::coff::makeTypeServerSource(PDBInputFile *pdbInputFile) {
  // Type server sources come in pairs: the TPI stream, and the IPI stream.
  auto *tpiSource = make<TypeServerSource>(pdbInputFile);
  if (pdbInputFile->session->getPDBFile().hasPDBIpiStream())
    tpiSource->ipiSrc = make<TypeServerIpiSource>();
  return tpiSource;
}

TpiSource *lld::coff::makeUseTypeServerSource(ObjFile *file,
                                              TypeServer2Record ts) {
  return make<UseTypeServerSource>(file, ts);
}

TpiSource *lld::coff::makePrecompSource(ObjFile *file) {
  return make<PrecompSource>(file);
}

TpiSource *lld::coff::makeUsePrecompSource(ObjFile *file,
                                           PrecompRecord precomp) {
  return make<UsePrecompSource>(file, precomp);
}

std::map<codeview::GUID, TypeServerSource *> TypeServerSource::mappings;

std::map<uint32_t, PrecompSource *> PrecompSource::mappings;

bool TpiSource::remapTypeIndex(TypeIndex &ti, TiRefKind refKind) const {
  if (ti.isSimple())
    return true;

  // This can be an item index or a type index. Choose the appropriate map.
  ArrayRef<TypeIndex> tpiOrIpiMap =
      (refKind == TiRefKind::IndexRef) ? ipiMap : tpiMap;
  if (ti.toArrayIndex() >= tpiOrIpiMap.size())
    return false;
  ti = tpiOrIpiMap[ti.toArrayIndex()];
  return true;
}

void TpiSource::remapRecord(MutableArrayRef<uint8_t> rec,
                            ArrayRef<TiReference> typeRefs) {
  MutableArrayRef<uint8_t> contents = rec.drop_front(sizeof(RecordPrefix));
  for (const TiReference &ref : typeRefs) {
    unsigned byteSize = ref.Count * sizeof(TypeIndex);
    if (contents.size() < ref.Offset + byteSize)
      fatal("symbol record too short");

    MutableArrayRef<TypeIndex> indices(
        reinterpret_cast<TypeIndex *>(contents.data() + ref.Offset), ref.Count);
    for (TypeIndex &ti : indices) {
      if (!remapTypeIndex(ti, ref.Kind)) {
        if (config->verbose) {
          uint16_t kind =
              reinterpret_cast<const RecordPrefix *>(rec.data())->RecordKind;
          StringRef fname = file ? file->getName() : "<unknown PDB>";
          log("failed to remap type index in record of kind 0x" +
              utohexstr(kind) + " in " + fname + " with bad " +
              (ref.Kind == TiRefKind::IndexRef ? "item" : "type") +
              " index 0x" + utohexstr(ti.getIndex()));
        }
        ti = TypeIndex(SimpleTypeKind::NotTranslated);
        continue;
      }
    }
  }
}

void TpiSource::remapTypesInTypeRecord(MutableArrayRef<uint8_t> rec) {
  // TODO: Handle errors similar to symbols.
  SmallVector<TiReference, 32> typeRefs;
  discoverTypeIndices(CVType(rec), typeRefs);
  remapRecord(rec, typeRefs);
}

bool TpiSource::remapTypesInSymbolRecord(MutableArrayRef<uint8_t> rec) {
  // Discover type index references in the record. Skip it if we don't
  // know where they are.
  SmallVector<TiReference, 32> typeRefs;
  if (!discoverTypeIndicesInSymbol(rec, typeRefs))
    return false;
  remapRecord(rec, typeRefs);
  return true;
}

// A COFF .debug$H section is currently a clang extension.  This function checks
// if a .debug$H section is in a format that we expect / understand, so that we
// can ignore any sections which are coincidentally also named .debug$H but do
// not contain a format we recognize.
static bool canUseDebugH(ArrayRef<uint8_t> debugH) {
  if (debugH.size() < sizeof(object::debug_h_header))
    return false;
  auto *header =
      reinterpret_cast<const object::debug_h_header *>(debugH.data());
  debugH = debugH.drop_front(sizeof(object::debug_h_header));
  return header->Magic == COFF::DEBUG_HASHES_SECTION_MAGIC &&
         header->Version == 0 &&
         header->HashAlgorithm == uint16_t(GlobalTypeHashAlg::SHA1_8) &&
         (debugH.size() % 8 == 0);
}

static Optional<ArrayRef<uint8_t>> getDebugH(ObjFile *file) {
  SectionChunk *sec =
      SectionChunk::findByName(file->getDebugChunks(), ".debug$H");
  if (!sec)
    return llvm::None;
  ArrayRef<uint8_t> contents = sec->getContents();
  if (!canUseDebugH(contents))
    return None;
  return contents;
}

static ArrayRef<GloballyHashedType>
getHashesFromDebugH(ArrayRef<uint8_t> debugH) {
  assert(canUseDebugH(debugH));
  debugH = debugH.drop_front(sizeof(object::debug_h_header));
  uint32_t count = debugH.size() / sizeof(GloballyHashedType);
  return {reinterpret_cast<const GloballyHashedType *>(debugH.data()), count};
}

// Merge .debug$T for a generic object file.
Error TpiSource::mergeDebugT(TypeMerger *m) {
  assert(!config->debugGHashes &&
         "use remapTpiWithGHashes when ghash is enabled");

  CVTypeArray types;
  BinaryStreamReader reader(file->debugTypes, support::little);
  cantFail(reader.readArray(types, reader.getLength()));

  if (auto err = mergeTypeAndIdRecords(
          m->idTable, m->typeTable, indexMapStorage, types, file->pchSignature))
    fatal("codeview::mergeTypeAndIdRecords failed: " +
          toString(std::move(err)));

  // In an object, there is only one mapping for both types and items.
  tpiMap = indexMapStorage;
  ipiMap = indexMapStorage;

  if (config->showSummary) {
    // Count how many times we saw each type record in our input. This
    // calculation requires a second pass over the type records to classify each
    // record as a type or index. This is slow, but this code executes when
    // collecting statistics.
    m->tpiCounts.resize(m->getTypeTable().size());
    m->ipiCounts.resize(m->getIDTable().size());
    uint32_t srcIdx = 0;
    for (CVType &ty : types) {
      TypeIndex dstIdx = tpiMap[srcIdx++];
      // Type merging may fail, so a complex source type may become the simple
      // NotTranslated type, which cannot be used as an array index.
      if (dstIdx.isSimple())
        continue;
      SmallVectorImpl<uint32_t> &counts =
          isIdRecord(ty.kind()) ? m->ipiCounts : m->tpiCounts;
      ++counts[dstIdx.toArrayIndex()];
    }
  }

  return Error::success();
}

// Merge types from a type server PDB.
Error TypeServerSource::mergeDebugT(TypeMerger *m) {
  assert(!config->debugGHashes &&
         "use remapTpiWithGHashes when ghash is enabled");

  pdb::PDBFile &pdbFile = pdbInputFile->session->getPDBFile();
  Expected<pdb::TpiStream &> expectedTpi = pdbFile.getPDBTpiStream();
  if (auto e = expectedTpi.takeError())
    fatal("Type server does not have TPI stream: " + toString(std::move(e)));
  pdb::TpiStream *maybeIpi = nullptr;
  if (pdbFile.hasPDBIpiStream()) {
    Expected<pdb::TpiStream &> expectedIpi = pdbFile.getPDBIpiStream();
    if (auto e = expectedIpi.takeError())
      fatal("Error getting type server IPI stream: " + toString(std::move(e)));
    maybeIpi = &*expectedIpi;
  }

  // Merge TPI first, because the IPI stream will reference type indices.
  if (auto err = mergeTypeRecords(m->typeTable, indexMapStorage,
                                  expectedTpi->typeArray()))
    fatal("codeview::mergeTypeRecords failed: " + toString(std::move(err)));
  tpiMap = indexMapStorage;

  // Merge IPI.
  if (maybeIpi) {
    if (auto err = mergeIdRecords(m->idTable, tpiMap, ipiSrc->indexMapStorage,
                                  maybeIpi->typeArray()))
      fatal("codeview::mergeIdRecords failed: " + toString(std::move(err)));
    ipiMap = ipiSrc->indexMapStorage;
  }

  if (config->showSummary) {
    // Count how many times we saw each type record in our input. If a
    // destination type index is present in the source to destination type index
    // map, that means we saw it once in the input. Add it to our histogram.
    m->tpiCounts.resize(m->getTypeTable().size());
    m->ipiCounts.resize(m->getIDTable().size());
    for (TypeIndex ti : tpiMap)
      if (!ti.isSimple())
        ++m->tpiCounts[ti.toArrayIndex()];
    for (TypeIndex ti : ipiMap)
      if (!ti.isSimple())
        ++m->ipiCounts[ti.toArrayIndex()];
  }

  return Error::success();
}

Expected<TypeServerSource *> UseTypeServerSource::getTypeServerSource() {
  const codeview::GUID &tsId = typeServerDependency.getGuid();
  StringRef tsPath = typeServerDependency.getName();

  TypeServerSource *tsSrc;
  auto it = TypeServerSource::mappings.find(tsId);
  if (it != TypeServerSource::mappings.end()) {
    tsSrc = it->second;
  } else {
    // The file failed to load, lookup by name
    PDBInputFile *pdb = PDBInputFile::findFromRecordPath(tsPath, file);
    if (!pdb)
      return createFileError(tsPath, errorCodeToError(std::error_code(
                                         ENOENT, std::generic_category())));
    // If an error occurred during loading, throw it now
    if (pdb->loadErr && *pdb->loadErr)
      return createFileError(tsPath, std::move(*pdb->loadErr));

    tsSrc = (TypeServerSource *)pdb->debugTypesObj;
  }
  return tsSrc;
}

Error UseTypeServerSource::mergeDebugT(TypeMerger *m) {
  Expected<TypeServerSource *> tsSrc = getTypeServerSource();
  if (!tsSrc)
    return tsSrc.takeError();

  pdb::PDBFile &pdbSession = (*tsSrc)->pdbInputFile->session->getPDBFile();
  auto expectedInfo = pdbSession.getPDBInfoStream();
  if (!expectedInfo)
    return expectedInfo.takeError();

  // Just because a file with a matching name was found and it was an actual
  // PDB file doesn't mean it matches.  For it to match the InfoStream's GUID
  // must match the GUID specified in the TypeServer2 record.
  if (expectedInfo->getGuid() != typeServerDependency.getGuid())
    return createFileError(
        typeServerDependency.getName(),
        make_error<pdb::PDBError>(pdb::pdb_error_code::signature_out_of_date));

  // Reuse the type index map of the type server.
  tpiMap = (*tsSrc)->tpiMap;
  ipiMap = (*tsSrc)->ipiMap;
  return Error::success();
}

static bool equalsPath(StringRef path1, StringRef path2) {
#if defined(_WIN32)
  return path1.equals_lower(path2);
#else
  return path1.equals(path2);
#endif
}

// Find by name an OBJ provided on the command line
static PrecompSource *findObjByName(StringRef fileNameOnly) {
  SmallString<128> currentPath;
  for (auto kv : PrecompSource::mappings) {
    StringRef currentFileName = sys::path::filename(kv.second->file->getName(),
                                                    sys::path::Style::windows);

    // Compare based solely on the file name (link.exe behavior)
    if (equalsPath(currentFileName, fileNameOnly))
      return kv.second;
  }
  return nullptr;
}

static PrecompSource *findPrecompSource(ObjFile *file, PrecompRecord &pr) {
  // Cross-compile warning: given that Clang doesn't generate LF_PRECOMP
  // records, we assume the OBJ comes from a Windows build of cl.exe. Thusly,
  // the paths embedded in the OBJs are in the Windows format.
  SmallString<128> prFileName =
      sys::path::filename(pr.getPrecompFilePath(), sys::path::Style::windows);

  auto it = PrecompSource::mappings.find(pr.getSignature());
  if (it != PrecompSource::mappings.end()) {
    return it->second;
  }
  // Lookup by name
  return findObjByName(prFileName);
}

static Expected<PrecompSource *> findPrecompMap(ObjFile *file,
                                                PrecompRecord &pr) {
  PrecompSource *precomp = findPrecompSource(file, pr);

  if (!precomp)
    return createFileError(
        pr.getPrecompFilePath(),
        make_error<pdb::PDBError>(pdb::pdb_error_code::no_matching_pch));

  if (pr.getSignature() != file->pchSignature)
    return createFileError(
        toString(file),
        make_error<pdb::PDBError>(pdb::pdb_error_code::no_matching_pch));

  if (pr.getSignature() != *precomp->file->pchSignature)
    return createFileError(
        toString(precomp->file),
        make_error<pdb::PDBError>(pdb::pdb_error_code::no_matching_pch));

  return precomp;
}

/// Merges a precompiled headers TPI map into the current TPI map. The
/// precompiled headers object will also be loaded and remapped in the
/// process.
Error UsePrecompSource::mergeInPrecompHeaderObj() {
  auto e = findPrecompMap(file, precompDependency);
  if (!e)
    return e.takeError();

  PrecompSource *precompSrc = *e;
  if (precompSrc->tpiMap.empty())
    return Error::success();

  assert(precompDependency.getStartTypeIndex() ==
         TypeIndex::FirstNonSimpleIndex);
  assert(precompDependency.getTypesCount() <= precompSrc->tpiMap.size());
  // Use the previously remapped index map from the precompiled headers.
  indexMapStorage.append(precompSrc->tpiMap.begin(),
                         precompSrc->tpiMap.begin() +
                             precompDependency.getTypesCount());

  if (config->debugGHashes)
    funcIdToType = precompSrc->funcIdToType; // FIXME: Save copy

  return Error::success();
}

Error UsePrecompSource::mergeDebugT(TypeMerger *m) {
  // This object was compiled with /Yu, so process the corresponding
  // precompiled headers object (/Yc) first. Some type indices in the current
  // object are referencing data in the precompiled headers object, so we need
  // both to be loaded.
  if (Error e = mergeInPrecompHeaderObj())
    return e;

  return TpiSource::mergeDebugT(m);
}

uint32_t TpiSource::countTypeServerPDBs() {
  return TypeServerSource::mappings.size();
}

uint32_t TpiSource::countPrecompObjs() {
  return PrecompSource::mappings.size();
}

void TpiSource::clear() {
  // Clean up any owned ghash allocations.
  clearGHashes();
  TpiSource::instances.clear();
  TypeServerSource::mappings.clear();
  PrecompSource::mappings.clear();
}

//===----------------------------------------------------------------------===//
// Parellel GHash type merging implementation.
//===----------------------------------------------------------------------===//

void TpiSource::loadGHashes() {
  if (Optional<ArrayRef<uint8_t>> debugH = getDebugH(file)) {
    ghashes = getHashesFromDebugH(*debugH);
    ownedGHashes = false;
  } else {
    CVTypeArray types;
    BinaryStreamReader reader(file->debugTypes, support::little);
    cantFail(reader.readArray(types, reader.getLength()));
    assignGHashesFromVector(GloballyHashedType::hashTypes(types));
  }

  fillIsItemIndexFromDebugT();
}

// Copies ghashes from a vector into an array. These are long lived, so it's
// worth the time to copy these into an appropriately sized vector to reduce
// memory usage.
void TpiSource::assignGHashesFromVector(
    std::vector<GloballyHashedType> &&hashVec) {
  GloballyHashedType *hashes = new GloballyHashedType[hashVec.size()];
  memcpy(hashes, hashVec.data(), hashVec.size() * sizeof(GloballyHashedType));
  ghashes = makeArrayRef(hashes, hashVec.size());
  ownedGHashes = true;
}

// Faster way to iterate type records. forEachTypeChecked is faster than
// iterating CVTypeArray. It avoids virtual readBytes calls in inner loops.
static void forEachTypeChecked(ArrayRef<uint8_t> types,
                               function_ref<void(const CVType &)> fn) {
  checkError(
      forEachCodeViewRecord<CVType>(types, [fn](const CVType &ty) -> Error {
        fn(ty);
        return Error::success();
      }));
}

// Walk over file->debugTypes and fill in the isItemIndex bit vector.
// TODO: Store this information in .debug$H so that we don't have to recompute
// it. This is the main bottleneck slowing down parallel ghashing with one
// thread over single-threaded ghashing.
void TpiSource::fillIsItemIndexFromDebugT() {
  uint32_t index = 0;
  isItemIndex.resize(ghashes.size());
  forEachTypeChecked(file->debugTypes, [&](const CVType &ty) {
    if (isIdRecord(ty.kind()))
      isItemIndex.set(index);
    ++index;
  });
}

void TpiSource::mergeTypeRecord(CVType ty) {
  // Decide if the merged type goes into TPI or IPI.
  bool isItem = isIdRecord(ty.kind());
  MergedInfo &merged = isItem ? mergedIpi : mergedTpi;

  // Copy the type into our mutable buffer.
  assert(ty.length() <= codeview::MaxRecordLength);
  size_t offset = merged.recs.size();
  size_t newSize = alignTo(ty.length(), 4);
  merged.recs.resize(offset + newSize);
  auto newRec = makeMutableArrayRef(&merged.recs[offset], newSize);
  memcpy(newRec.data(), ty.data().data(), newSize);

  // Fix up the record prefix and padding bytes if it required resizing.
  if (newSize != ty.length()) {
    reinterpret_cast<RecordPrefix *>(newRec.data())->RecordLen = newSize - 2;
    for (size_t i = ty.length(); i < newSize; ++i)
      newRec[i] = LF_PAD0 + (newSize - i);
  }

  // Remap the type indices in the new record.
  remapTypesInTypeRecord(newRec);
  uint32_t pdbHash = check(pdb::hashTypeRecord(CVType(newRec)));
  merged.recSizes.push_back(static_cast<uint16_t>(newSize));
  merged.recHashes.push_back(pdbHash);
}

void TpiSource::mergeUniqueTypeRecords(ArrayRef<uint8_t> typeRecords,
                                       TypeIndex beginIndex) {
  // Re-sort the list of unique types by index.
  if (kind == PDB)
    assert(std::is_sorted(uniqueTypes.begin(), uniqueTypes.end()));
  else
    llvm::sort(uniqueTypes);

  // Accumulate all the unique types into one buffer in mergedTypes.
  uint32_t ghashIndex = 0;
  auto nextUniqueIndex = uniqueTypes.begin();
  assert(mergedTpi.recs.empty());
  assert(mergedIpi.recs.empty());
  forEachTypeChecked(typeRecords, [&](const CVType &ty) {
    if (nextUniqueIndex != uniqueTypes.end() &&
        *nextUniqueIndex == ghashIndex) {
      mergeTypeRecord(ty);
      ++nextUniqueIndex;
    }
    if (ty.kind() == LF_FUNC_ID || ty.kind() == LF_MFUNC_ID) {
      bool success = ty.length() >= 12;
      TypeIndex srcFuncIdIndex = beginIndex + ghashIndex;
      TypeIndex funcId = srcFuncIdIndex;
      TypeIndex funcType;
      if (success) {
        funcType = *reinterpret_cast<const TypeIndex *>(&ty.data()[8]);
        success &= remapTypeIndex(funcId, TiRefKind::IndexRef);
        success &= remapTypeIndex(funcType, TiRefKind::TypeRef);
      }
      if (success) {
        funcIdToType.insert({funcId, funcType});
      } else {
        StringRef fname = file ? file->getName() : "<unknown PDB>";
        warn("corrupt LF_[M]FUNC_ID record 0x" +
             utohexstr(srcFuncIdIndex.getIndex()) + " in " + fname);
      }
    }
    ++ghashIndex;
  });
  assert(nextUniqueIndex == uniqueTypes.end() &&
         "failed to merge all desired records");
  assert(uniqueTypes.size() ==
             mergedTpi.recSizes.size() + mergedIpi.recSizes.size() &&
         "missing desired record");
}

void TpiSource::remapTpiWithGHashes(GHashState *g) {
  assert(config->debugGHashes && "ghashes must be enabled");
  fillMapFromGHashes(g, indexMapStorage);
  tpiMap = indexMapStorage;
  ipiMap = indexMapStorage;
  mergeUniqueTypeRecords(file->debugTypes);
  // TODO: Free all unneeded ghash resources now that we have a full index map.
}

// PDBs do not actually store global hashes, so when merging a type server
// PDB we have to synthesize global hashes.  To do this, we first synthesize
// global hashes for the TPI stream, since it is independent, then we
// synthesize hashes for the IPI stream, using the hashes for the TPI stream
// as inputs.
void TypeServerSource::loadGHashes() {
  // Don't hash twice.
  if (!ghashes.empty())
    return;
  pdb::PDBFile &pdbFile = pdbInputFile->session->getPDBFile();

  // Hash TPI stream.
  Expected<pdb::TpiStream &> expectedTpi = pdbFile.getPDBTpiStream();
  if (auto e = expectedTpi.takeError())
    fatal("Type server does not have TPI stream: " + toString(std::move(e)));
  assignGHashesFromVector(
      GloballyHashedType::hashTypes(expectedTpi->typeArray()));
  isItemIndex.resize(ghashes.size());

  // Hash IPI stream, which depends on TPI ghashes.
  if (!pdbFile.hasPDBIpiStream())
    return;
  Expected<pdb::TpiStream &> expectedIpi = pdbFile.getPDBIpiStream();
  if (auto e = expectedIpi.takeError())
    fatal("error retreiving IPI stream: " + toString(std::move(e)));
  ipiSrc->assignGHashesFromVector(
      GloballyHashedType::hashIds(expectedIpi->typeArray(), ghashes));

  // The IPI stream isItemIndex bitvector should be all ones.
  ipiSrc->isItemIndex.resize(ipiSrc->ghashes.size());
  ipiSrc->isItemIndex.set(0, ipiSrc->ghashes.size());
}

// Flatten discontiguous PDB type arrays to bytes so that we can use
// forEachTypeChecked instead of CVTypeArray iteration. Copying all types from
// type servers is faster than iterating all object files compiled with /Z7 with
// CVTypeArray, which has high overheads due to the virtual interface of
// BinaryStream::readBytes.
static ArrayRef<uint8_t> typeArrayToBytes(const CVTypeArray &types) {
  BinaryStreamRef stream = types.getUnderlyingStream();
  ArrayRef<uint8_t> debugTypes;
  checkError(stream.readBytes(0, stream.getLength(), debugTypes));
  return debugTypes;
}

// Merge types from a type server PDB.
void TypeServerSource::remapTpiWithGHashes(GHashState *g) {
  assert(config->debugGHashes && "ghashes must be enabled");

  // IPI merging depends on TPI, so do TPI first, then do IPI.  No need to
  // propagate errors, those should've been handled during ghash loading.
  pdb::PDBFile &pdbFile = pdbInputFile->session->getPDBFile();
  pdb::TpiStream &tpi = check(pdbFile.getPDBTpiStream());
  fillMapFromGHashes(g, indexMapStorage);
  tpiMap = indexMapStorage;
  mergeUniqueTypeRecords(typeArrayToBytes(tpi.typeArray()));
  if (pdbFile.hasPDBIpiStream()) {
    pdb::TpiStream &ipi = check(pdbFile.getPDBIpiStream());
    ipiSrc->indexMapStorage.resize(ipiSrc->ghashes.size());
    ipiSrc->fillMapFromGHashes(g, ipiSrc->indexMapStorage);
    ipiMap = ipiSrc->indexMapStorage;
    ipiSrc->tpiMap = tpiMap;
    ipiSrc->ipiMap = ipiMap;
    ipiSrc->mergeUniqueTypeRecords(typeArrayToBytes(ipi.typeArray()));
    funcIdToType = ipiSrc->funcIdToType; // FIXME: Save copy
  }
}

void UseTypeServerSource::remapTpiWithGHashes(GHashState *g) {
  // No remapping to do with /Zi objects. Simply use the index map from the type
  // server. Errors should have been reported earlier. Symbols from this object
  // will be ignored.
  Expected<TypeServerSource *> maybeTsSrc = getTypeServerSource();
  if (!maybeTsSrc) {
    typeMergingError =
        joinErrors(std::move(typeMergingError), maybeTsSrc.takeError());
    return;
  }
  TypeServerSource *tsSrc = *maybeTsSrc;
  tpiMap = tsSrc->tpiMap;
  ipiMap = tsSrc->ipiMap;
  funcIdToType = tsSrc->funcIdToType; // FIXME: Save copy
}

void PrecompSource::loadGHashes() {
  if (getDebugH(file)) {
    warn("ignoring .debug$H section; pch with ghash is not implemented");
  }

  uint32_t ghashIdx = 0;
  std::vector<GloballyHashedType> hashVec;
  forEachTypeChecked(file->debugTypes, [&](const CVType &ty) {
    // Remember the index of the LF_ENDPRECOMP record so it can be excluded from
    // the PDB. There must be an entry in the list of ghashes so that the type
    // indexes of the following records in the /Yc PCH object line up.
    if (ty.kind() == LF_ENDPRECOMP)
      endPrecompGHashIdx = ghashIdx;

    hashVec.push_back(GloballyHashedType::hashType(ty, hashVec, hashVec));
    isItemIndex.push_back(isIdRecord(ty.kind()));
    ++ghashIdx;
  });
  assignGHashesFromVector(std::move(hashVec));
}

void UsePrecompSource::loadGHashes() {
  PrecompSource *pchSrc = findPrecompSource(file, precompDependency);
  if (!pchSrc)
    return;

  // To compute ghashes of a /Yu object file, we need to build on the the
  // ghashes of the /Yc PCH object. After we are done hashing, discard the
  // ghashes from the PCH source so we don't unnecessarily try to deduplicate
  // them.
  std::vector<GloballyHashedType> hashVec =
      pchSrc->ghashes.take_front(precompDependency.getTypesCount());
  forEachTypeChecked(file->debugTypes, [&](const CVType &ty) {
    hashVec.push_back(GloballyHashedType::hashType(ty, hashVec, hashVec));
    isItemIndex.push_back(isIdRecord(ty.kind()));
  });
  hashVec.erase(hashVec.begin(),
                hashVec.begin() + precompDependency.getTypesCount());
  assignGHashesFromVector(std::move(hashVec));
}

void UsePrecompSource::remapTpiWithGHashes(GHashState *g) {
  // This object was compiled with /Yu, so process the corresponding
  // precompiled headers object (/Yc) first. Some type indices in the current
  // object are referencing data in the precompiled headers object, so we need
  // both to be loaded.
  if (Error e = mergeInPrecompHeaderObj()) {
    typeMergingError = joinErrors(std::move(typeMergingError), std::move(e));
    return;
  }

  fillMapFromGHashes(g, indexMapStorage);
  tpiMap = indexMapStorage;
  ipiMap = indexMapStorage;
  mergeUniqueTypeRecords(file->debugTypes,
                         TypeIndex(precompDependency.getStartTypeIndex() +
                                   precompDependency.getTypesCount()));
}

namespace {
/// A concurrent hash table for global type hashing. It is based on this paper:
/// Concurrent Hash Tables: Fast and General(?)!
/// https://dl.acm.org/doi/10.1145/3309206
///
/// This hash table is meant to be used in two phases:
/// 1. concurrent insertions
/// 2. concurrent reads
/// It does not support lookup, deletion, or rehashing. It uses linear probing.
///
/// The paper describes storing a key-value pair in two machine words.
/// Generally, the values stored in this map are type indices, and we can use
/// those values to recover the ghash key from a side table. This allows us to
/// shrink the table entries further at the cost of some loads, and sidesteps
/// the need for a 128 bit atomic compare-and-swap operation.
///
/// During insertion, a priority function is used to decide which insertion
/// should be preferred. This ensures that the output is deterministic. For
/// ghashing, lower tpiSrcIdx values (earlier inputs) are preferred.
///
class GHashCell;
struct GHashTable {
  GHashCell *table = nullptr;
  uint32_t tableSize = 0;

  GHashTable() = default;
  ~GHashTable();

  /// Initialize the table with the given size. Because the table cannot be
  /// resized, the initial size of the table must be large enough to contain all
  /// inputs, or insertion may not be able to find an empty cell.
  void init(uint32_t newTableSize);

  /// Insert the cell with the given ghash into the table. Return the insertion
  /// position in the table. It is safe for the caller to store the insertion
  /// position because the table cannot be resized.
  uint32_t insert(GloballyHashedType ghash, GHashCell newCell);
};

/// A ghash table cell for deduplicating types from TpiSources.
class GHashCell {
  uint64_t data = 0;

public:
  GHashCell() = default;

  // Construct data most to least significant so that sorting works well:
  // - isItem
  // - tpiSrcIdx
  // - ghashIdx
  // Add one to the tpiSrcIdx so that the 0th record from the 0th source has a
  // non-zero representation.
  GHashCell(bool isItem, uint32_t tpiSrcIdx, uint32_t ghashIdx)
      : data((uint64_t(isItem) << 63U) | (uint64_t(tpiSrcIdx + 1) << 32ULL) |
             ghashIdx) {
    assert(tpiSrcIdx == getTpiSrcIdx() && "round trip failure");
    assert(ghashIdx == getGHashIdx() && "round trip failure");
  }

  explicit GHashCell(uint64_t data) : data(data) {}

  // The empty cell is all zeros.
  bool isEmpty() const { return data == 0ULL; }

  /// Extract the tpiSrcIdx.
  uint32_t getTpiSrcIdx() const {
    return ((uint32_t)(data >> 32U) & 0x7FFFFFFF) - 1;
  }

  /// Extract the index into the ghash array of the TpiSource.
  uint32_t getGHashIdx() const { return (uint32_t)data; }

  bool isItem() const { return data & (1ULL << 63U); }

  /// Get the ghash key for this cell.
  GloballyHashedType getGHash() const {
    return TpiSource::instances[getTpiSrcIdx()]->ghashes[getGHashIdx()];
  }

  /// The priority function for the cell. The data is stored such that lower
  /// tpiSrcIdx and ghashIdx values are preferred, which means that type record
  /// from earlier sources are more likely to prevail.
  friend inline bool operator<(const GHashCell &l, const GHashCell &r) {
    return l.data < r.data;
  }
};
} // namespace

namespace lld {
namespace coff {
/// This type is just a wrapper around GHashTable with external linkage so it
/// can be used from a header.
struct GHashState {
  GHashTable table;
};
} // namespace coff
} // namespace lld

GHashTable::~GHashTable() { delete[] table; }

void GHashTable::init(uint32_t newTableSize) {
  table = new GHashCell[newTableSize];
  memset(table, 0, newTableSize * sizeof(GHashCell));
  tableSize = newTableSize;
}

uint32_t GHashTable::insert(GloballyHashedType ghash, GHashCell newCell) {
  assert(!newCell.isEmpty() && "cannot insert empty cell value");

  // FIXME: The low bytes of SHA1 have low entropy for short records, which
  // type records are. Swap the byte order for better entropy. A better ghash
  // won't need this.
  uint32_t startIdx =
      ByteSwap_64(*reinterpret_cast<uint64_t *>(&ghash)) % tableSize;

  // Do a linear probe starting at startIdx.
  uint32_t idx = startIdx;
  while (true) {
    // Run a compare and swap loop. There are four cases:
    // - cell is empty: CAS into place and return
    // - cell has matching key, earlier priority: do nothing, return
    // - cell has matching key, later priority: CAS into place and return
    // - cell has non-matching key: hash collision, probe next cell
    auto *cellPtr = reinterpret_cast<std::atomic<GHashCell> *>(&table[idx]);
    GHashCell oldCell(cellPtr->load());
    while (oldCell.isEmpty() || oldCell.getGHash() == ghash) {
      // Check if there is an existing ghash entry with a higher priority
      // (earlier ordering). If so, this is a duplicate, we are done.
      if (!oldCell.isEmpty() && oldCell < newCell)
        return idx;
      // Either the cell is empty, or our value is higher priority. Try to
      // compare and swap. If it succeeds, we are done.
      if (cellPtr->compare_exchange_weak(oldCell, newCell))
        return idx;
      // If the CAS failed, check this cell again.
    }

    // Advance the probe. Wrap around to the beginning if we run off the end.
    ++idx;
    idx = idx == tableSize ? 0 : idx;
    if (idx == startIdx) {
      // If this becomes an issue, we could mark failure and rehash from the
      // beginning with a bigger table. There is no difference between rehashing
      // internally and starting over.
      report_fatal_error("ghash table is full");
    }
  }
  llvm_unreachable("left infloop");
}

TypeMerger::TypeMerger(llvm::BumpPtrAllocator &alloc)
    : typeTable(alloc), idTable(alloc) {}

TypeMerger::~TypeMerger() = default;

void TypeMerger::mergeTypesWithGHash() {
  // Load ghashes. Do type servers and PCH objects first.
  {
    ScopedTimer t1(loadGHashTimer);
    parallelForEach(TpiSource::dependencySources,
                    [&](TpiSource *source) { source->loadGHashes(); });
    parallelForEach(TpiSource::objectSources,
                    [&](TpiSource *source) { source->loadGHashes(); });
  }

  ScopedTimer t2(mergeGHashTimer);
  GHashState ghashState;

  // Estimate the size of hash table needed to deduplicate ghashes. This *must*
  // be larger than the number of unique types, or hash table insertion may not
  // be able to find a vacant slot. Summing the input types guarantees this, but
  // it is a gross overestimate. The table size could be reduced to save memory,
  // but it would require implementing rehashing, and this table is generally
  // small compared to total memory usage, at eight bytes per input type record,
  // and most input type records are larger than eight bytes.
  size_t tableSize = 0;
  for (TpiSource *source : TpiSource::instances)
    tableSize += source->ghashes.size();

  // Cap the table size so that we can use 32-bit cell indices. Type indices are
  // also 32-bit, so this is an inherent PDB file format limit anyway.
  tableSize = std::min(size_t(INT32_MAX), tableSize);
  ghashState.table.init(static_cast<uint32_t>(tableSize));

  // Insert ghashes in parallel. During concurrent insertion, we cannot observe
  // the contents of the hash table cell, but we can remember the insertion
  // position. Because the table does not rehash, the position will not change
  // under insertion. After insertion is done, the value of the cell can be read
  // to retreive the final PDB type index.
  parallelForEachN(0, TpiSource::instances.size(), [&](size_t tpiSrcIdx) {
    TpiSource *source = TpiSource::instances[tpiSrcIdx];
    source->indexMapStorage.resize(source->ghashes.size());
    for (uint32_t i = 0, e = source->ghashes.size(); i < e; i++) {
      if (source->shouldOmitFromPdb(i)) {
        source->indexMapStorage[i] = TypeIndex(SimpleTypeKind::NotTranslated);
        continue;
      }
      GloballyHashedType ghash = source->ghashes[i];
      bool isItem = source->isItemIndex.test(i);
      uint32_t cellIdx =
          ghashState.table.insert(ghash, GHashCell(isItem, tpiSrcIdx, i));

      // Store the ghash cell index as a type index in indexMapStorage. Later
      // we will replace it with the PDB type index.
      source->indexMapStorage[i] = TypeIndex::fromArrayIndex(cellIdx);
    }
  });

  // Collect all non-empty cells and sort them. This will implicitly assign
  // destination type indices, and partition the entries into type records and
  // item records. It arranges types in this order:
  // - type records
  //   - source 0, type 0...
  //   - source 1, type 1...
  // - item records
  //   - source 0, type 1...
  //   - source 1, type 0...
  std::vector<GHashCell> entries;
  for (const GHashCell &cell :
       makeArrayRef(ghashState.table.table, tableSize)) {
    if (!cell.isEmpty())
      entries.push_back(cell);
  }
  parallelSort(entries, std::less<GHashCell>());
  log(formatv("ghash table load factor: {0:p} (size {1} / capacity {2})\n",
              double(entries.size()) / tableSize, entries.size(), tableSize));

  // Find out how many type and item indices there are.
  auto mid =
      std::lower_bound(entries.begin(), entries.end(), GHashCell(true, 0, 0));
  assert((mid == entries.end() || mid->isItem()) &&
         (mid == entries.begin() || !std::prev(mid)->isItem()) &&
         "midpoint is not midpoint");
  uint32_t numTypes = std::distance(entries.begin(), mid);
  uint32_t numItems = std::distance(mid, entries.end());
  log("Tpi record count: " + Twine(numTypes));
  log("Ipi record count: " + Twine(numItems));

  // Make a list of the "unique" type records to merge for each tpi source. Type
  // merging will skip indices not on this list. Store the destination PDB type
  // index for these unique types in the tpiMap for each source. The entries for
  // non-unique types will be filled in prior to type merging.
  for (uint32_t i = 0, e = entries.size(); i < e; ++i) {
    auto &cell = entries[i];
    uint32_t tpiSrcIdx = cell.getTpiSrcIdx();
    TpiSource *source = TpiSource::instances[tpiSrcIdx];
    source->uniqueTypes.push_back(cell.getGHashIdx());

    // Update the ghash table to store the destination PDB type index in the
    // table.
    uint32_t pdbTypeIndex = i < numTypes ? i : i - numTypes;
    uint32_t ghashCellIndex =
        source->indexMapStorage[cell.getGHashIdx()].toArrayIndex();
    ghashState.table.table[ghashCellIndex] =
        GHashCell(cell.isItem(), cell.getTpiSrcIdx(), pdbTypeIndex);
  }

  // In parallel, remap all types.
  for_each(TpiSource::dependencySources, [&](TpiSource *source) {
    source->remapTpiWithGHashes(&ghashState);
  });
  parallelForEach(TpiSource::objectSources, [&](TpiSource *source) {
    source->remapTpiWithGHashes(&ghashState);
  });

  TpiSource::clearGHashes();
}

/// Given the index into the ghash table for a particular type, return the type
/// index for that type in the output PDB.
static TypeIndex loadPdbTypeIndexFromCell(GHashState *g,
                                          uint32_t ghashCellIdx) {
  GHashCell cell = g->table.table[ghashCellIdx];
  return TypeIndex::fromArrayIndex(cell.getGHashIdx());
}

// Fill in a TPI or IPI index map using ghashes. For each source type, use its
// ghash to lookup its final type index in the PDB, and store that in the map.
void TpiSource::fillMapFromGHashes(GHashState *g,
                                   SmallVectorImpl<TypeIndex> &mapToFill) {
  for (size_t i = 0, e = ghashes.size(); i < e; ++i) {
    TypeIndex fakeCellIndex = indexMapStorage[i];
    if (fakeCellIndex.isSimple())
      mapToFill[i] = fakeCellIndex;
    else
      mapToFill[i] = loadPdbTypeIndexFromCell(g, fakeCellIndex.toArrayIndex());
  }
}

void TpiSource::clearGHashes() {
  for (TpiSource *src : TpiSource::instances) {
    if (src->ownedGHashes)
      delete[] src->ghashes.data();
    src->ghashes = {};
    src->isItemIndex.clear();
    src->uniqueTypes.clear();
  }
}

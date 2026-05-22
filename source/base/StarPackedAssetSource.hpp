#pragma once

#include "StarOrderedMap.hpp"
#include "StarFile.hpp"
#include "StarDirectoryAssetSource.hpp"

namespace Star {

STAR_CLASS(PackedAssetSource);

class PackedAssetSource : public AssetSource {
public:
  typedef function<void(size_t, size_t, String, String)> BuildProgressCallback;

  // Build a packed asset file from the given DirectoryAssetSource.
  //
  // 'extensionSorting' sorts the packed file with file extensions that case
  // insensitive match the given extensions in the order they are given.  If a
  // file has an extension that doesn't match any in this list, it goes after
  // all other files.  All files are sorted secondarily by case insensitive
  // alphabetical order.
  //
  // If given, 'progressCallback' will be called with the total number of
  // files, the current file number, the file name, and the asset path.
  static void build(DirectoryAssetSource& directorySource, String const& targetPackedFile,
      StringList const& extensionSorting = {}, BuildProgressCallback progressCallback = {});

  // Read only the metadata from a packed asset file without loading the full
  // asset index.  Useful for scanning / discovery when the index is not needed.
  static JsonObject readMetadata(String const& packedFileName);

  PackedAssetSource(String const& packedFileName);

  JsonObject metadata() const override;
  StringList assetPaths() const override;
  void forEachAssetPath(function<void(String const&)> callback) const override;
#ifdef STAR_PLATFORM_N3DS
  // Like forEachAssetPath but also provides the file offset and size within
  // the packed archive, allowing the index to be freed after descriptors are
  // built.
  void forEachAssetPathWithOffset(function<void(String const&, uint64_t offset, uint64_t size)> callback) const;
  // Free the in-memory index to save heap.  After calling this, open() and
  // read() will no longer work — use openAt(offset, size) instead.
  void releaseIndex();
  IODevicePtr openAt(uint64_t offset, uint64_t size, String const& path = String());
  ByteArray readAt(uint64_t offset, uint64_t size);
  // Fast O(1) lookup of offset+size by asset path. Returns true and sets
  // offset/size/originalName if found, false otherwise.  Used for lazy
  // descriptor building.  The lookup expects the path to be pre-normalised
  // (no leading '/', lowercased) to match the packed index format.
  bool findOffset(String const& path, uint64_t& offset, uint64_t& size, String& originalName) const;
#endif

  IODevicePtr open(String const& path) override;
  ByteArray read(String const& path) override;

private:
  FilePtr m_packedFile;
  JsonObject m_metadata;
  OrderedHashMap<String, pair<uint64_t, uint64_t>> m_index;
};

}

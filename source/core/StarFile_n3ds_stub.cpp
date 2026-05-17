#include "StarFile.hpp"
#include "StarFormat.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Star {

namespace {
  std::atomic<uint64_t> sTempCounter{0};
  String sCurrentDirectory = "sdmc:/OpenStarbound";
  bool isAbsolutePath(String const& path);

  String normalizeIoPath(String const& inputPath) {
    auto normalized = File::convertDirSeparators(inputPath).trim();
    if (normalized.empty())
      throw IOException("Invalid empty path");

    if (normalized.regexMatch("^[A-Za-z]+:$", false, false))
      normalized += "/";

    if (!isAbsolutePath(normalized))
      normalized = File::relativeTo(File::currentDirectory(), normalized);

    return normalized;
  }

  bool statPath(char const* path, struct stat& st) {
    return ::stat(path, &st) == 0;
  }

  bool isAbsolutePath(String const& path) {
    return path.beginsWith("/") || path.regexMatch("^[A-Za-z]+:/", false, false);
  }

  char const* modeString(IOMode mode) {
    if ((mode & IOMode::Read) && (mode & IOMode::Write)) {
      if (mode & IOMode::Append)
        return "ab+";
      if (mode & IOMode::Truncate)
        return "wb+";
      return "rb+";
    }

    if (mode & IOMode::Write) {
      if (mode & IOMode::Append)
        return "ab";
      return "wb";
    }

    return "rb";
  }
}

String File::convertDirSeparators(String const& path) {
  return path.replace("\\", "/");
}

String File::currentDirectory() {
  return sCurrentDirectory;
}

void File::changeDirectory(String const& dirName) {
  auto normalized = convertDirSeparators(dirName);
  if (normalized.empty())
    return;

  String nextDirectory;
  if (isAbsolutePath(normalized))
    nextDirectory = normalized.trimEnd("/");
  else
    nextDirectory = relativeTo(sCurrentDirectory, normalized).trimEnd("/");

  if (nextDirectory.regexMatch("^[A-Za-z]+:$", false, false))
    nextDirectory += "/";

  sCurrentDirectory = std::move(nextDirectory);
}

void File::makeDirectory(String const& dirName) {
  auto path = normalizeIoPath(dirName);
  if (::mkdir(path.utf8Ptr(), 0777) != 0 && errno != EEXIST)
    throw IOException::format("could not create directory '{}', {}", dirName, std::strerror(errno));
}

List<pair<String, bool>> File::dirList(String const& dirName, bool skipDots) {
  List<pair<String, bool>> fileList;
  auto path = normalizeIoPath(dirName);
  DIR* directory = ::opendir(path.utf8Ptr());
  if (directory == nullptr)
    throw IOException::format("dirList failed on dir: '{}'", dirName.utf8Ptr());

  for (dirent* entry = ::readdir(directory); entry != nullptr; entry = ::readdir(directory)) {
    String entryString = entry->d_name;
    if (!skipDots || (entryString != "." && entryString != "..")) {
      bool isDirectory = false;
      if (entry->d_type == DT_DIR) {
        isDirectory = true;
      } else if (entry->d_type == DT_LNK || entry->d_type == DT_UNKNOWN) {
        isDirectory = File::isDirectory(File::relativeTo(dirName, entryString));
      }
      fileList.append({entryString, isDirectory});
    }
  }

  ::closedir(directory);
  return fileList;
}

String File::baseName(String const& fileName) {
  auto idx = fileName.findLast('/');
  if (idx == NPos)
    idx = fileName.findLast('\\');
  if (idx == NPos)
    return fileName;
  return fileName.substr(idx + 1);
}

String File::dirName(String const& fileName) {
  auto idx = fileName.findLast('/');
  if (idx == NPos)
    idx = fileName.findLast('\\');
  if (idx == NPos)
    return ".";
  return fileName.substr(0, idx);
}

String File::relativeTo(String const& relativeBase, String const& path) {
  if (path.empty())
    return relativeBase.trimEnd("/");

  if (isAbsolutePath(path))
    return path;

  if (relativeBase.empty())
    return path;

  return relativeBase.trimEnd("/") + '/' + path;
}

String File::fullPath(String const& path) {
  if (path.empty())
    return currentDirectory();

  if (isAbsolutePath(path))
    return path;

  return relativeTo(currentDirectory(), path);
}

String File::temporaryFileName() {
  auto id = ++sTempCounter;
  return strf("/tmp/starbound.tmpfile.{}", id);
}

FilePtr File::temporaryFile() {
  return open(temporaryFileName(), IOMode::ReadWrite | IOMode::Truncate);
}

FilePtr File::ephemeralFile() {
  // STUB: no unlink-on-close behavior in phase 1 placeholder.
  return temporaryFile();
}

String File::temporaryDirectory() {
  String dirname = strf("/tmp/starbound.tmpdir.{}", ++sTempCounter);
  makeDirectory(dirname);
  return dirname;
}

bool File::exists(String const& path) {
  if (path.empty())
    return false;
  struct stat st;
  return statPath(normalizeIoPath(path).utf8Ptr(), st);
}

bool File::isFile(String const& path) {
  if (path.empty())
    return false;
  struct stat st;
  return statPath(normalizeIoPath(path).utf8Ptr(), st) && S_ISREG(st.st_mode);
}

bool File::isDirectory(String const& path) {
  if (path.empty())
    return false;
  struct stat st;
  return statPath(normalizeIoPath(path).utf8Ptr(), st) && S_ISDIR(st.st_mode);
}

void File::remove(String const& filename) {
  auto path = normalizeIoPath(filename);
  if (::remove(path.utf8Ptr()) != 0 && errno != ENOENT)
    throw IOException::format("remove error: {}", std::strerror(errno));
}

void File::rename(String const& source, String const& target) {
  auto sourcePath = normalizeIoPath(source);
  auto targetPath = normalizeIoPath(target);
  if (::rename(sourcePath.utf8Ptr(), targetPath.utf8Ptr()) != 0)
    throw IOException::format("rename error: {}", std::strerror(errno));
}

void File::overwriteFileWithRename(char const* data, size_t len, String const& filename, String const& newSuffix) {
  String newFile = filename + newSuffix;
  writeFile(data, len, newFile);
  File::rename(newFile, filename);
}

void* File::fopen(char const* filename, IOMode mode) {
  if (!filename || filename[0] == '\0')
    throw IOException("Error opening file with empty path");

  auto path = normalizeIoPath(filename);
#ifdef STAR_PLATFORM_N3DS
  static std::atomic<unsigned> sFopenTraceCount{0};
  unsigned traceIndex = ++sFopenTraceCount;
  if (traceIndex <= 256)
    std::fprintf(stderr, "OSBN3DS fopen[%u] mode=%d path=%s\n", traceIndex, static_cast<int>(mode), path.utf8Ptr());
#endif
  FILE* file = std::fopen(path.utf8Ptr(), modeString(mode));

  if (!file && (mode & IOMode::Read) && (mode & IOMode::Write) && !(mode & IOMode::Truncate) && !(mode & IOMode::Append)) {
    file = std::fopen(path.utf8Ptr(), "wb+");
  }

  if (!file)
    throw IOException::format("Error opening file '{}', error: {}", path, std::strerror(errno));

  if (mode & IOMode::Append)
    std::fseek(file, 0, SEEK_END);

  return file;
}

void File::fseek(void* file, StreamOffset offset, IOSeek seekMode) {
  int origin = SEEK_SET;
  if (seekMode == IOSeek::Relative)
    origin = SEEK_CUR;
  else if (seekMode == IOSeek::End)
    origin = SEEK_END;

  if (std::fseek(static_cast<FILE*>(file), static_cast<long>(offset), origin) != 0)
    throw IOException::format("Seek error: {}", std::strerror(errno));
}

StreamOffset File::ftell(void* file) {
  return static_cast<StreamOffset>(std::ftell(static_cast<FILE*>(file)));
}

size_t File::fread(void* file, char* data, size_t len) {
  if (len == 0)
    return 0;

  return std::fread(data, 1, len, static_cast<FILE*>(file));
}

size_t File::fwrite(void* file, char const* data, size_t len) {
  if (len == 0)
    return 0;

  return std::fwrite(data, 1, len, static_cast<FILE*>(file));
}

void File::fsync(void* file) {
  std::fflush(static_cast<FILE*>(file));
}

void File::fclose(void* file) {
  if (std::fclose(static_cast<FILE*>(file)) != 0)
    throw IOException::format("Close error: {}", std::strerror(errno));
}

StreamOffset File::fsize(void* file) {
  auto f = static_cast<FILE*>(file);
  auto pos = std::ftell(f);
  std::fseek(f, 0, SEEK_END);
  auto size = std::ftell(f);
  std::fseek(f, pos, SEEK_SET);
  return static_cast<StreamOffset>(size);
}

size_t File::pread(void* file, char* data, size_t len, StreamOffset position) {
  auto f = static_cast<FILE*>(file);
  auto pos = std::ftell(f);
  std::fseek(f, static_cast<long>(position), SEEK_SET);
  auto bytes = std::fread(data, 1, len, f);
  std::fseek(f, pos, SEEK_SET);
  return bytes;
}

size_t File::pwrite(void* file, char const* data, size_t len, StreamOffset position) {
  auto f = static_cast<FILE*>(file);
  auto pos = std::ftell(f);
  std::fseek(f, static_cast<long>(position), SEEK_SET);
  auto bytes = std::fwrite(data, 1, len, f);
  std::fseek(f, pos, SEEK_SET);
  return bytes;
}

void File::resize(void*, StreamOffset) {
  // STUB: file truncation/extension is not implemented in N3DS phase 1.
}

}

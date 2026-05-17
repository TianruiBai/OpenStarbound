#include "StarLockFile.hpp"

namespace Star {

int64_t const LockFile::MaximumSleepMillis;

Maybe<LockFile> LockFile::acquireLock(String const& filename, int64_t lockTimeout) {
  LockFile lockFile(filename);
  if (lockFile.lock(lockTimeout))
    return lockFile;

  return {};
}

LockFile::LockFile(String const& filename)
  : m_filename(std::move(filename)) {}

LockFile::LockFile(LockFile&& lockFile) {
  operator=(std::move(lockFile));
}

LockFile::~LockFile() {
  unlock();
}

LockFile& LockFile::operator=(LockFile&& lockFile) {
  unlock();
  m_filename = std::move(lockFile.m_filename);
  m_handle = take(lockFile.m_handle);
  return *this;
}

bool LockFile::lock(int64_t) {
  // STUB: phase 1 does not provide inter-process file locking on N3DS.
  if (m_handle)
    return true;

  m_handle = make_shared<int>(1);
  return true;
}

void LockFile::unlock() {
  m_handle.reset();
}

bool LockFile::isLocked() const {
  return (bool)m_handle;
}

}

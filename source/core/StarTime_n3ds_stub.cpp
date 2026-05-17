#include "StarTime.hpp"
#include "StarFormat.hpp"

#include <ctime>
#include <ctime>

namespace Star {

String Time::printDateAndTime(int64_t epochTicks, String format) {
  // PLACEHOLDER: minimal formatting until locale/date formatting is implemented for N3DS.
  return strf("{} [{}]", epochTicks, format);
}

int64_t Time::epochTicks() {
  // STUB: relies on C runtime wall-clock availability for phase 1 bootstrap.
  return static_cast<int64_t>(std::time(nullptr));
}

int64_t Time::epochTickFrequency() {
  return 1;
}

int64_t Time::monotonicTicks() {
  return static_cast<int64_t>(std::clock());
}

int64_t Time::monotonicTickFrequency() {
  return static_cast<int64_t>(CLOCKS_PER_SEC);
}

}

#include "StarTime.hpp"
#include "StarFormat.hpp"

#include <ctime>

#include <3ds.h>

namespace Star {

namespace {

constexpr uint64_t N3dsUnixEpochOffsetMillis = 2208988800ULL * 1000ULL;

}

String Time::printDateAndTime(int64_t epochTicks, String format) {
  auto requestedTime = static_cast<std::time_t>(epochTicks / epochTickFrequency());
  std::tm dateTime{};
  if (auto localTime = std::localtime(&requestedTime))
    dateTime = *localTime;

  return format.replaceTags(StringMap<String>{
      {"year", strf("{:04d}", dateTime.tm_year + 1900)},
      {"month", strf("{:02d}", dateTime.tm_mon + 1)},
      {"day", strf("{:02d}", dateTime.tm_mday)},
      {"hours", strf("{:02d}", dateTime.tm_hour)},
      {"minutes", strf("{:02d}", dateTime.tm_min)},
      {"seconds", strf("{:02d}", dateTime.tm_sec)},
      {"millis", strf("{:03d}", (epochTicks % epochTickFrequency()) / (epochTickFrequency() / 1000))}
    });
}

int64_t Time::epochTicks() {
  auto n3dsTimeMillis = osGetTime();
  if (n3dsTimeMillis <= N3dsUnixEpochOffsetMillis)
    return 0;

  return static_cast<int64_t>((n3dsTimeMillis - N3dsUnixEpochOffsetMillis) * 1000ULL);
}

int64_t Time::epochTickFrequency() {
  return 1000000;
}

int64_t Time::monotonicTicks() {
  return static_cast<int64_t>(svcGetSystemTick());
}

int64_t Time::monotonicTickFrequency() {
  return static_cast<int64_t>(SYSCLOCK_ARM11);
}

}

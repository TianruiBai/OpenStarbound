#pragma once

#include "StarString.hpp"

namespace Star {

struct ServerTimingAccumulator {
  uint64_t samples = 0;
  uint64_t totalMicroseconds = 0;
  uint64_t maxMicroseconds = 0;
  uint64_t latestMicroseconds = 0;
  List<uint64_t> recentSamples;
  size_t recentSampleIndex = 0;
};

struct ServerTimingRecord {
  String name;
  ServerTimingAccumulator timing;
};

struct ServerTimingStatus {
  String name;
  uint64_t samples = 0;
  uint64_t totalMicroseconds = 0;
  uint64_t averageMicroseconds = 0;
  uint64_t latestMicroseconds = 0;
  uint64_t maxMicroseconds = 0;
  uint64_t p50Microseconds = 0;
  uint64_t p95Microseconds = 0;
  uint64_t p99Microseconds = 0;
};

inline size_t constexpr ServerTimingSampleLimit = 256;

inline void recordServerTiming(ServerTimingAccumulator& timing, int64_t durationMicroseconds) {
  if (durationMicroseconds < 0)
    return;

  auto duration = static_cast<uint64_t>(durationMicroseconds);
  timing.samples++;
  timing.totalMicroseconds += duration;
  timing.latestMicroseconds = duration;
  if (duration > timing.maxMicroseconds)
    timing.maxMicroseconds = duration;

  if (timing.recentSamples.size() < ServerTimingSampleLimit)
    timing.recentSamples.append(duration);
  else {
    timing.recentSamples[timing.recentSampleIndex] = duration;
    timing.recentSampleIndex = (timing.recentSampleIndex + 1) % ServerTimingSampleLimit;
  }
}

inline void mergeServerTiming(ServerTimingAccumulator& target, ServerTimingAccumulator const& source) {
  target.samples += source.samples;
  target.totalMicroseconds += source.totalMicroseconds;
  target.latestMicroseconds = source.latestMicroseconds;
  if (source.maxMicroseconds > target.maxMicroseconds)
    target.maxMicroseconds = source.maxMicroseconds;

  for (auto sample : source.recentSamples) {
    if (target.recentSamples.size() < ServerTimingSampleLimit)
      target.recentSamples.append(sample);
    else {
      target.recentSamples[target.recentSampleIndex] = sample;
      target.recentSampleIndex = (target.recentSampleIndex + 1) % ServerTimingSampleLimit;
    }
  }
}

inline void mergeServerTimingRecord(List<ServerTimingRecord>& records, ServerTimingRecord const& source) {
  for (auto& record : records) {
    if (record.name == source.name) {
      mergeServerTiming(record.timing, source.timing);
      return;
    }
  }
  records.append(source);
}

inline ServerTimingStatus serverTimingStatus(String name, ServerTimingAccumulator const& timing) {
  ServerTimingStatus status;
  status.name = std::move(name);
  status.samples = timing.samples;
  status.totalMicroseconds = timing.totalMicroseconds;
  status.averageMicroseconds = timing.samples ? timing.totalMicroseconds / timing.samples : 0;
  status.latestMicroseconds = timing.latestMicroseconds;
  status.maxMicroseconds = timing.maxMicroseconds;

  auto samples = timing.recentSamples;
  if (!samples.empty()) {
    samples.sort();
    auto percentile = [&samples](size_t percent) {
      return samples[(samples.size() - 1) * percent / 100];
    };
    status.p50Microseconds = percentile(50);
    status.p95Microseconds = percentile(95);
    status.p99Microseconds = percentile(99);
  }

  return status;
}

inline ServerTimingStatus serverTimingStatus(ServerTimingRecord const& record) {
  return serverTimingStatus(record.name, record.timing);
}

inline List<ServerTimingStatus> serverTimingStatusList(List<ServerTimingRecord> const& records) {
  List<ServerTimingStatus> status;
  for (auto const& record : records)
    status.append(serverTimingStatus(record));
  return status;
}

}

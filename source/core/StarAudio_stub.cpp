#include "StarAudio.hpp"

#include <cmath>

namespace Star {

float const DefaultPerceptualRangeDb = 40.f;
float const DefaultPerceptualBoostRangeDb = 6.f;

float perceptualToAmplitude(float perceptual, float normalizedMax, float range, float boostRange) {
  if (perceptual == 0.f)
    return 0.f;
  float dB = perceptual > normalizedMax
    ? ((perceptual - normalizedMax) / normalizedMax) * boostRange
    : (perceptual / normalizedMax) * range - range;
  return normalizedMax * std::pow(10.f, dB / 20.f);
}

float amplitudeToPerceptual(float amp, float normalizedMax, float range, float boostRange) {
  if (amp == 0.f)
    return 0.f;
  float const dB = 20.f * std::log10(amp / normalizedMax);
  float perceptual = dB > 0.f
    ? dB / boostRange + 1
    : (range + dB) / range;
  return normalizedMax * perceptual;
}

Audio::Audio(IODevicePtr, String name)
  : m_name(std::move(name)) {
  // STUB: Nintendo 3DS phase1 does not include Vorbis/WAV decode path yet.
}

Audio::Audio(Audio const& audio) = default;
Audio::Audio(Audio&& audio) = default;

Audio& Audio::operator=(Audio const& audio) = default;
Audio& Audio::operator=(Audio&& audio) = default;

unsigned Audio::channels() const {
  return 0;
}

unsigned Audio::sampleRate() const {
  return 0;
}

double Audio::totalTime() const {
  return 0.0;
}

uint64_t Audio::totalSamples() const {
  return 0;
}

bool Audio::compressed() const {
  return false;
}

void Audio::uncompress() {
  // PLACEHOLDER: no-op until handheld audio decode pipeline is implemented.
}

void Audio::seekTime(double) {}

void Audio::seekSample(uint64_t) {}

double Audio::currentTime() const {
  return 0.0;
}

uint64_t Audio::currentSample() const {
  return 0;
}

size_t Audio::readPartial(int16_t*, size_t) {
  return 0;
}

size_t Audio::read(int16_t*, size_t) {
  return 0;
}

size_t Audio::resample(unsigned, unsigned, int16_t*, size_t, double) {
  return 0;
}

String const& Audio::name() const {
  return m_name;
}

void Audio::setName(String name) {
  m_name = std::move(name);
}

}

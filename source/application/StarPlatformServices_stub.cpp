#include "StarPlatformServices_stub.hpp"
#include "StarLogging.hpp"

namespace Star {

namespace {

// PLACEHOLDER: Statistics bridge when no platform statistics backend exists.
class PlaceholderStatisticsService final : public StatisticsService {
public:
  bool initialized() const override {
    return false;
  }

  Maybe<String> error() const override {
    return String("Platform statistics backend is unavailable (placeholder)");
  }

  bool setStat(String const&, String const&, Json const&) override {
    return false;
  }

  Json getStat(String const&, String const&, Json def = {}) const override {
    return def;
  }

  bool reportEvent(String const&, Json const&) override {
    return false;
  }

  bool unlockAchievement(String const&) override {
    return false;
  }

  StringSet achievementsUnlocked() const override {
    return {};
  }

  void refresh() override {}
  void flush() override {}

  bool reset() override {
    return false;
  }
};

// PLACEHOLDER: UGC bridge when storefront/workshop integration is unavailable.
class PlaceholderUserGeneratedContentService final : public UserGeneratedContentService {
public:
  StringList subscribedContentIds() const override {
    return {};
  }

  Maybe<String> contentDownloadDirectory(String const&) const override {
    return {};
  }

  UserGeneratedContentService::UGCState triggerContentDownload() override {
    return UserGeneratedContentService::UGCState::NoDownload;
  }
};

// STUB: Desktop URL service for non-desktop targets where URL launching is unsupported.
class StubDesktopService final : public DesktopService {
public:
  bool supportsOpenUrl() const override {
    return false;
  }

  void openUrl(String const& url) override {
    Logger::warn("Desktop URL open requested on stub platform service: {}", url);
  }
};

}

StatisticsServicePtr makePlaceholderStatisticsService() {
  return make_shared<PlaceholderStatisticsService>();
}

UserGeneratedContentServicePtr makePlaceholderUserGeneratedContentService() {
  return make_shared<PlaceholderUserGeneratedContentService>();
}

DesktopServicePtr makePlaceholderDesktopService() {
  return make_shared<StubDesktopService>();
}

}

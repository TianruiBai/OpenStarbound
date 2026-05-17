#pragma once

namespace Star {

STAR_CLASS(DesktopService);

class DesktopService {
public:
  ~DesktopService() = default;

  virtual bool supportsOpenUrl() const {
    return true;
  }

  virtual void openUrl(String const& url) = 0;
};

}

// STUB/PLACEHOLDER: Nintendo 3DS phase1 application backend.
// Implements runMainApplication() with a minimal 3DS main loop, and provides
// a stub ApplicationController that satisfies the interface without desktop
// dependencies (no SDL3, no OpenGL).
// Phase 2/3 will replace the event loop and renderer integration here.

#include "StarMainApplication.hpp"
#include "StarApplication.hpp"
#include "StarApplicationController.hpp"
#include "StarRenderer_n3ds_stub.hpp"
#include "StarPlatformServices_stub.hpp"
#include "StarLogging.hpp"
#include "StarSignalHandler.hpp"

#include <cstring>

#ifdef STAR_PLATFORM_N3DS
#include <3ds.h>

namespace {
  inline void n3dsDebugMarker(char const* message) {
    if (!message)
      return;
    svcOutputDebugString(message, std::strlen(message));
  }
}
#endif

namespace Star {

// ---------------------------------------------------------------------------
// N3dsApplicationController
// ---------------------------------------------------------------------------

class N3dsApplicationController : public ApplicationController {
public:
  N3dsApplicationController() {
    m_statisticsService   = makePlaceholderStatisticsService();
    m_ugcService          = makePlaceholderUserGeneratedContentService();
    m_desktopService      = makePlaceholderDesktopService();
  }

  // Timing controls — stored but not yet wired to a real scheduler.
  void setTargetUpdateRate(float r) override { m_targetUpdateRate = r; }
  void setTargetRenderRate(Maybe<float> r) override { m_targetRenderRate = r; }
  void setUpdateTrackWindow(float w) override { m_updateTrackWindow = w; }
  void setMaxFrameSkip(unsigned n) override { m_maxFrameSkip = n; }

  // Window management — N3DS has fixed screens; accept calls but do nothing.
  void setApplicationTitle(String) override {}
  void setFullscreenWindow(Vec2U) override {}
  void setNormalWindow(Vec2U) override {}
  void setMaximizedWindow() override {}
  void setBorderlessWindow() override {}
  void setVSyncEnabled(bool) override {}
  void setCursorVisible(bool) override {}
  void setCursorPosition(Vec2I) override {}
  void setCursorHardware(bool) override {}
  bool setCursorImage(const String&, const ImageConstPtr&, unsigned, const Vec2I&) override { return false; }
  void setAcceptingTextInput(bool) override {}
  void setTextArea(Maybe<pair<RectI, int>>) override {}

  // Audio — STUB: not wired to N3DS DSP yet.
  AudioFormat enableAudio() override { return AudioFormat{44100, 2}; } // STUB
  void disableAudio() override {}                                       // STUB
  bool openAudioInputDevice(uint32_t, int, int, AudioCallback) override { return false; } // STUB
  bool closeAudioInputDevice() override { return false; }               // STUB
  bool supportsAudioInput() const override { return false; }            // STUB

  // Clipboard — not available on 3DS.
  bool hasClipboard() override { return false; }
  bool setClipboard(String) override { return false; }
  bool setClipboardData(StringMap<ByteArray>) override { return false; }
  bool setClipboardImage(Image const&, ByteArray*, String const*) override { return false; }
  bool setClipboardFile(String const&) override { return false; }
  Maybe<String> getClipboard() override { return {}; }

  bool isFocused() const override { return true; } // 3DS is always "focused"

  // Rate telemetry — PLACEHOLDER: return target rates as measured rates.
  float updateRate() const override { return m_targetUpdateRate; }      // PLACEHOLDER
  float renderFps() const override { return m_targetRenderRate.value(60.0f); } // PLACEHOLDER
  float getDisplayScale() const override { return 1.0f; }

  // Platform services — use existing stub implementations.
  StatisticsServicePtr statisticsService() const override { return m_statisticsService; }
  P2PNetworkingServicePtr p2pNetworkingService() const override { return nullptr; }
  UserGeneratedContentServicePtr userGeneratedContentService() const override { return m_ugcService; }
  DesktopServicePtr desktopService() const override { return m_desktopService; }

  void quit() override {
    m_quit = true;
  }
  bool shouldQuit() const { return m_quit; }

private:
  float m_targetUpdateRate = 60.0f;
  Maybe<float> m_targetRenderRate;
  float m_updateTrackWindow = 1.0f;
  unsigned m_maxFrameSkip = 5;
  bool m_quit = false;

  StatisticsServicePtr m_statisticsService;
  UserGeneratedContentServicePtr m_ugcService;
  DesktopServicePtr m_desktopService;
};

// ---------------------------------------------------------------------------
// runMainApplication (N3DS implementation)
// PLACEHOLDER: runs a fixed-timestep loop using 3DS aptMainLoop.
// Input, audio, and rendering will be progressively wired in Phase 2/3.
// ---------------------------------------------------------------------------

int runMainApplication(ApplicationUPtr application, StringList cmdLineArgs) {
  SignalHandler signalHandler;

  try {
#ifdef STAR_PLATFORM_N3DS
    n3dsDebugMarker("OSBN3DS: before gfx/hid/romfs init\n");
    gfxInit(GSP_BGR8_OES, GSP_BGR8_OES, false);
    hidInit();
    auto romfsResult = romfsInit();
    if (R_FAILED(romfsResult))
      n3dsDebugMarker("OSBN3DS: romfsInit failed\n");
    else
      n3dsDebugMarker("OSBN3DS: romfsInit ok\n");
#endif

    auto appController = make_shared<N3dsApplicationController>();
    auto renderer      = make_shared<N3dsStubRenderer>();
    StringList startupArgs = {"-bootconfig", "romfs:/sbinit.config"};
    if (cmdLineArgs.size() > 1)
      startupArgs.appendAll(cmdLineArgs.slice(1));

    Logger::info("N3dsMainApplication: startup"); // PLACEHOLDER

    n3dsDebugMarker("OSBN3DS: before startup\n");
    application->startup(startupArgs);
    n3dsDebugMarker("OSBN3DS: after startup\n");
    // Temporary runtime smoke probe: advance lifecycle one stage at a time.
    n3dsDebugMarker("OSBN3DS: before applicationInit\n");
    application->applicationInit(appController);
    n3dsDebugMarker("OSBN3DS: after applicationInit\n");
    n3dsDebugMarker("OSBN3DS: before renderInit\n");
    application->renderInit(renderer);
    n3dsDebugMarker("OSBN3DS: after renderInit\n");

#ifdef STAR_PLATFORM_N3DS
    // PLACEHOLDER: bare 3DS main loop. Some Citra builds can report false from
    // aptMainLoop for homebrew CXI launch paths, so keep running until explicit
    // quit while still polling APT each frame.
    unsigned frameCounter = 0;
    while (!appController->shouldQuit()) {
      hidScanInput();
      (void)aptMainLoop();

      application->update();
      application->render();
      renderer->flush(Mat3F::identity());
      gspWaitForVBlank(); // PLACEHOLDER: trivial frame pace
      ++frameCounter;
    }
#else
    // Fallback for non-N3DS builds of this translation unit (should not happen).
    application->update();
    application->render();
#endif

    // application->shutdown();
    Logger::info("N3dsMainApplication: shutdown"); // PLACEHOLDER

#ifdef STAR_PLATFORM_N3DS
  hidExit();
    romfsExit();
    gfxExit();
#endif

    return 0;
  } catch (std::exception const& e) {
    Logger::error("N3dsMainApplication: unhandled exception: {}", e.what());
#ifdef STAR_PLATFORM_N3DS
  hidExit();
    romfsExit();
    gfxExit();
#endif
    return 1;
  }
}

} // namespace Star

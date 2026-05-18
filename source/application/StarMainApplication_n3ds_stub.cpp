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
#include "StarTime.hpp"

#include <algorithm>
#include <cmath>

#ifdef STAR_PLATFORM_N3DS
#include <3ds.h>

namespace {

constexpr u32 N3dsCirclePadUpBit = 1u << 28;
constexpr u32 N3dsCirclePadDownBit = 1u << 29;
constexpr u32 N3dsCirclePadLeftBit = 1u << 30;
constexpr u32 N3dsCirclePadRightBit = 1u << 31;

struct N3dsInputState {
  u32 previousCirclePadMask = 0;
  bool touchPressed = false;
  Star::Vec2F lastTouchPosition = {0.0f, 0.0f};
};

void writeN3dsFramebufferProbe(gfxScreen_t screen, gfx3dSide_t side, unsigned frameCounter) {
  u16 framebufferWidth = 0;
  u16 framebufferHeight = 0;
  u8* framebuffer = gfxGetFramebuffer(screen, side, &framebufferWidth, &framebufferHeight);
  if (!framebuffer || framebufferWidth == 0 || framebufferHeight == 0)
    return;

  bool flashPhase = ((frameCounter / 30) % 2) == 0;
  for (u32 row = 0; row < framebufferHeight; ++row) {
    for (u32 column = 0; column < framebufferWidth; ++column) {
      bool border = column < 10 || row < 10 || column + 10 >= framebufferWidth || row + 10 >= framebufferHeight;
      bool checker = (((column / 24) + (row / 24) + (flashPhase ? 1u : 0u)) % 2u) == 0;

      u8 red = 0;
      u8 green = 0;
      u8 blue = 0;

      if (border) {
        red = 255;
        green = 255;
        blue = 255;
      } else if (screen == GFX_TOP) {
        red = checker ? 255 : 24;
        green = checker ? 24 : 220;
        blue = checker ? 180 : 255;
      } else {
        red = checker ? 255 : 20;
        green = checker ? 220 : 255;
        blue = checker ? 32 : 32;
      }

      size_t pixelOffset = (static_cast<size_t>(row) * framebufferWidth + column) * 3;
      framebuffer[pixelOffset + 0] = blue;
      framebuffer[pixelOffset + 1] = green;
      framebuffer[pixelOffset + 2] = red;
    }
  }
}

void runN3dsFramebufferVisibilityProbe() {
  // PLACEHOLDER: direct software framebuffer probe for Citra/device bring-up.
  // This intentionally bypasses citro2d so blank-screen reports can be split
  // between applet/framebuffer setup and GPU render-target setup.
  constexpr unsigned ProbeFrames = 180;
  Star::Logger::info("N3dsMainApplication: direct framebuffer visibility probe");

  writeN3dsFramebufferProbe(GFX_TOP, GFX_LEFT, 0);
  writeN3dsFramebufferProbe(GFX_BOTTOM, GFX_LEFT, 15);
  gfxFlushBuffers();
  gfxSwapBuffers();
  gspWaitForVBlank();

  for (unsigned frameCounter = 0; frameCounter < ProbeFrames; ++frameCounter) {
    hidScanInput();
    (void)aptMainLoop();
    if (hidKeysDown() & KEY_START)
      break;

    writeN3dsFramebufferProbe(GFX_TOP, GFX_LEFT, frameCounter);
    writeN3dsFramebufferProbe(GFX_BOTTOM, GFX_LEFT, frameCounter + 15);
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
  }

}

void runN3dsCitroVisibilityProbe(Star::N3dsStubRenderer& renderer) {
  // PLACEHOLDER: citro-backed visibility probe for the active GPU renderer.
  // If the direct framebuffer probe is visible but this is not, keep the next
  // debugging pass inside the citro render-target/presentation path.
  constexpr unsigned ProbeFrames = 300;
  Star::Logger::info("N3dsMainApplication: citro visibility probe");

  for (unsigned frameCounter = 0; frameCounter < ProbeFrames; ++frameCounter) {
    hidScanInput();
    (void)aptMainLoop();
    if (hidKeysDown() & KEY_START)
      break;

    renderer.flush(Star::Mat3F::identity());
    gspWaitForVBlank();
  }

}

Star::KeyMod n3dsKeyMods(u32 held) {
  Star::KeyMod mods = Star::KeyMod::NoMod;
  if (held & KEY_L)
    mods |= Star::KeyMod::LShift;
  if (held & KEY_R)
    mods |= Star::KeyMod::RShift;
  return mods;
}

void appendMappedKeyEvents(Star::List<Star::InputEvent>& outEvents, u32 down, u32 up, Star::KeyMod mods) {
  struct Mapping {
    u32 button;
    Star::Key key;
  };

  static Mapping const mappings[] = {
      {KEY_DUP, Star::Key::Up},
      {KEY_DDOWN, Star::Key::Down},
      {KEY_DLEFT, Star::Key::Left},
      {KEY_DRIGHT, Star::Key::Right},
      {KEY_START, Star::Key::Escape},
      {KEY_SELECT, Star::Key::Tab},
      {KEY_A, Star::Key::Return},
      {KEY_B, Star::Key::Escape},
      {KEY_X, Star::Key::Space},
      {KEY_Y, Star::Key::E},
      {N3dsCirclePadUpBit, Star::Key::W},
      {N3dsCirclePadDownBit, Star::Key::S},
      {N3dsCirclePadLeftBit, Star::Key::A},
      {N3dsCirclePadRightBit, Star::Key::D},
  };

  for (auto const& mapping : mappings) {
    if (down & mapping.button)
      outEvents.append(Star::KeyDownEvent{mapping.key, mods});
    if (up & mapping.button)
      outEvents.append(Star::KeyUpEvent{mapping.key});
  }
}

Star::List<Star::InputEvent> n3dsProcessInputEvents(N3dsInputState& state) {
  Star::List<Star::InputEvent> events;

  u32 held = hidKeysHeld();
  u32 down = hidKeysDown();
  u32 up = hidKeysUp();

  circlePosition circle;
  hidCircleRead(&circle);
  u32 circleMask = 0;
  constexpr int CircleDeadzone = 48;
  if (circle.dx <= -CircleDeadzone)
    circleMask |= N3dsCirclePadLeftBit;
  else if (circle.dx >= CircleDeadzone)
    circleMask |= N3dsCirclePadRightBit;
  if (circle.dy <= -CircleDeadzone)
    circleMask |= N3dsCirclePadDownBit;
  else if (circle.dy >= CircleDeadzone)
    circleMask |= N3dsCirclePadUpBit;

  u32 circleDown = circleMask & ~state.previousCirclePadMask;
  u32 circleUp = state.previousCirclePadMask & ~circleMask;
  state.previousCirclePadMask = circleMask;

  down |= circleDown;
  up |= circleUp;

  appendMappedKeyEvents(events, down, up, n3dsKeyMods(held));

  touchPosition touch;
  hidTouchRead(&touch);
  bool touchNow = (held & KEY_TOUCH) != 0;
  Star::Vec2F touchPos = {(float)touch.px, 239.0f - (float)touch.py};

  if (touchNow) {
    events.append(Star::MouseMoveEvent{{0.0f, 0.0f}, touchPos});
    if (!state.touchPressed)
      events.append(Star::MouseButtonDownEvent{Star::MouseButton::Left, touchPos});
    state.touchPressed = true;
    state.lastTouchPosition = touchPos;
  } else if (state.touchPressed) {
    events.append(Star::MouseButtonUpEvent{Star::MouseButton::Left, state.lastTouchPosition});
    state.touchPressed = false;
  }

  return events;
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

  // Timing controls: stored and consumed by the N3DS scheduler.
  void setTargetUpdateRate(float r) override { m_targetUpdateRate = r; }
  void setTargetRenderRate(Maybe<float> r) override { m_targetRenderRate = r; }
  void setUpdateTrackWindow(float w) override { m_updateTrackWindow = w; }
  void setMaxFrameSkip(unsigned n) override { m_maxFrameSkip = n; }

  // Window management: N3DS has fixed screens; accept calls but do nothing.
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

  // Audio: STUB, not wired to N3DS DSP yet.
  AudioFormat enableAudio() override { return AudioFormat{44100, 2}; } // STUB
  void disableAudio() override {}                                       // STUB
  bool openAudioInputDevice(uint32_t, int, int, AudioCallback) override { return false; } // STUB
  bool closeAudioInputDevice() override { return false; }               // STUB
  bool supportsAudioInput() const override { return false; }            // STUB

  // Clipboard is not available on 3DS.
  bool hasClipboard() override { return false; }
  bool setClipboard(String) override { return false; }
  bool setClipboardData(StringMap<ByteArray>) override { return false; }
  bool setClipboardImage(Image const&, ByteArray*, String const*) override { return false; }
  bool setClipboardFile(String const&) override { return false; }
  Maybe<String> getClipboard() override { return {}; }

  bool isFocused() const override { return true; } // 3DS is always "focused"

  // Rate telemetry: PLACEHOLDER, return target rates as measured rates.
  float updateRate() const override { return m_targetUpdateRate; }      // PLACEHOLDER
  float renderFps() const override { return m_targetRenderRate.value(60.0f); } // PLACEHOLDER
  float getDisplayScale() const override { return 1.0f; }

  // Platform services use existing stub implementations.
  StatisticsServicePtr statisticsService() const override { return m_statisticsService; }
  P2PNetworkingServicePtr p2pNetworkingService() const override { return nullptr; }
  UserGeneratedContentServicePtr userGeneratedContentService() const override { return m_ugcService; }
  DesktopServicePtr desktopService() const override { return m_desktopService; }

  void quit() override {
    m_quit = true;
  }
  bool shouldQuit() const { return m_quit; }

  float targetUpdateRate() const {
    return m_targetUpdateRate;
  }

  Maybe<float> targetRenderRate() const {
    return m_targetRenderRate;
  }

  unsigned maxFrameSkip() const {
    return m_maxFrameSkip;
  }

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
    gfxInit(GSP_BGR8_OES, GSP_BGR8_OES, false);
    gfxSet3D(false);
    gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);
    gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
    gfxSetDoubleBuffering(GFX_TOP, true);
    gfxSetDoubleBuffering(GFX_BOTTOM, true);
    hidInit();
    romfsInit();
    runN3dsFramebufferVisibilityProbe();
#endif

    auto appController = make_shared<N3dsApplicationController>();
    auto renderer      = make_shared<N3dsStubRenderer>();
#ifdef STAR_PLATFORM_N3DS
    runN3dsCitroVisibilityProbe(*renderer);
#endif
    StringList startupArgs = {"-bootconfig", "romfs:/sbinit.config"};
    if (cmdLineArgs.size() > 1)
      startupArgs.appendAll(cmdLineArgs.slice(1));

    Logger::info("N3dsMainApplication: startup"); // PLACEHOLDER

    application->startup(startupArgs);
    application->applicationInit(appController);
    application->renderInit(renderer);

#ifdef STAR_PLATFORM_N3DS
    // Fixed-timestep loop with bounded catch-up. Some Citra builds can report
    // false from aptMainLoop for homebrew CXI launch paths, so keep running
    // until explicit quit while still polling APT each frame.
    constexpr double MaxFrameDelta = 0.25;
    double const updateStep = 1.0 / std::max(1.0f, appController->targetUpdateRate());
    Maybe<float> targetRenderRate = appController->targetRenderRate();
    double const renderStep = targetRenderRate ? (1.0 / std::max(1.0f, *targetRenderRate)) : 0.0;
    double updateAccumulator = 0.0;
    double renderAccumulator = 0.0;
    int64_t lastTickMs = Time::monotonicMilliseconds();
    N3dsInputState inputState;

    while (!appController->shouldQuit()) {
      hidScanInput();
      (void)aptMainLoop();

      for (auto const& event : n3dsProcessInputEvents(inputState))
        application->processInput(event);

      int64_t nowTickMs = Time::monotonicMilliseconds();
      double frameDelta = static_cast<double>(nowTickMs - lastTickMs) / 1000.0;
      lastTickMs = nowTickMs;
      frameDelta = std::clamp(frameDelta, 0.0, MaxFrameDelta);

      updateAccumulator += frameDelta;
      renderAccumulator += frameDelta;

      unsigned updates = 0;
      unsigned maxUpdates = std::max(1u, appController->maxFrameSkip() + 1);
      while (updateAccumulator >= updateStep && updates < maxUpdates) {
        application->update();
        updateAccumulator -= updateStep;
        ++updates;
      }

      if (updateAccumulator >= updateStep)
        updateAccumulator = std::fmod(updateAccumulator, updateStep);

      if (!targetRenderRate || renderAccumulator >= renderStep) {
        application->render();
        renderer->flush(Mat3F::identity());
        if (targetRenderRate)
          renderAccumulator = std::fmod(renderAccumulator, renderStep);
      }

      // Citro presents GPU frames during C3D_FrameEnd; swapping the software
      // buffers again here can immediately rotate the displayed buffer away.
      gspWaitForVBlank();
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

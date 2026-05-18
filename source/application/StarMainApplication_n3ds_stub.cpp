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
#include "StarFile.hpp"
#include "StarLogging.hpp"
#include "StarSignalHandler.hpp"
#include "StarTime.hpp"
#include "StarImage.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#ifdef STAR_PLATFORM_N3DS
#include <3ds.h>

namespace {

constexpr u32 N3dsCirclePadMask = KEY_CPAD_LEFT | KEY_CPAD_RIGHT | KEY_CPAD_UP | KEY_CPAD_DOWN;
constexpr int N3dsCircleDeadzone = 48;
constexpr float N3dsCStickCursorStep = 5.0f;
constexpr float N3dsPointerMaxX = 399.0f;
constexpr float N3dsPointerMaxY = 239.0f;
constexpr bool N3dsRunStartupGraphicsProbesByDefault = false;
constexpr float N3dsBottomHotbarX = 11.0f;
constexpr float N3dsBottomHotbarY = 206.0f;
constexpr float N3dsBottomHotbarSlotSize = 28.0f;
constexpr float N3dsBottomHotbarSlotGap = 2.0f;
constexpr unsigned N3dsBottomHotbarSlotCount = 10;
constexpr bool N3dsExplicitSdmcMount = false;

enum N3dsStartupDiagnostic : u32 {
  N3dsDiagGfxReady = 1u << 0,
  N3dsDiagSdmcMounted = 1u << 1,
  N3dsDiagNativeDirectory = 1u << 2,
  N3dsDiagNativeLogWrite = 1u << 3,
  N3dsDiagFileLogSink = 1u << 4,
  N3dsDiagPreStartupFlush = 1u << 5,
  N3dsDiagStartupEntered = 1u << 6,
  N3dsDiagStartupComplete = 1u << 7,
};

u32 sN3dsStartupDiagnostics = 0;

struct N3dsInputState {
  u32 previousCirclePadMask = 0;
  bool touchPressed = false;
  bool touchUiPressed = false;
  bool cStickPointerPressed = false;
  Star::Vec2F lastTouchPosition = {0.0f, 0.0f};
  Star::Vec2F pointerPosition = {200.0f, 120.0f};
  Star::N3dsHandheldOverlayState handheldOverlay;
};

void markN3dsStartupDiagnostic(N3dsStartupDiagnostic diagnostic) {
  sN3dsStartupDiagnostics |= diagnostic;
}

void appendN3dsNativeDiagnosticLog(char const* message) {
  std::FILE* file = std::fopen("sdmc:/OpenStarbound/starbound_n3ds_native.log", "ab");
  if (!file)
    return;

  std::fputs(message, file);
  std::fputc('\n', file);
  std::fflush(file);
  std::fclose(file);
  markN3dsStartupDiagnostic(N3dsDiagNativeLogWrite);
}

void prepareN3dsNativeDiagnosticLog() {
  if (::mkdir("sdmc:/OpenStarbound", 0777) == 0 || errno == EEXIST)
    markN3dsStartupDiagnostic(N3dsDiagNativeDirectory);

  appendN3dsNativeDiagnosticLog("native diagnostic log active");
}

bool installN3dsFileLogger() {
  try {
    Star::File::makeDirectory("sdmc:/OpenStarbound");
    Star::Logger::addSink(Star::make_shared<Star::FileLogSink>("sdmc:/OpenStarbound/starbound_n3ds.log", Star::LogLevel::Info, true));
    markN3dsStartupDiagnostic(N3dsDiagFileLogSink);
    Star::Logger::info("N3dsMainApplication: SDMC file logger active");
    appendN3dsNativeDiagnosticLog("engine file logger active");
    return true;
  } catch (std::exception const& e) {
    Star::Logger::warn("N3dsMainApplication: SDMC file logger unavailable: {}", e.what());
    appendN3dsNativeDiagnosticLog("engine file logger unavailable");
    return false;
  }
}

bool n3dsRunStartupGraphicsProbes(Star::StringList const& cmdLineArgs) {
  if (N3dsRunStartupGraphicsProbesByDefault)
    return true;

  for (auto const& arg : cmdLineArgs) {
    if (arg == "--n3ds-graphics-probes" || arg == "-n3dsGraphicsProbes")
      return true;
  }

  return false;
}

Star::Maybe<unsigned> n3dsBottomHotbarSlotAt(float x, float yTopOrigin) {
  if (yTopOrigin < N3dsBottomHotbarY || yTopOrigin >= N3dsBottomHotbarY + N3dsBottomHotbarSlotSize)
    return {};

  float relativeX = x - N3dsBottomHotbarX;
  if (relativeX < 0.0f)
    return {};

  float slotPitch = N3dsBottomHotbarSlotSize + N3dsBottomHotbarSlotGap;
  unsigned slot = static_cast<unsigned>(relativeX / slotPitch);
  float slotStart = slot * slotPitch;
  if (slot >= N3dsBottomHotbarSlotCount || relativeX < slotStart || relativeX >= slotStart + N3dsBottomHotbarSlotSize)
    return {};

  return slot;
}

Star::Key n3dsHotbarKeyForSlot(unsigned slot) {
  static constexpr Star::Key HotbarKeys[N3dsBottomHotbarSlotCount] = {
      Star::Key::One,
      Star::Key::Two,
      Star::Key::Three,
      Star::Key::Four,
      Star::Key::Five,
      Star::Key::Six,
      Star::Key::R,
      Star::Key::T,
      Star::Key::Y,
      Star::Key::N,
  };

  return HotbarKeys[std::min(slot, N3dsBottomHotbarSlotCount - 1)];
}

void appendKeyTapEvents(Star::List<Star::InputEvent>& outEvents, Star::Key key) {
  outEvents.append(Star::KeyDownEvent{key, Star::KeyMod::NoMod});
  outEvents.append(Star::KeyUpEvent{key});
}

void appendHotbarSelectEvents(Star::List<Star::InputEvent>& outEvents, N3dsInputState& state, unsigned slot) {
  state.handheldOverlay.selectedHotbarSlot = std::min(slot, N3dsBottomHotbarSlotCount - 1);
  appendKeyTapEvents(outEvents, n3dsHotbarKeyForSlot(state.handheldOverlay.selectedHotbarSlot));
}

Star::Maybe<Star::Key> n3dsBottomQuickActionAt(float x, float yTopOrigin) {
  struct TouchButton {
    float centerX;
    float centerY;
    float radius;
    Star::Key key;
  };

  static constexpr TouchButton TouchButtons[] = {
      {292.0f, 144.0f, 16.0f, Star::Key::Return},
      {260.0f, 174.0f, 16.0f, Star::Key::Escape},
      {260.0f, 114.0f, 16.0f, Star::Key::Space},
      {228.0f, 144.0f, 16.0f, Star::Key::E},
  };

  for (auto const& touchButton : TouchButtons) {
    float deltaX = x - touchButton.centerX;
    float deltaY = yTopOrigin - touchButton.centerY;
    if (deltaX * deltaX + deltaY * deltaY <= touchButton.radius * touchButton.radius)
      return touchButton.key;
  }

  return {};
}

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

  // PLACEHOLDER: use a generated split-color texture so direct-open Citra
  // captures can verify rectangular UV sampling on the current textured-quad
  // path before full UI/world atlas content is trustworthy on handheld.
  Star::Image probeTextureImage(Star::Vec2U(64, 64), Star::PixelFormat::RGBA32);
  probeTextureImage.fillRect(Star::Vec2U(0, 0), Star::Vec2U(32, 64), Star::Vec4B(80, 112, 255, 255));
  probeTextureImage.fillRect(Star::Vec2U(32, 0), Star::Vec2U(32, 64), Star::Vec4B(255, 224, 48, 255));
  auto probeTexture = renderer.createTexture(probeTextureImage, Star::TextureAddressing::Clamp, Star::TextureFiltering::Nearest);

  for (unsigned frameCounter = 0; frameCounter < ProbeFrames; ++frameCounter) {
    hidScanInput();
    (void)aptMainLoop();
    if (hidKeysDown() & KEY_START)
      break;

    // PLACEHOLDER: inject a tiny deterministic replay batch so Citra captures
    // can validate primitive/scissor replay before full client UI rendering is
    // reliably contributing visible geometry on the handheld path.
    renderer.immediatePrimitives().emplace_back(
        std::in_place_type_t<Star::RenderTriangle>(),
        Star::Vec2F(56.0f, 172.0f),
        Star::Vec2F(112.0f, 212.0f),
        Star::Vec2F(144.0f, 148.0f),
        Star::Vec4B(255, 96, 96, 255),
        0.0f);
    renderer.setScissorRect(Star::RectI::withSize(Star::Vec2I(196, 118), Star::Vec2I(72, 64)));
    renderer.immediatePrimitives().emplace_back(
        std::in_place_type_t<Star::RenderQuad>(),
        probeTexture,
        Star::Vec2F(180.0f, 112.0f), Star::Vec2F(32.0f, 0.0f),
        Star::Vec2F(300.0f, 112.0f), Star::Vec2F(64.0f, 0.0f),
        Star::Vec2F(300.0f, 180.0f), Star::Vec2F(64.0f, 64.0f),
        Star::Vec2F(180.0f, 180.0f), Star::Vec2F(32.0f, 64.0f),
        Star::Vec4B::filled(255),
        0.0f);
    renderer.setScissorRect({});

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
      {KEY_ZL, Star::Key::PageUp},
      {KEY_ZR, Star::Key::PageDown},
      {KEY_CPAD_UP, Star::Key::W},
      {KEY_CPAD_DOWN, Star::Key::S},
      {KEY_CPAD_LEFT, Star::Key::A},
      {KEY_CPAD_RIGHT, Star::Key::D},
  };

  for (auto const& mapping : mappings) {
    if (down & mapping.button)
      outEvents.append(Star::KeyDownEvent{mapping.key, mods});
    if (up & mapping.button)
      outEvents.append(Star::KeyUpEvent{mapping.key});
  }
}

void appendMappedControllerButtonEvents(Star::List<Star::InputEvent>& outEvents, u32 down, u32 up) {
  struct Mapping {
    u32 button;
    Star::ControllerButton controllerButton;
  };

  static Mapping const mappings[] = {
      {KEY_A, Star::ControllerButton::A},
      {KEY_B, Star::ControllerButton::B},
      {KEY_X, Star::ControllerButton::X},
      {KEY_Y, Star::ControllerButton::Y},
      {KEY_SELECT, Star::ControllerButton::Back},
      {KEY_START, Star::ControllerButton::Start},
      {KEY_L, Star::ControllerButton::LeftShoulder},
      {KEY_R, Star::ControllerButton::RightShoulder},
      {KEY_DUP, Star::ControllerButton::DPadUp},
      {KEY_DDOWN, Star::ControllerButton::DPadDown},
      {KEY_DLEFT, Star::ControllerButton::DPadLeft},
      {KEY_DRIGHT, Star::ControllerButton::DPadRight},
      {KEY_ZL, Star::ControllerButton::Paddle1},
      {KEY_ZR, Star::ControllerButton::Paddle2},
  };

  constexpr Star::ControllerId N3dsControllerId = 0;
  for (auto const& mapping : mappings) {
    if (down & mapping.button)
      outEvents.append(Star::ControllerButtonDownEvent{N3dsControllerId, mapping.controllerButton});
    if (up & mapping.button)
      outEvents.append(Star::ControllerButtonUpEvent{N3dsControllerId, mapping.controllerButton});
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
  if (circle.dx <= -N3dsCircleDeadzone)
    circleMask |= KEY_CPAD_LEFT;
  else if (circle.dx >= N3dsCircleDeadzone)
    circleMask |= KEY_CPAD_RIGHT;
  if (circle.dy <= -N3dsCircleDeadzone)
    circleMask |= KEY_CPAD_DOWN;
  else if (circle.dy >= N3dsCircleDeadzone)
    circleMask |= KEY_CPAD_UP;

  u32 circleDown = circleMask & ~state.previousCirclePadMask;
  u32 circleUp = state.previousCirclePadMask & ~circleMask;
  state.previousCirclePadMask = circleMask;

  down &= ~N3dsCirclePadMask;
  up &= ~N3dsCirclePadMask;
  down |= circleDown;
  up |= circleUp;

  if (down & KEY_DRIGHT)
    appendHotbarSelectEvents(events, state, (state.handheldOverlay.selectedHotbarSlot + 1) % N3dsBottomHotbarSlotCount);
  if (down & KEY_DLEFT)
    appendHotbarSelectEvents(events, state, (state.handheldOverlay.selectedHotbarSlot + N3dsBottomHotbarSlotCount - 1) % N3dsBottomHotbarSlotCount);

  appendMappedKeyEvents(events, down, up, n3dsKeyMods(held));
  appendMappedControllerButtonEvents(events, down, up);

  Star::Vec2F pointerMove = {0.0f, 0.0f};
  if (held & KEY_CSTICK_LEFT)
    pointerMove[0] -= N3dsCStickCursorStep;
  if (held & KEY_CSTICK_RIGHT)
    pointerMove[0] += N3dsCStickCursorStep;
  if (held & KEY_CSTICK_UP)
    pointerMove[1] += N3dsCStickCursorStep;
  if (held & KEY_CSTICK_DOWN)
    pointerMove[1] -= N3dsCStickCursorStep;

  if (pointerMove[0] != 0.0f || pointerMove[1] != 0.0f) {
    Star::Vec2F previousPointerPosition = state.pointerPosition;
    state.pointerPosition[0] = std::clamp(state.pointerPosition[0] + pointerMove[0], 0.0f, N3dsPointerMaxX);
    state.pointerPosition[1] = std::clamp(state.pointerPosition[1] + pointerMove[1], 0.0f, N3dsPointerMaxY);
    events.append(Star::MouseMoveEvent{state.pointerPosition - previousPointerPosition, state.pointerPosition});
  }

  state.handheldOverlay.pointerPosition = state.pointerPosition;
  state.handheldOverlay.circlePadActive = circleMask != 0;
  state.handheldOverlay.dpadActive = (held & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT)) != 0;
  state.handheldOverlay.buttonA = (held & KEY_A) != 0;
  state.handheldOverlay.buttonB = (held & KEY_B) != 0;
  state.handheldOverlay.buttonX = (held & KEY_X) != 0;
  state.handheldOverlay.buttonY = (held & KEY_Y) != 0;
  state.handheldOverlay.shoulderL = (held & KEY_L) != 0;
  state.handheldOverlay.shoulderR = (held & KEY_R) != 0;
  state.handheldOverlay.shoulderZL = (held & KEY_ZL) != 0;
  state.handheldOverlay.shoulderZR = (held & KEY_ZR) != 0;

  if ((down & KEY_ZR) && !state.cStickPointerPressed) {
    events.append(Star::MouseButtonDownEvent{Star::MouseButton::Left, state.pointerPosition});
    state.cStickPointerPressed = true;
  } else if ((up & KEY_ZR) && state.cStickPointerPressed) {
    events.append(Star::MouseButtonUpEvent{Star::MouseButton::Left, state.pointerPosition});
    state.cStickPointerPressed = false;
  }
  state.handheldOverlay.pointerPressed = state.cStickPointerPressed;

  touchPosition touch;
  hidTouchRead(&touch);
  bool touchNow = (held & KEY_TOUCH) != 0;
  Star::Vec2F touchPos = {(float)touch.px, 239.0f - (float)touch.py};
  state.handheldOverlay.touchPosition = touchPos;
  state.handheldOverlay.touchPressed = touchNow;

  bool touchHandledByBottomUi = state.touchUiPressed;
  if (touchNow && !state.touchPressed && !state.touchUiPressed) {
    if (auto slot = n3dsBottomHotbarSlotAt((float)touch.px, (float)touch.py)) {
      appendHotbarSelectEvents(events, state, *slot);
      touchHandledByBottomUi = true;
      state.touchUiPressed = true;
    } else if (auto quickAction = n3dsBottomQuickActionAt((float)touch.px, (float)touch.py)) {
      appendKeyTapEvents(events, *quickAction);
      touchHandledByBottomUi = true;
      state.touchUiPressed = true;
    }
  }

  if (touchHandledByBottomUi) {
    if (!touchNow)
      state.touchUiPressed = false;
    state.touchPressed = false;
  } else if (touchNow) {
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
#ifdef STAR_PLATFORM_N3DS
  bool n3dsSdmcMounted = false;
#endif

  try {
#ifdef STAR_PLATFORM_N3DS
    bool runStartupGraphicsProbes = n3dsRunStartupGraphicsProbes(cmdLineArgs);
    gfxInit(GSP_BGR8_OES, GSP_BGR8_OES, false);
    gfxSet3D(false);
    gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);
    gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
    gfxSetDoubleBuffering(GFX_TOP, true);
    gfxSetDoubleBuffering(GFX_BOTTOM, true);
    hidInit();
    romfsInit();
    markN3dsStartupDiagnostic(N3dsDiagGfxReady);
    if constexpr (N3dsExplicitSdmcMount) {
      Result sdmcResult = archiveMountSdmc();
      if (R_SUCCEEDED(sdmcResult)) {
        n3dsSdmcMounted = true;
        markN3dsStartupDiagnostic(N3dsDiagSdmcMounted);
      } else {
        Logger::warn("N3dsMainApplication: SDMC mount failed: {}", (unsigned)sdmcResult);
      }
    } else {
      markN3dsStartupDiagnostic(N3dsDiagSdmcMounted);
    }
    prepareN3dsNativeDiagnosticLog();
    installN3dsFileLogger();
    if (runStartupGraphicsProbes)
      runN3dsFramebufferVisibilityProbe();
#endif

    auto appController = make_shared<N3dsApplicationController>();
    auto renderer      = make_shared<N3dsStubRenderer>();
#ifdef STAR_PLATFORM_N3DS
    if (runStartupGraphicsProbes)
      runN3dsCitroVisibilityProbe(*renderer);
    else {
      markN3dsStartupDiagnostic(N3dsDiagPreStartupFlush);
      N3dsHandheldOverlayState startupOverlay;
      startupOverlay.startupDiagnostics = sN3dsStartupDiagnostics;
      renderer->setHandheldOverlayState(startupOverlay);
      renderer->flush(Mat3F::identity());
    }
#endif
    StringList startupArgs = {"-bootconfig", "romfs:/sbinit.config"};
    if (cmdLineArgs.size() > 1)
      startupArgs.appendAll(cmdLineArgs.slice(1));

    Logger::info("N3dsMainApplication: startup"); // PLACEHOLDER
  #ifdef STAR_PLATFORM_N3DS
    markN3dsStartupDiagnostic(N3dsDiagStartupEntered);
    appendN3dsNativeDiagnosticLog("application startup enter");
  #endif

    application->startup(startupArgs);
  #ifdef STAR_PLATFORM_N3DS
    markN3dsStartupDiagnostic(N3dsDiagStartupComplete);
    appendN3dsNativeDiagnosticLog("application startup complete");
  #endif
    Logger::info("N3dsMainApplication: application startup complete"); // PLACEHOLDER
    application->applicationInit(appController);
    Logger::info("N3dsMainApplication: application init complete"); // PLACEHOLDER
    application->renderInit(renderer);
    Logger::info("N3dsMainApplication: render init complete"); // PLACEHOLDER

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
      renderer->setHandheldOverlayState(inputState.handheldOverlay);

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
    if (n3dsSdmcMounted)
      archiveUnmount("sdmc");
    romfsExit();
    gfxExit();
#endif

    return 0;
  } catch (std::exception const& e) {
    Logger::error("N3dsMainApplication: unhandled exception: {}", e.what());
#ifdef STAR_PLATFORM_N3DS
    hidExit();
    if (n3dsSdmcMounted)
      archiveUnmount("sdmc");
    romfsExit();
    gfxExit();
#endif
    return 1;
  }
}

} // namespace Star

// STUB/PLACEHOLDER: Nintendo 3DS renderer backend with a minimal citro-backed
// frame path for Phase 3 bring-up. Most RenderPrimitive paths are still
// placeholders.

#include "StarRenderer_n3ds_stub.hpp"
#include "StarLogging.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef STAR_PLATFORM_N3DS
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#endif
#include <vector>


namespace Star {

namespace {
bool sN3dsTitleMenuInputActive = false;
}


void setN3dsTitleMenuInputActive(bool active) {
  sN3dsTitleMenuInputActive = active;
}

bool n3dsTitleMenuInputActive() {
  return sN3dsTitleMenuInputActive;
}


// ---- Stub Texture -------------------------------------------------------

namespace {

constexpr size_t N3dsMaxQueuedPrimitives = 768;

struct FrameReplayStats {
  size_t replayedPrimitives = 0;
  size_t texturedQuads = 0;
  size_t batchCount = 0;
  size_t scissoredBatches = 0;
};

#ifdef STAR_PLATFORM_N3DS
constexpr int N3dsTopScreenWidth = 400;
constexpr int N3dsTopScreenHeight = 240;
constexpr int N3dsBottomScreenWidth = 320;
constexpr int N3dsBottomScreenHeight = 240;
constexpr unsigned N3dsMaxTextureExtent = 1024;
constexpr size_t N3dsTextureUploadBudget = 6 * 1024 * 1024;
constexpr bool N3dsDrawTopDiagnostics = false;

struct N3dsLightState {
  bool enabled = false;
  Vec2U size;
  std::vector<Vec3F> values;
  Vec2F scale = {16.0f, 16.0f};
  Vec2F offset;
  float multiplier = 1.0f;
};

N3dsLightState sN3dsLightState;

u32 toC2dColor(Vec4B const& color) {
  return C2D_Color32(color[0], color[1], color[2], color[3]);
}

Vec3F sampleN3dsLight(Vec2F const& screenCoordinate) {
  if (!sN3dsLightState.enabled || sN3dsLightState.values.empty() || sN3dsLightState.scale[0] == 0.0f || sN3dsLightState.scale[1] == 0.0f)
    return Vec3F::filled(1.0f);

  Vec2F lightCoordinate = Vec2F(
      (screenCoordinate[0] - sN3dsLightState.offset[0]) / sN3dsLightState.scale[0],
      (screenCoordinate[1] - sN3dsLightState.offset[1]) / sN3dsLightState.scale[1]);
  int x = static_cast<int>(std::floor(lightCoordinate[0]));
  int y = static_cast<int>(std::floor(lightCoordinate[1]));
  if (x < 0 || y < 0 || x >= static_cast<int>(sN3dsLightState.size[0]) || y >= static_cast<int>(sN3dsLightState.size[1]))
    return Vec3F::filled(1.0f);

  return sN3dsLightState.values[static_cast<size_t>(y) * sN3dsLightState.size[0] + x];
}

Vec4B applyN3dsLighting(RenderVertex const& vertex) {
  float amount = std::clamp(vertex.param1, 0.0f, 1.0f);
  if (amount <= 0.0f || !sN3dsLightState.enabled)
    return vertex.color;

  Vec3F light = sampleN3dsLight(vertex.screenCoordinate) * sN3dsLightState.multiplier;
  Vec4B color = vertex.color;
  for (size_t i = 0; i < 3; ++i) {
    float factor = 1.0f - amount + amount * light[i];
    color[i] = static_cast<uint8_t>(std::clamp(std::round(color[i] * factor), 0.0f, 255.0f));
  }
  return color;
}

Vec4B averageN3dsLitQuadColor(RenderQuad const& quad) {
  Vec4B a = applyN3dsLighting(quad.a);
  Vec4B b = applyN3dsLighting(quad.b);
  Vec4B c = applyN3dsLighting(quad.c);
  Vec4B d = applyN3dsLighting(quad.d);
  return Vec4B(
      static_cast<uint8_t>((static_cast<unsigned>(a[0]) + b[0] + c[0] + d[0]) / 4),
      static_cast<uint8_t>((static_cast<unsigned>(a[1]) + b[1] + c[1] + d[1]) / 4),
      static_cast<uint8_t>((static_cast<unsigned>(a[2]) + b[2] + c[2] + d[2]) / 4),
      static_cast<uint8_t>((static_cast<unsigned>(a[3]) + b[3] + c[3] + d[3]) / 4));
}

inline float toTopScreenY(float yBottomOrigin) {
  return static_cast<float>(N3dsTopScreenHeight) - yBottomOrigin;
}

inline float toBottomScreenY(float yBottomOrigin) {
  return static_cast<float>(N3dsBottomScreenHeight) - yBottomOrigin;
}

inline Vec2F toTopScreenPoint(Vec2F const& bottomOriginPoint) {
  return Vec2F(bottomOriginPoint[0], toTopScreenY(bottomOriginPoint[1]));
}

unsigned n3dsTextureExtent(unsigned value) {
  unsigned extent = 8;
  while (extent < value)
    extent <<= 1;
  return extent;
}

u32 interleaveBits3(u32 value) {
  value = (value | (value << 2)) & 0x33;
  value = (value | (value << 1)) & 0x55;
  return value;
}

size_t n3dsTiledPixelIndex(unsigned x, unsigned y, unsigned textureWidth) {
  unsigned tileX = x >> 3;
  unsigned tileY = y >> 3;
  unsigned inTileX = x & 7;
  unsigned inTileY = y & 7;
  size_t tileIndex = static_cast<size_t>(tileY) * (textureWidth >> 3) + tileX;
  size_t mortonIndex = interleaveBits3(inTileX) | (interleaveBits3(inTileY) << 1);
  return tileIndex * 64 + mortonIndex;
}

void writeNativeRgba8Pixel(uint8_t* destination, uint8_t const* sourceRgba) {
  // tex3ds encodes GPU_RGBA8/RGBA8888 texture bytes as ABGR.
  destination[0] = sourceRgba[3];
  destination[1] = sourceRgba[2];
  destination[2] = sourceRgba[1];
  destination[3] = sourceRgba[0];
}

bool nearlyEqual(float a, float b) {
  return std::fabs(a - b) < 0.01f;
}

float metricFill(size_t value, size_t maximum, float maxExtent) {
  if (maximum == 0)
    return 0.0f;

  return maxExtent * (std::min(value, maximum) / static_cast<float>(maximum));
}

void drawDiagnosticBar(float x, float y, float width, float height, size_t value, size_t maximum, u32 color) {
  C2D_DrawRectSolid(x - 1.0f, y - 1.0f, 0.0f, width + 2.0f, height + 2.0f, C2D_Color32(255, 255, 255, 255));
  C2D_DrawRectSolid(x, y, 0.0f, width, height, C2D_Color32(24, 24, 24, 255));

  float fillWidth = metricFill(value, maximum, width);
  if (fillWidth > 0.0f)
    C2D_DrawRectSolid(x, y, 0.0f, fillWidth, height, color);
}

void drawTopScreenDiagnostics(FrameReplayStats const& stats) {
  constexpr float PanelX = 36.0f;
  constexpr float PanelY = 156.0f;
  constexpr float PanelWidth = 136.0f;
  constexpr float PanelHeight = 52.0f;
  constexpr float BarX = PanelX + 8.0f;
  constexpr float BarWidth = 120.0f;
  constexpr float BarHeight = 8.0f;

  C2D_DrawRectSolid(PanelX, PanelY, 0.0f, PanelWidth, PanelHeight, C2D_Color32(0, 0, 0, 224));

  drawDiagnosticBar(BarX, PanelY + 6.0f, BarWidth, BarHeight, stats.replayedPrimitives, N3dsMaxQueuedPrimitives, C2D_Color32(64, 224, 255, 255));
  drawDiagnosticBar(BarX, PanelY + 17.0f, BarWidth, BarHeight, stats.texturedQuads, 128, C2D_Color32(255, 96, 208, 255));
  drawDiagnosticBar(BarX, PanelY + 28.0f, BarWidth, BarHeight, stats.batchCount, 16, C2D_Color32(255, 176, 48, 255));
  drawDiagnosticBar(BarX, PanelY + 39.0f, BarWidth, BarHeight, stats.scissoredBatches, 16, C2D_Color32(255, 255, 255, 255));
}

void drawBottomSlot(float x, float y, float size, bool selected, bool filled, u32 fillColor) {
  u32 shadow = C2D_Color32(5, 7, 10, 220);
  u32 border = selected ? C2D_Color32(255, 242, 126, 255) : C2D_Color32(80, 84, 92, 255);
  C2D_DrawRectSolid(x + 1.0f, y + 2.0f, 0.0f, size, size, shadow);
  C2D_DrawRectSolid(x, y, 0.0f, size, size, border);
  C2D_DrawRectSolid(x + 2.0f, y + 2.0f, 0.0f, size - 4.0f, size - 4.0f, C2D_Color32(23, 26, 31, 255));
  C2D_DrawRectSolid(x + 3.0f, y + 3.0f, 0.0f, size - 6.0f, 5.0f, C2D_Color32(50, 55, 64, 255));

  if (filled) {
    C2D_DrawRectSolid(x + 7.0f, y + 9.0f, 0.0f, size - 14.0f, size - 14.0f, fillColor);
    C2D_DrawRectSolid(x + 9.0f, y + 11.0f, 0.0f, size - 18.0f, 3.0f, C2D_Color32(255, 255, 255, 90));
  }
}

void drawBottomStatusBar(float x, float y, float width, float height, float fill, u32 color) {
  float clampedFill = std::clamp(fill, 0.0f, 1.0f);
  C2D_DrawRectSolid(x, y, 0.0f, width, height, C2D_Color32(8, 10, 14, 255));
  C2D_DrawRectSolid(x + 1.0f, y + 1.0f, 0.0f, width - 2.0f, height - 2.0f, C2D_Color32(88, 92, 102, 255));
  C2D_DrawRectSolid(x + 2.0f, y + 2.0f, 0.0f, width - 4.0f, height - 4.0f, C2D_Color32(18, 20, 24, 255));
  C2D_DrawRectSolid(x + 3.0f, y + 3.0f, 0.0f, std::max(0.0f, (width - 6.0f) * clampedFill), height - 6.0f, color);
  C2D_DrawRectSolid(x + 3.0f, y + 3.0f, 0.0f, std::max(0.0f, (width - 6.0f) * clampedFill), 2.0f, C2D_Color32(255, 255, 255, 60));
}

void drawBottomRoundButton(float x, float y, float radius, bool active, u32 color) {
  C2D_DrawCircleSolid(x, y, 0.0f, radius + 2.0f, active ? C2D_Color32(255, 255, 255, 255) : C2D_Color32(70, 76, 86, 255));
  C2D_DrawCircleSolid(x, y, 0.0f, radius, active ? color : C2D_Color32(30, 34, 40, 255));
  C2D_DrawCircleSolid(x, y, 0.0f, radius * 0.45f, color);
}

enum class BottomButtonGlyph {
  Check,
  Cross,
  Up,
  Diamond
};

void drawBottomButtonGlyph(float x, float y, BottomButtonGlyph glyph, u32 color) {
  if (glyph == BottomButtonGlyph::Check) {
    C2D_DrawLine(x - 6.0f, y + 1.0f, color, x - 1.0f, y + 6.0f, color, 2.0f, 0.0f);
    C2D_DrawLine(x - 1.0f, y + 6.0f, color, x + 7.0f, y - 6.0f, color, 2.0f, 0.0f);
  } else if (glyph == BottomButtonGlyph::Cross) {
    C2D_DrawLine(x - 6.0f, y - 6.0f, color, x + 6.0f, y + 6.0f, color, 2.0f, 0.0f);
    C2D_DrawLine(x + 6.0f, y - 6.0f, color, x - 6.0f, y + 6.0f, color, 2.0f, 0.0f);
  } else if (glyph == BottomButtonGlyph::Up) {
    C2D_DrawTriangle(x, y - 8.0f, color, x - 8.0f, y + 6.0f, color, x + 8.0f, y + 6.0f, color, 0.0f);
  } else if (glyph == BottomButtonGlyph::Diamond) {
    C2D_DrawTriangle(x, y - 8.0f, color, x - 8.0f, y, color, x, y + 8.0f, color, 0.0f);
    C2D_DrawTriangle(x, y - 8.0f, color, x, y + 8.0f, color, x + 8.0f, y, color, 0.0f);
  }
}

void drawBottomQuickButton(float x, float y, float radius, bool active, u32 color, BottomButtonGlyph glyph) {
  drawBottomRoundButton(x, y, radius, active, color);
  drawBottomButtonGlyph(x, y, glyph, C2D_Color32(255, 255, 255, 255));
}

void drawBottomToolIcon(float x, float y, unsigned index, bool selected) {
  float size = selected ? 34.0f : 28.0f;
  u32 border = selected ? C2D_Color32(255, 242, 126, 255) : C2D_Color32(68, 72, 82, 255);
  u32 fill = selected ? C2D_Color32(38, 42, 50, 255) : C2D_Color32(24, 27, 33, 255);
  C2D_DrawRectSolid(x - size / 2.0f + 2.0f, y - size / 2.0f + 3.0f, 0.0f, size, size, C2D_Color32(0, 0, 0, 190));
  C2D_DrawRectSolid(x - size / 2.0f, y - size / 2.0f, 0.0f, size, size, border);
  C2D_DrawRectSolid(x - size / 2.0f + 2.0f, y - size / 2.0f + 2.0f, 0.0f, size - 4.0f, size - 4.0f, fill);

  u32 icon = index == 0 ? C2D_Color32(232, 188, 72, 255) : index == 1 ? C2D_Color32(220, 104, 240, 255) : index == 2 ? C2D_Color32(120, 190, 255, 255) : C2D_Color32(255, 234, 116, 255);
  if (index == 0) {
    C2D_DrawLine(x - 8.0f, y + 9.0f, icon, x + 7.0f, y - 7.0f, icon, 3.0f, 0.0f);
    C2D_DrawTriangle(x + 5.0f, y - 10.0f, icon, x + 12.0f, y - 5.0f, icon, x + 5.0f, y + 1.0f, icon, 0.0f);
  } else if (index == 1) {
    C2D_DrawCircleSolid(x - 4.0f, y - 2.0f, 0.0f, 6.0f, icon);
    C2D_DrawLine(x + 2.0f, y + 4.0f, icon, x + 11.0f, y + 13.0f, icon, 3.0f, 0.0f);
  } else if (index == 2) {
    C2D_DrawLine(x - 9.0f, y + 8.0f, icon, x + 8.0f, y - 9.0f, icon, 4.0f, 0.0f);
    C2D_DrawRectSolid(x + 5.0f, y - 12.0f, 0.0f, 5.0f, 10.0f, icon);
  } else {
    C2D_DrawCircleSolid(x - 2.0f, y - 2.0f, 0.0f, 7.0f, icon);
    C2D_DrawLine(x + 4.0f, y + 4.0f, icon, x + 12.0f, y + 12.0f, icon, 3.0f, 0.0f);
  }
}

void drawBottomMissionCard(bool inWorld) {
  C2D_DrawRectSolid(70.0f, 55.0f, 0.0f, 206.0f, 40.0f, C2D_Color32(8, 9, 12, 230));
  C2D_DrawRectSolid(72.0f, 57.0f, 0.0f, 202.0f, 36.0f, C2D_Color32(54, 58, 66, 255));
  C2D_DrawRectSolid(75.0f, 60.0f, 0.0f, 196.0f, 30.0f, C2D_Color32(32, 35, 42, 255));
  C2D_DrawRectSolid(85.0f, 70.0f, 0.0f, inWorld ? 106.0f : 60.0f, 4.0f, C2D_Color32(255, 255, 255, 230));
  C2D_DrawRectSolid(85.0f, 80.0f, 0.0f, inWorld ? 142.0f : 92.0f, 4.0f, inWorld ? C2D_Color32(72, 220, 128, 255) : C2D_Color32(255, 224, 48, 255));
  C2D_DrawCircleSolid(58.0f, 75.0f, 0.0f, 20.0f, C2D_Color32(70, 76, 86, 255));
  C2D_DrawCircleSolid(58.0f, 75.0f, 0.0f, 16.0f, C2D_Color32(24, 27, 33, 255));
  C2D_DrawLine(50.0f, 67.0f, C2D_Color32(120, 126, 136, 255), 66.0f, 83.0f, C2D_Color32(120, 126, 136, 255), 3.0f, 0.0f);
  C2D_DrawLine(66.0f, 67.0f, C2D_Color32(120, 126, 136, 255), 50.0f, 83.0f, C2D_Color32(120, 126, 136, 255), 3.0f, 0.0f);
}

void drawBottomMovementPad(float x, float y, bool circlePadActive, bool dpadActive) {
  u32 base = C2D_Color32(30, 34, 40, 255);
  u32 border = circlePadActive || dpadActive ? C2D_Color32(255, 255, 255, 255) : C2D_Color32(78, 84, 94, 255);
  u32 accent = circlePadActive ? C2D_Color32(72, 220, 128, 255) : C2D_Color32(96, 224, 255, 255);

  C2D_DrawCircleSolid(x, y, 0.0f, 36.0f, C2D_Color32(12, 14, 18, 255));
  C2D_DrawCircleSolid(x, y, 0.0f, 34.0f, border);
  C2D_DrawCircleSolid(x, y, 0.0f, 31.0f, base);
  C2D_DrawRectSolid(x - 7.0f, y - 27.0f, 0.0f, 14.0f, 54.0f, C2D_Color32(46, 52, 60, 255));
  C2D_DrawRectSolid(x - 27.0f, y - 7.0f, 0.0f, 54.0f, 14.0f, C2D_Color32(46, 52, 60, 255));
  C2D_DrawCircleSolid(x, y, 0.0f, circlePadActive ? 12.0f : 9.0f, accent);
  if (dpadActive)
    C2D_DrawCircleSolid(x, y, 0.0f, 4.0f, C2D_Color32(255, 224, 48, 255));
}

void drawBottomCursor(Vec2F const& position, bool pressed, bool touchCursor) {
  float x = std::clamp(position[0], 0.0f, static_cast<float>(N3dsBottomScreenWidth - 1));
  float y = std::clamp(toBottomScreenY(position[1]), 0.0f, static_cast<float>(N3dsBottomScreenHeight - 1));
  u32 color = touchCursor ? C2D_Color32(255, 224, 48, 255) : C2D_Color32(96, 224, 255, 255);
  u32 shadow = C2D_Color32(0, 0, 0, 255);
  float radius = pressed ? 8.0f : 6.0f;

  C2D_DrawLine(x - 10.0f, y, shadow, x + 10.0f, y, shadow, 3.0f, 0.0f);
  C2D_DrawLine(x, y - 10.0f, shadow, x, y + 10.0f, shadow, 3.0f, 0.0f);
  C2D_DrawLine(x - 10.0f, y, color, x + 10.0f, y, color, 1.0f, 0.0f);
  C2D_DrawLine(x, y - 10.0f, color, x, y + 10.0f, color, 1.0f, 0.0f);
  C2D_DrawCircleSolid(x, y, 0.0f, radius, C2D_Color32(0, 0, 0, 220));
  C2D_DrawCircleSolid(x, y, 0.0f, radius - 2.0f, color);
}

void drawBottomStartupDiagnostics(uint32_t diagnosticBits) {
  constexpr float BeadSize = 6.0f;
  constexpr float BeadGap = 4.0f;
  constexpr float StartX = 168.0f;
  constexpr float StartY = 9.0f;
  u32 activeColors[] = {
      C2D_Color32(96, 224, 255, 255),
      C2D_Color32(72, 220, 128, 255),
      C2D_Color32(255, 224, 48, 255),
      C2D_Color32(255, 144, 72, 255),
      C2D_Color32(216, 120, 255, 255),
      C2D_Color32(255, 104, 160, 255),
      C2D_Color32(224, 224, 224, 255),
      C2D_Color32(232, 64, 72, 255),
  };

  for (unsigned i = 0; i < 8; ++i) {
    float x = StartX + i * (BeadSize + BeadGap);
    u32 color = (diagnosticBits & (1u << i)) ? activeColors[i] : C2D_Color32(46, 52, 60, 255);
    C2D_DrawRectSolid(x, StartY, 0.0f, BeadSize, BeadSize, C2D_Color32(10, 12, 16, 255));
    C2D_DrawRectSolid(x + 1.0f, StartY + 1.0f, 0.0f, BeadSize - 2.0f, BeadSize - 2.0f, color);
  }
}

C2D_TextBuf& bottomOverlayTextBuffer() {
  static C2D_TextBuf textBuffer = C2D_TextBufNew(2048);
  return textBuffer;
}

void drawBottomText(char const* text, float x, float y, float scale, u32 color) {
  C2D_Text parsedText;
  C2D_TextParse(&parsedText, bottomOverlayTextBuffer(), text);
  C2D_TextOptimize(&parsedText);
  C2D_DrawText(&parsedText, C2D_WithColor, x, y, 0.0f, scale, scale, color);
}

void drawBottomTitleMenu(N3dsHandheldOverlayState const& overlayState) {
  C2D_TextBufClear(bottomOverlayTextBuffer());

  C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, 320.0f, 240.0f, C2D_Color32(13, 17, 25, 255));
  C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, 320.0f, 48.0f, C2D_Color32(23, 31, 45, 255));
  C2D_DrawRectSolid(0.0f, 48.0f, 0.0f, 320.0f, 1.0f, C2D_Color32(88, 104, 124, 255));
  drawBottomText("OPENSTARBOUND", 18.0f, 13.0f, 0.62f, C2D_Color32(230, 238, 246, 255));
  drawBottomStartupDiagnostics(overlayState.startupDiagnostics);

  struct MenuItem {
    char const* label;
    u32 accent;
  };

  static const MenuItem MenuItems[] = {
      {"Single Player", C2D_Color32(255, 214, 82, 255)},
      {"Multiplayer", C2D_Color32(88, 190, 255, 255)},
      {"Settings", C2D_Color32(92, 220, 144, 255)},
      {"Exit", C2D_Color32(238, 82, 92, 255)},
  };

  constexpr float ButtonX = 18.0f;
  constexpr float ButtonY = 58.0f;
  constexpr float ButtonW = 284.0f;
  constexpr float ButtonH = 34.0f;
  constexpr float ButtonGap = 10.0f;

  for (unsigned item = 0; item < 4; ++item) {
    float y = ButtonY + item * (ButtonH + ButtonGap);
    bool selected = item == overlayState.selectedTitleMenuItem;
    u32 base = selected ? C2D_Color32(52, 64, 82, 255) : C2D_Color32(30, 37, 49, 255);
    u32 border = selected ? C2D_Color32(226, 234, 244, 255) : C2D_Color32(74, 84, 98, 255);

    C2D_DrawRectSolid(ButtonX, y, 0.0f, ButtonW, ButtonH, border);
    C2D_DrawRectSolid(ButtonX + 1.0f, y + 1.0f, 0.0f, ButtonW - 2.0f, ButtonH - 2.0f, base);
    C2D_DrawRectSolid(ButtonX + 8.0f, y + 8.0f, 0.0f, 5.0f, ButtonH - 16.0f, MenuItems[item].accent);
    drawBottomText(MenuItems[item].label, ButtonX + 24.0f, y + 8.0f, 0.52f, selected ? C2D_Color32(255, 255, 255, 255) : C2D_Color32(204, 214, 226, 255));
  }

  drawBottomText("A: Select   B: Exit   X: Multi   Y: Settings", 16.0f, 224.0f, 0.34f, C2D_Color32(160, 174, 190, 255));

  if (overlayState.touchPressed)
    drawBottomCursor(overlayState.touchPosition, true, true);
}

void drawBottomHandheldOverlay(N3dsHandheldOverlayState const& overlayState, unsigned frameCounter) {
  (void)frameCounter;

  if (overlayState.titleMenuActive) {
    drawBottomTitleMenu(overlayState);
    return;
  }

  C2D_TextBufClear(bottomOverlayTextBuffer());
  C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, 320.0f, 240.0f, C2D_Color32(178, 232, 248, 255));
  C2D_DrawCircleSolid(42.0f, 38.0f, 0.0f, 34.0f, C2D_Color32(220, 246, 255, 255));
  C2D_DrawCircleSolid(104.0f, 26.0f, 0.0f, 28.0f, C2D_Color32(210, 242, 255, 255));
  C2D_DrawCircleSolid(248.0f, 45.0f, 0.0f, 42.0f, C2D_Color32(222, 247, 255, 255));
  C2D_DrawRectSolid(0.0f, 198.0f, 0.0f, 320.0f, 42.0f, C2D_Color32(28, 30, 36, 235));
  C2D_DrawRectSolid(0.0f, 197.0f, 0.0f, 320.0f, 1.0f, C2D_Color32(82, 88, 98, 255));

  drawBottomStatusBar(10.0f, 9.0f, 98.0f, 10.0f, overlayState.healthFill, C2D_Color32(232, 64, 72, 255));
  drawBottomStatusBar(10.0f, 23.0f, 98.0f, 10.0f, overlayState.energyFill, C2D_Color32(72, 176, 255, 255));
  drawBottomStatusBar(118.0f, 9.0f, 58.0f, 10.0f, overlayState.breathFill, C2D_Color32(72, 220, 128, 255));
  drawBottomStatusBar(118.0f, 23.0f, 58.0f, 10.0f, overlayState.circlePadActive ? 1.0f : 0.22f, C2D_Color32(255, 224, 48, 255));
  drawBottomStartupDiagnostics(overlayState.startupDiagnostics);

  for (unsigned icon = 0; icon < 4; ++icon)
    drawBottomToolIcon(118.0f + icon * 36.0f, 44.0f, icon, icon == overlayState.selectedHotbarSlot % 4);

  drawBottomMissionCard(overlayState.inWorld);

  drawBottomRoundButton(210.0f, 21.0f, 7.0f, overlayState.shoulderZL, C2D_Color32(216, 120, 255, 255));
  drawBottomRoundButton(242.0f, 21.0f, 9.0f, overlayState.shoulderL, C2D_Color32(96, 224, 255, 255));
  drawBottomRoundButton(276.0f, 21.0f, 9.0f, overlayState.shoulderR, C2D_Color32(255, 224, 48, 255));
  drawBottomRoundButton(308.0f, 21.0f, 7.0f, overlayState.shoulderZR, C2D_Color32(255, 104, 160, 255));

  C2D_DrawRectSolid(14.0f, 110.0f, 0.0f, 112.0f, 70.0f, C2D_Color32(17, 20, 26, 220));
  drawBottomMovementPad(70.0f, 145.0f, overlayState.circlePadActive, overlayState.dpadActive);

  C2D_DrawRectSolid(146.0f, 112.0f, 0.0f, 56.0f, 56.0f, C2D_Color32(16, 19, 24, 225));
  C2D_DrawCircleSolid(174.0f, 140.0f, 0.0f, 18.0f, C2D_Color32(68, 74, 84, 255));
  C2D_DrawCircleSolid(174.0f, 140.0f, 0.0f, 11.0f, C2D_Color32(38, 42, 50, 255));
  C2D_DrawCircleSolid(174.0f, 140.0f, 0.0f, 5.0f, C2D_Color32(255, 224, 48, 255));

  C2D_DrawRectSolid(214.0f, 100.0f, 0.0f, 94.0f, 86.0f, C2D_Color32(18, 21, 27, 225));
  drawBottomQuickButton(292.0f, 144.0f, 13.0f, overlayState.buttonA, C2D_Color32(255, 224, 48, 255), BottomButtonGlyph::Check);
  drawBottomQuickButton(260.0f, 174.0f, 13.0f, overlayState.buttonB, C2D_Color32(232, 64, 72, 255), BottomButtonGlyph::Cross);
  drawBottomQuickButton(260.0f, 114.0f, 13.0f, overlayState.buttonX, C2D_Color32(96, 224, 255, 255), BottomButtonGlyph::Up);
  drawBottomQuickButton(228.0f, 144.0f, 13.0f, overlayState.buttonY, C2D_Color32(72, 220, 128, 255), BottomButtonGlyph::Diamond);
  C2D_DrawRectSolid(206.0f, 99.0f, 0.0f, 24.0f, 5.0f, C2D_Color32(216, 120, 255, 255));
  C2D_DrawRectSolid(206.0f, 179.0f, 0.0f, 30.0f, 5.0f, C2D_Color32(255, 104, 160, 255));

  float slotSize = 28.0f;
  float slotGap = 2.0f;
  float slotX = 11.0f;
  float slotY = 206.0f;
  u32 slotColors[] = {
      C2D_Color32(232, 188, 72, 255), C2D_Color32(220, 104, 240, 255), C2D_Color32(120, 190, 255, 255), C2D_Color32(255, 234, 116, 255), C2D_Color32(80, 160, 255, 255),
      C2D_Color32(255, 144, 72, 255), C2D_Color32(80, 86, 96, 255), C2D_Color32(80, 86, 96, 255), C2D_Color32(80, 86, 96, 255), C2D_Color32(80, 86, 96, 255)};
  for (unsigned slot = 0; slot < 10; ++slot)
    drawBottomSlot(slotX + slot * (slotSize + slotGap), slotY, slotSize, slot == overlayState.selectedHotbarSlot, slot < 6, slotColors[slot]);

  Vec2F bottomPointer = {overlayState.pointerPosition[0] * (static_cast<float>(N3dsBottomScreenWidth) / static_cast<float>(N3dsTopScreenWidth)), overlayState.pointerPosition[1]};
  drawBottomCursor(bottomPointer, overlayState.pointerPressed, false);
  if (overlayState.touchPressed)
    drawBottomCursor(overlayState.touchPosition, true, true);
}

void applyScissorRect(Maybe<RectI> const& scissorRect) {
  if (!scissorRect) {
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    return;
  }

  int left = std::clamp(scissorRect->xMin(), 0, N3dsTopScreenWidth);
  int right = std::clamp(scissorRect->xMax(), 0, N3dsTopScreenWidth);
  int bottomOriginMinY = std::clamp(scissorRect->yMin(), 0, N3dsTopScreenHeight);
  int bottomOriginMaxY = std::clamp(scissorRect->yMax(), 0, N3dsTopScreenHeight);
  int top = N3dsTopScreenHeight - bottomOriginMaxY;
  int bottom = N3dsTopScreenHeight - bottomOriginMinY;

  if (right <= left || bottom <= top) {
    C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, 0, 0, 0);
    return;
  }

  C3D_SetScissor(GPU_SCISSOR_NORMAL, static_cast<u32>(left), static_cast<u32>(top), static_cast<u32>(right), static_cast<u32>(bottom));
}

size_t& n3dsTextureBytesInUse() {
  static size_t bytesInUse = 0;
  return bytesInUse;
}
#endif

RenderVertex transformedVertex(RenderVertex vertex, Mat3F const& transformation) {
  vertex.screenCoordinate = transformation * vertex.screenCoordinate;
  return vertex;
}

RenderPrimitive transformedPrimitive(RenderPrimitive primitive, Mat3F const& transformation) {
  if (auto tri = primitive.ptr<RenderTriangle>()) {
    tri->a = transformedVertex(tri->a, transformation);
    tri->b = transformedVertex(tri->b, transformation);
    tri->c = transformedVertex(tri->c, transformation);
  } else if (auto quad = primitive.ptr<RenderQuad>()) {
    quad->a = transformedVertex(quad->a, transformation);
    quad->b = transformedVertex(quad->b, transformation);
    quad->c = transformedVertex(quad->c, transformation);
    quad->d = transformedVertex(quad->d, transformation);
  } else if (auto poly = primitive.ptr<RenderPoly>()) {
    for (auto& vertex : poly->vertexes)
      vertex = transformedVertex(vertex, transformation);
  }

  return primitive;
}

class N3dsStubTexture : public Texture {
public:
  N3dsStubTexture(Image const& image, TextureFiltering filtering, TextureAddressing addressing)
    : m_size(image.size()), m_filtering(filtering), m_addressing(addressing) {
#ifdef STAR_PLATFORM_N3DS
    uploadImage(image);
#endif
  }

  N3dsStubTexture(Vec2U size, TextureFiltering filtering, TextureAddressing addressing)
    : m_size(size), m_filtering(filtering), m_addressing(addressing) {}

  ~N3dsStubTexture() override {
#ifdef STAR_PLATFORM_N3DS
    if (m_textureReady) {
      C3D_TexDelete(&m_texture);
      n3dsTextureBytesInUse() -= m_textureBytes;
    }
#endif
  }

  Vec2U size() const override { return m_size; }
  TextureFiltering filtering() const override { return m_filtering; }
  TextureAddressing addressing() const override { return m_addressing; }

#ifdef STAR_PLATFORM_N3DS
  bool ready() const {
    return m_textureReady;
  }

  C2D_Image image() const {
    return C2D_Image{const_cast<C3D_Tex*>(&m_texture), &m_subTexture};
  }

  bool imageForQuad(RenderQuad const& quad, C2D_Image& image, Tex3DS_SubTexture& subTexture) const {
    if (!m_textureReady || m_storageWidth == 0 || m_storageHeight == 0)
      return false;

    auto clampTextureCoordinate = [](float value, float maximum) {
      return std::clamp(value, 0.0f, maximum);
    };

    float maxTextureX = static_cast<float>(m_size[0]);
    float maxTextureY = static_cast<float>(m_size[1]);
    float left = clampTextureCoordinate(quad.a.textureCoordinate[0], maxTextureX);
    float right = clampTextureCoordinate(quad.b.textureCoordinate[0], maxTextureX);
    float bottom = clampTextureCoordinate(quad.a.textureCoordinate[1], maxTextureY);
    float top = clampTextureCoordinate(quad.d.textureCoordinate[1], maxTextureY);

    if (right <= left || top <= bottom)
      return false;

    subTexture.width = static_cast<u16>(std::lround(right - left));
    subTexture.height = static_cast<u16>(std::lround(top - bottom));
    subTexture.left = left / static_cast<float>(m_storageWidth);
    subTexture.right = right / static_cast<float>(m_storageWidth);
    subTexture.bottom = 1.0f - (maxTextureY - bottom) / static_cast<float>(m_storageHeight);
    subTexture.top = 1.0f - (maxTextureY - top) / static_cast<float>(m_storageHeight);
    image = C2D_Image{const_cast<C3D_Tex*>(&m_texture), &subTexture};
    return true;
  }
#endif

private:
#ifdef STAR_PLATFORM_N3DS
  void uploadImage(Image const& image) {
    if (image.empty())
      return;

    unsigned storageWidth = n3dsTextureExtent(image.width());
    unsigned storageHeight = n3dsTextureExtent(image.height());
    if (storageWidth > N3dsMaxTextureExtent || storageHeight > N3dsMaxTextureExtent) {
      Logger::warn("N3dsStubTexture: skipping oversized texture {}x{}", image.width(), image.height());
      return;
    }

    size_t textureBytes = static_cast<size_t>(storageWidth) * storageHeight * 4;
    if (n3dsTextureBytesInUse() + textureBytes > N3dsTextureUploadBudget) {
      Logger::warn("N3dsStubTexture: skipping texture {}x{} because upload budget is exhausted", image.width(), image.height());
      return;
    }

    void* linearUploadData = linearAlloc(textureBytes);
    if (!linearUploadData) {
      Logger::warn("N3dsStubTexture: linearAlloc failed for {} byte upload", textureBytes);
      return;
    }

    std::memset(linearUploadData, 0, textureBytes);
    auto* uploadBytes = static_cast<uint8_t*>(linearUploadData);
    for (unsigned y = 0; y < image.height(); ++y) {
      unsigned textureY = image.height() - y - 1;
      for (unsigned x = 0; x < image.width(); ++x) {
        size_t destinationOffset = n3dsTiledPixelIndex(x, textureY, storageWidth) * 4;
        auto pixel = image.getrgb({x, y});
        writeNativeRgba8Pixel(uploadBytes + destinationOffset, pixel.ptr());
      }
    }

    if (!C3D_TexInit(&m_texture, static_cast<u16>(storageWidth), static_cast<u16>(storageHeight), GPU_RGBA8)) {
      linearFree(linearUploadData);
      Logger::warn("N3dsStubTexture: C3D_TexInit failed for {}x{}", image.width(), image.height());
      return;
    }

    C3D_TexUpload(&m_texture, linearUploadData);
    linearFree(linearUploadData);

    auto filter = m_filtering == TextureFiltering::Linear ? GPU_LINEAR : GPU_NEAREST;
    auto wrap = m_addressing == TextureAddressing::Wrap ? GPU_REPEAT : GPU_CLAMP_TO_EDGE;
    C3D_TexSetFilter(&m_texture, filter, filter);
    C3D_TexSetWrap(&m_texture, wrap, wrap);

    m_subTexture.width = static_cast<u16>(image.width());
    m_subTexture.height = static_cast<u16>(image.height());
    m_subTexture.left = 0.0f;
    m_subTexture.right = static_cast<float>(image.width()) / static_cast<float>(storageWidth);
    m_subTexture.bottom = 1.0f - static_cast<float>(image.height()) / static_cast<float>(storageHeight);
    m_subTexture.top = 1.0f;
    m_storageWidth = storageWidth;
    m_storageHeight = storageHeight;
    m_textureBytes = textureBytes;
    n3dsTextureBytesInUse() += m_textureBytes;
    m_textureReady = true;
  }
#endif

  Vec2U m_size;
  TextureFiltering m_filtering;
  TextureAddressing m_addressing;

#ifdef STAR_PLATFORM_N3DS
  C3D_Tex m_texture{};
  Tex3DS_SubTexture m_subTexture{};
  unsigned m_storageWidth = 0;
  unsigned m_storageHeight = 0;
  size_t m_textureBytes = 0;
  bool m_textureReady = false;
#endif
};

class N3dsStubTextureGroup : public TextureGroup {
public:
  explicit N3dsStubTextureGroup(TextureFiltering filtering) : m_filtering(filtering) {}

  TextureFiltering filtering() const override { return m_filtering; }
  TexturePtr create(Image const& texture) override {
    return make_ref<N3dsStubTexture>(texture, m_filtering, TextureAddressing::Clamp);
  }

private:
  TextureFiltering m_filtering;
};

class N3dsStubRenderBuffer : public RenderBuffer {
public:
  void set(List<RenderPrimitive>& primitives) override {
    m_primitives = std::move(primitives);
  }

  List<RenderPrimitive> const& primitives() const {
    return m_primitives;
  }

private:
  List<RenderPrimitive> m_primitives;
};

#ifdef STAR_PLATFORM_N3DS
bool isAxisAlignedQuad(RenderQuad const& quad) {
  return nearlyEqual(quad.a.screenCoordinate[1], quad.b.screenCoordinate[1])
      && nearlyEqual(quad.c.screenCoordinate[1], quad.d.screenCoordinate[1])
      && nearlyEqual(quad.a.screenCoordinate[0], quad.d.screenCoordinate[0])
      && nearlyEqual(quad.b.screenCoordinate[0], quad.c.screenCoordinate[0]);
}

bool isAxisAlignedTextureRect(RenderQuad const& quad) {
  return nearlyEqual(quad.a.textureCoordinate[1], quad.b.textureCoordinate[1])
      && nearlyEqual(quad.c.textureCoordinate[1], quad.d.textureCoordinate[1])
      && nearlyEqual(quad.a.textureCoordinate[0], quad.d.textureCoordinate[0])
      && nearlyEqual(quad.b.textureCoordinate[0], quad.c.textureCoordinate[0]);
}

bool isOpaqueWhite(Vec4B const& color) {
  return color[0] == 255 && color[1] == 255 && color[2] == 255 && color[3] == 255;
}

void drawUntexturedTriangle(RenderVertex const& a, RenderVertex const& b, RenderVertex const& c) {
  C2D_DrawTriangle(
  a.screenCoordinate[0], toTopScreenY(a.screenCoordinate[1]), toC2dColor(applyN3dsLighting(a)),
  b.screenCoordinate[0], toTopScreenY(b.screenCoordinate[1]), toC2dColor(applyN3dsLighting(b)),
  c.screenCoordinate[0], toTopScreenY(c.screenCoordinate[1]), toC2dColor(applyN3dsLighting(c)),
      0.0f);
}

bool drawTexturedQuad(RenderQuad const& quad) {
  if (!quad.texture)
    return false;

  auto texture = dynamic_cast<N3dsStubTexture const*>(quad.texture.get());
  if (!texture || !texture->ready() || !isAxisAlignedTextureRect(quad))
    return false;

  C2D_ImageTint tint;
  C2D_Image image;
  Tex3DS_SubTexture subTexture;
  if (!texture->imageForQuad(quad, image, subTexture))
    return false;

  C2D_DrawParams params{};
  if (isAxisAlignedQuad(quad)) {
    float minX = std::min(std::min(quad.a.screenCoordinate[0], quad.b.screenCoordinate[0]), std::min(quad.c.screenCoordinate[0], quad.d.screenCoordinate[0]));
    float maxX = std::max(std::max(quad.a.screenCoordinate[0], quad.b.screenCoordinate[0]), std::max(quad.c.screenCoordinate[0], quad.d.screenCoordinate[0]));
    float minY = std::min(std::min(quad.a.screenCoordinate[1], quad.b.screenCoordinate[1]), std::min(quad.c.screenCoordinate[1], quad.d.screenCoordinate[1]));
    float maxY = std::max(std::max(quad.a.screenCoordinate[1], quad.b.screenCoordinate[1]), std::max(quad.c.screenCoordinate[1], quad.d.screenCoordinate[1]));

    float width = maxX - minX;
    float height = maxY - minY;
    if (width <= 0.0f || height <= 0.0f)
      return true;

    params = {{minX, toTopScreenY(maxY), width, height}, {0.0f, 0.0f}, 0.0f, 0.0f};
  } else {
    Vec2F topLeft = toTopScreenPoint(quad.d.screenCoordinate);
    Vec2F topRight = toTopScreenPoint(quad.c.screenCoordinate);
    Vec2F bottomLeft = toTopScreenPoint(quad.a.screenCoordinate);
    Vec2F widthVector = topRight - topLeft;
    Vec2F heightVector = bottomLeft - topLeft;
    float width = vmag(widthVector);
    float height = vmag(heightVector);
    if (width <= 0.0f || height <= 0.0f)
      return true;

    params = {{topLeft[0], topLeft[1], width, height}, {0.0f, 0.0f}, 0.0f, std::atan2(widthVector[1], widthVector[0])};
  }

  Vec4B quadColor = averageN3dsLitQuadColor(quad);
  if (isOpaqueWhite(quadColor))
    return C2D_DrawImage(image, &params, nullptr);

  C2D_PlainImageTint(&tint, toC2dColor(quadColor), 1.0f);
  C2D_SetTintMode(C2D_TintMult);
  bool drawn = C2D_DrawImage(image, &params, &tint);
  C2D_SetTintMode(C2D_TintSolid);
  return drawn;
}

void drawPrimitive(RenderPrimitive const& primitive, C3D_RenderTarget* target, FrameReplayStats& stats) {
  (void)target;
  ++stats.replayedPrimitives;

  if (auto tri = primitive.ptr<RenderTriangle>()) {
    drawUntexturedTriangle(tri->a, tri->b, tri->c);
  } else if (auto quad = primitive.ptr<RenderQuad>()) {
    if (drawTexturedQuad(*quad)) {
      ++stats.texturedQuads;
      return;
    }

    drawUntexturedTriangle(quad->a, quad->b, quad->c);
    drawUntexturedTriangle(quad->a, quad->c, quad->d);
  } else if (auto poly = primitive.ptr<RenderPoly>()) {
    if (poly->vertexes.size() >= 3) {
      auto const& a = poly->vertexes[0];
      for (size_t i = 1; i + 1 < poly->vertexes.size(); ++i)
        drawUntexturedTriangle(a, poly->vertexes[i], poly->vertexes[i + 1]);
    }
  }
}
#endif

} // anonymous namespace

// ---- N3dsStubRenderer ---------------------------------------------------

N3dsStubRenderer::N3dsStubRenderer() {
#ifdef STAR_PLATFORM_N3DS
  if (C3D_Init(C3D_DEFAULT_CMDBUF_SIZE) && C2D_Init(C2D_DEFAULT_MAX_OBJECTS)) {
    C2D_Prepare();
    m_topTarget = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    m_bottomTarget = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    m_gpuReady = m_topTarget != nullptr;
  }

  if (m_gpuReady)
    Logger::info("N3dsStubRenderer: minimal citro frame path initialised");
  else
    Logger::warn("N3dsStubRenderer: citro init failed, renderer falling back to no-op");
#else
  Logger::info("N3dsStubRenderer: non-N3DS build using no-op renderer"); // STUB
#endif
}

N3dsStubRenderer::~N3dsStubRenderer() {
#ifdef STAR_PLATFORM_N3DS
  if (m_gpuReady) {
    C2D_Fini();
    C3D_Fini();
    m_gpuReady = false;
    m_topTarget = nullptr;
    m_bottomTarget = nullptr;
  }
#endif
}

String N3dsStubRenderer::rendererId() const { return "N3dsStub"; }

Vec2U N3dsStubRenderer::screenSize() const {
  // PLACEHOLDER: return top-screen size until the renderer API exposes a
  // handheld dual-screen layout contract.
  return Vec2U(N3DS_TOP_SCREEN_WIDTH, N3DS_TOP_SCREEN_HEIGHT);
}

void N3dsStubRenderer::loadConfig(Json const&) {}                          // STUB
void N3dsStubRenderer::loadEffectConfig(String const&, Json const&, StringMap<String> const&) {} // STUB
void N3dsStubRenderer::setEffectParameter(String const& parameterName, RenderEffectParameter const& parameter) {
#ifdef STAR_PLATFORM_N3DS
  if (parameterName == "lightMapEnabled") {
    if (auto value = parameter.ptr<bool>())
      sN3dsLightState.enabled = *value;
  } else if (parameterName == "lightMapScale") {
    if (auto value = parameter.ptr<Vec2F>())
      sN3dsLightState.scale = *value;
  } else if (parameterName == "lightMapOffset") {
    if (auto value = parameter.ptr<Vec2F>())
      sN3dsLightState.offset = *value;
  } else if (parameterName == "lightMapMultiplier") {
    if (auto value = parameter.ptr<float>())
      sN3dsLightState.multiplier = *value;
  }
#else
  (void)parameterName;
  (void)parameter;
#endif
}
void N3dsStubRenderer::setEffectScriptableParameter(String const&, String const&, RenderEffectParameter const&) {} // STUB

Maybe<RenderEffectParameter> N3dsStubRenderer::getEffectScriptableParameter(String const&, String const&) {
  return {}; // STUB
}

Maybe<VariantTypeIndex> N3dsStubRenderer::getEffectScriptableParameterType(String const&, String const&) {
  return {}; // STUB
}

void N3dsStubRenderer::setEffectTexture(String const& textureName, ImageView const& image) {
#ifdef STAR_PLATFORM_N3DS
  if (textureName != "lightMap")
    return;

  sN3dsLightState.size = image.size;
  sN3dsLightState.values.clear();
  if (image.empty() || !image.data)
    return;

  size_t pixelCount = static_cast<size_t>(image.size[0]) * image.size[1];
  sN3dsLightState.values.resize(pixelCount, Vec3F::filled(1.0f));
  if (image.format == PixelFormat::RGB_F || image.format == PixelFormat::RGBA_F) {
    size_t floatsPerPixel = image.format == PixelFormat::RGB_F ? 3 : 4;
    auto source = reinterpret_cast<float const*>(image.data);
    for (size_t i = 0; i < pixelCount; ++i)
      sN3dsLightState.values[i] = Vec3F(source[i * floatsPerPixel + 0], source[i * floatsPerPixel + 1], source[i * floatsPerPixel + 2]);
  } else {
    size_t bytesPerPixelValue = bytesPerPixel(image.format);
    for (size_t i = 0; i < pixelCount; ++i) {
      auto source = image.data + i * bytesPerPixelValue;
      if (image.format == PixelFormat::BGR24 || image.format == PixelFormat::BGRA32)
        sN3dsLightState.values[i] = Vec3F(source[2], source[1], source[0]) / 255.0f;
      else
        sN3dsLightState.values[i] = Vec3F(source[0], source[1], source[2]) / 255.0f;
    }
  }
#else
  (void)textureName;
  (void)image;
#endif
}
bool N3dsStubRenderer::switchEffectConfig(String const&) { return false; }  // STUB
void N3dsStubRenderer::setScissorRect(Maybe<RectI> const& scissorRect) {
  if (scissorRect == m_scissorRect)
    return;

  sealImmediatePrimitiveBatch();
  m_scissorRect = scissorRect;
}
void N3dsStubRenderer::setSizeLimitEnabled(bool) {}                          // STUB
void N3dsStubRenderer::setMultiTexturingEnabled(bool) {}                     // STUB
void N3dsStubRenderer::setMultiSampling(unsigned) {}                         // STUB

TexturePtr N3dsStubRenderer::createTexture(Image const& texture, TextureAddressing addressing, TextureFiltering filtering) {
  return make_ref<N3dsStubTexture>(texture, filtering, addressing);
}

TextureGroupPtr N3dsStubRenderer::createTextureGroup(TextureGroupSize, TextureFiltering filtering) {
  return make_shared<N3dsStubTextureGroup>(filtering); // STUB
}

RenderBufferPtr N3dsStubRenderer::createRenderBuffer() {
  return make_shared<N3dsStubRenderBuffer>(); // STUB
}

List<RenderPrimitive>& N3dsStubRenderer::immediatePrimitives() {
  return m_immediatePrimitives;
}

void N3dsStubRenderer::render(RenderPrimitive primitive) {
  if (m_queuedPrimitiveCount + m_immediatePrimitives.size() < N3dsMaxQueuedPrimitives)
    m_immediatePrimitives.append(std::move(primitive));
}

void N3dsStubRenderer::renderBuffer(RenderBufferPtr const& renderBuffer, Mat3F const& transformation) {
  if (auto n3dsRenderBuffer = dynamic_cast<N3dsStubRenderBuffer const*>(renderBuffer.get())) {
    for (auto const& primitive : n3dsRenderBuffer->primitives()) {
      if (m_queuedPrimitiveCount + m_immediatePrimitives.size() >= N3dsMaxQueuedPrimitives)
        break;
      m_immediatePrimitives.append(transformedPrimitive(primitive, transformation));
    }
  }
}

void N3dsStubRenderer::setHandheldOverlayState(N3dsHandheldOverlayState overlayState) {
  overlayState.inWorld = m_handheldOverlayState.inWorld;
  overlayState.titleMenuActive = m_handheldOverlayState.titleMenuActive;
  overlayState.healthFill = m_handheldOverlayState.healthFill;
  overlayState.energyFill = m_handheldOverlayState.energyFill;
  overlayState.breathFill = m_handheldOverlayState.breathFill;
  m_handheldOverlayState = overlayState;
}

void N3dsStubRenderer::setHandheldGameplayState(bool inWorld, float healthFill, float energyFill, float breathFill) {
  m_handheldOverlayState.inWorld = inWorld;
  m_handheldOverlayState.healthFill = std::clamp(healthFill, 0.0f, 1.0f);
  m_handheldOverlayState.energyFill = std::clamp(energyFill, 0.0f, 1.0f);
  m_handheldOverlayState.breathFill = std::clamp(breathFill, 0.0f, 1.0f);
}

void N3dsStubRenderer::setHandheldTitleMenuState(bool active) {
  m_handheldOverlayState.titleMenuActive = active;
  if (active)
    m_handheldOverlayState.inWorld = false;
  setN3dsTitleMenuInputActive(active);
}

void N3dsStubRenderer::sealImmediatePrimitiveBatch() {
  if (m_immediatePrimitives.empty())
    return;

  size_t availablePrimitives = N3dsMaxQueuedPrimitives - std::min(m_queuedPrimitiveCount, N3dsMaxQueuedPrimitives);
  if (m_immediatePrimitives.size() > availablePrimitives)
    m_immediatePrimitives.eraseAt(availablePrimitives, m_immediatePrimitives.size());

  if (!m_immediatePrimitives.empty()) {
    m_queuedPrimitiveCount += m_immediatePrimitives.size();
    m_primitiveBatches.append(PrimitiveBatch{m_scissorRect, std::move(m_immediatePrimitives)});
  }

  m_immediatePrimitives.clear();
}

void N3dsStubRenderer::flush(Mat3F const& transformation) {
  sealImmediatePrimitiveBatch();

  if (m_primitiveBatches.empty() && m_frameCounter > 0)
    return;

#ifdef STAR_PLATFORM_N3DS
  if (m_gpuReady && m_topTarget) {
    auto* topTarget = static_cast<C3D_RenderTarget*>(m_topTarget);
    auto* bottomTarget = static_cast<C3D_RenderTarget*>(m_bottomTarget);

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    FrameReplayStats frameStats;
    frameStats.batchCount = m_primitiveBatches.size();
    C2D_TargetClear(topTarget, C2D_Color32(0, 0, 0, 255));
    C2D_SceneBegin(topTarget);
    applyScissorRect({});

    // PLACEHOLDER: first-pass primitive replay path. Axis-aligned textured
    // quads now use uploaded C2D images; other primitives fall back to solid
    // triangle replay. Scissor changes are preserved as batches for GUI
    // clipping while full texture/effect aware geometry is still partial.
    size_t primitiveCount = 0;
    for (auto const& batch : m_primitiveBatches) {
      if (batch.scissorRect)
        ++frameStats.scissoredBatches;

      applyScissorRect(batch.scissorRect);
      for (auto const& primitive : batch.primitives) {
        if (primitiveCount >= N3dsMaxQueuedPrimitives)
          break;
        drawPrimitive(transformedPrimitive(primitive, transformation), topTarget, frameStats);
        ++primitiveCount;
      }
    }
    applyScissorRect({});
    if (N3dsDrawTopDiagnostics)
      drawTopScreenDiagnostics(frameStats);

    if (bottomTarget) {
      C2D_TargetClear(bottomTarget, C2D_Color32(14, 17, 22, 255));
      C2D_SceneBegin(bottomTarget);
      drawBottomHandheldOverlay(m_handheldOverlayState, m_frameCounter);
    }

    C3D_FrameEnd(0);
    ++m_frameCounter;
  }
#endif

  // STUB: Full texture/effect aware replay is not implemented yet.
  m_immediatePrimitives.clear();
  m_primitiveBatches.clear();
  m_queuedPrimitiveCount = 0;
}

} // namespace Star

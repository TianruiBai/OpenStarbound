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

namespace Star {

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

u32 toC2dColor(Vec4B const& color) {
  return C2D_Color32(color[0], color[1], color[2], color[3]);
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
  u32 border = selected ? C2D_Color32(255, 255, 255, 255) : C2D_Color32(84, 92, 102, 255);
  C2D_DrawRectSolid(x, y, 0.0f, size, size, border);
  C2D_DrawRectSolid(x + 2.0f, y + 2.0f, 0.0f, size - 4.0f, size - 4.0f, C2D_Color32(26, 29, 34, 255));

  if (filled)
    C2D_DrawRectSolid(x + 6.0f, y + 6.0f, 0.0f, size - 12.0f, size - 12.0f, fillColor);
}

void drawBottomStatusBar(float x, float y, float width, float height, float fill, u32 color) {
  C2D_DrawRectSolid(x, y, 0.0f, width, height, C2D_Color32(58, 62, 70, 255));
  C2D_DrawRectSolid(x + 1.0f, y + 1.0f, 0.0f, width - 2.0f, height - 2.0f, C2D_Color32(18, 20, 24, 255));
  C2D_DrawRectSolid(x + 2.0f, y + 2.0f, 0.0f, std::max(0.0f, (width - 4.0f) * fill), height - 4.0f, color);
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

void drawBottomHandheldOverlay(N3dsHandheldOverlayState const& overlayState, unsigned frameCounter) {
  (void)frameCounter;

  C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, 320.0f, 240.0f, C2D_Color32(13, 16, 21, 255));
  C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, 320.0f, 36.0f, C2D_Color32(24, 29, 36, 255));
  C2D_DrawRectSolid(0.0f, 198.0f, 0.0f, 320.0f, 42.0f, C2D_Color32(24, 27, 33, 255));
  C2D_DrawRectSolid(0.0f, 36.0f, 0.0f, 320.0f, 1.0f, C2D_Color32(58, 66, 76, 255));
  C2D_DrawRectSolid(0.0f, 197.0f, 0.0f, 320.0f, 1.0f, C2D_Color32(58, 66, 76, 255));

  drawBottomStatusBar(10.0f, 7.0f, 88.0f, 7.0f, 0.78f, C2D_Color32(232, 64, 72, 255));
  drawBottomStatusBar(10.0f, 18.0f, 88.0f, 7.0f, 0.62f, C2D_Color32(72, 176, 255, 255));
  drawBottomStatusBar(108.0f, 7.0f, 48.0f, 7.0f, 0.95f, C2D_Color32(72, 220, 128, 255));
  drawBottomStatusBar(108.0f, 18.0f, 48.0f, 7.0f, overlayState.circlePadActive ? 1.0f : 0.25f, C2D_Color32(255, 224, 48, 255));
  drawBottomStartupDiagnostics(overlayState.startupDiagnostics);

  drawBottomRoundButton(210.0f, 18.0f, 7.0f, overlayState.shoulderZL, C2D_Color32(216, 120, 255, 255));
  drawBottomRoundButton(242.0f, 18.0f, 9.0f, overlayState.shoulderL, C2D_Color32(96, 224, 255, 255));
  drawBottomRoundButton(276.0f, 18.0f, 9.0f, overlayState.shoulderR, C2D_Color32(255, 224, 48, 255));
  drawBottomRoundButton(308.0f, 18.0f, 7.0f, overlayState.shoulderZR, C2D_Color32(255, 104, 160, 255));

  C2D_DrawRectSolid(12.0f, 47.0f, 0.0f, 164.0f, 30.0f, C2D_Color32(27, 32, 39, 255));
  C2D_DrawCircleSolid(28.0f, 62.0f, 0.0f, 8.0f, C2D_Color32(255, 224, 48, 255));
  C2D_DrawRectSolid(44.0f, 55.0f, 0.0f, 112.0f, 4.0f, C2D_Color32(96, 224, 255, 255));
  C2D_DrawRectSolid(44.0f, 65.0f, 0.0f, 86.0f, 4.0f, C2D_Color32(72, 220, 128, 255));

  C2D_DrawRectSolid(12.0f, 86.0f, 0.0f, 164.0f, 82.0f, C2D_Color32(20, 24, 30, 255));
  drawBottomMovementPad(60.0f, 127.0f, overlayState.circlePadActive, overlayState.dpadActive);
  C2D_DrawRectSolid(112.0f, 105.0f, 0.0f, overlayState.circlePadActive ? 44.0f : 22.0f, 5.0f, C2D_Color32(72, 220, 128, 255));
  C2D_DrawRectSolid(112.0f, 122.0f, 0.0f, overlayState.dpadActive ? 50.0f : 26.0f, 5.0f, C2D_Color32(255, 224, 48, 255));
  C2D_DrawRectSolid(112.0f, 139.0f, 0.0f, overlayState.pointerPressed ? 38.0f : 18.0f, 5.0f, C2D_Color32(96, 224, 255, 255));

  C2D_DrawRectSolid(196.0f, 86.0f, 0.0f, 112.0f, 100.0f, C2D_Color32(20, 24, 30, 255));
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
      C2D_Color32(232, 64, 72, 255), C2D_Color32(255, 224, 48, 255), C2D_Color32(72, 220, 128, 255), C2D_Color32(96, 224, 255, 255), C2D_Color32(216, 120, 255, 255),
      C2D_Color32(255, 144, 72, 255), C2D_Color32(160, 220, 96, 255), C2D_Color32(96, 144, 255, 255), C2D_Color32(255, 104, 160, 255), C2D_Color32(224, 224, 224, 255)};
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
    subTexture.bottom = bottom / static_cast<float>(m_storageHeight);
    subTexture.top = top / static_cast<float>(m_storageHeight);
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
      for (unsigned x = 0; x < image.width(); ++x) {
        size_t destinationOffset = n3dsTiledPixelIndex(x, y, storageWidth) * 4;
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
    m_subTexture.bottom = 0.0f;
    m_subTexture.top = static_cast<float>(image.height()) / static_cast<float>(storageHeight);
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
    m_primitives = primitives;
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
      a.screenCoordinate[0], toTopScreenY(a.screenCoordinate[1]), toC2dColor(a.color),
      b.screenCoordinate[0], toTopScreenY(b.screenCoordinate[1]), toC2dColor(b.color),
      c.screenCoordinate[0], toTopScreenY(c.screenCoordinate[1]), toC2dColor(c.color),
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

  if (isOpaqueWhite(quad.a.color))
    return C2D_DrawImage(image, &params, nullptr);

  C2D_PlainImageTint(&tint, toC2dColor(quad.a.color), 1.0f);
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
void N3dsStubRenderer::setEffectParameter(String const&, RenderEffectParameter const&) {}       // STUB
void N3dsStubRenderer::setEffectScriptableParameter(String const&, String const&, RenderEffectParameter const&) {} // STUB

Maybe<RenderEffectParameter> N3dsStubRenderer::getEffectScriptableParameter(String const&, String const&) {
  return {}; // STUB
}

Maybe<VariantTypeIndex> N3dsStubRenderer::getEffectScriptableParameterType(String const&, String const&) {
  return {}; // STUB
}

void N3dsStubRenderer::setEffectTexture(String const&, ImageView const&) {} // STUB
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
  m_handheldOverlayState = overlayState;
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

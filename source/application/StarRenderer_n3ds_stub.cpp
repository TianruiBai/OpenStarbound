// STUB/PLACEHOLDER: Nintendo 3DS renderer backend with a minimal citro-backed
// frame path for Phase 3 bring-up. Most RenderPrimitive paths are still
// placeholders.

#include "StarRenderer_n3ds_stub.hpp"
#include "StarLogging.hpp"

#include <algorithm>

#ifdef STAR_PLATFORM_N3DS
#include <citro2d.h>
#include <citro3d.h>
#endif

namespace Star {

// ---- Stub Texture -------------------------------------------------------

namespace {

#ifdef STAR_PLATFORM_N3DS
u32 toC2dColor(Vec4B const& color) {
  return C2D_Color32(color[0], color[1], color[2], color[3]);
}

inline float toTopScreenY(float yBottomOrigin) {
  constexpr float TopScreenHeight = 240.0f;
  return TopScreenHeight - yBottomOrigin;
}

void drawPrimitivePlaceholder(RenderPrimitive const& primitive, C3D_RenderTarget* target) {
  (void)target;

  if (auto tri = primitive.ptr<RenderTriangle>()) {
    C2D_DrawTriangle(
        tri->a.screenCoordinate[0], toTopScreenY(tri->a.screenCoordinate[1]), toC2dColor(tri->a.color),
        tri->b.screenCoordinate[0], toTopScreenY(tri->b.screenCoordinate[1]), toC2dColor(tri->b.color),
        tri->c.screenCoordinate[0], toTopScreenY(tri->c.screenCoordinate[1]), toC2dColor(tri->c.color),
        0.0f);
  } else if (auto quad = primitive.ptr<RenderQuad>()) {
    C2D_DrawTriangle(
        quad->a.screenCoordinate[0], toTopScreenY(quad->a.screenCoordinate[1]), toC2dColor(quad->a.color),
        quad->b.screenCoordinate[0], toTopScreenY(quad->b.screenCoordinate[1]), toC2dColor(quad->b.color),
        quad->c.screenCoordinate[0], toTopScreenY(quad->c.screenCoordinate[1]), toC2dColor(quad->c.color),
        0.0f);
    C2D_DrawTriangle(
        quad->a.screenCoordinate[0], toTopScreenY(quad->a.screenCoordinate[1]), toC2dColor(quad->a.color),
        quad->c.screenCoordinate[0], toTopScreenY(quad->c.screenCoordinate[1]), toC2dColor(quad->c.color),
        quad->d.screenCoordinate[0], toTopScreenY(quad->d.screenCoordinate[1]), toC2dColor(quad->d.color),
        0.0f);
  } else if (auto poly = primitive.ptr<RenderPoly>()) {
    if (poly->vertexes.size() >= 3) {
      auto const& a = poly->vertexes[0];
      for (size_t i = 1; i + 1 < poly->vertexes.size(); ++i) {
        auto const& b = poly->vertexes[i];
        auto const& c = poly->vertexes[i + 1];
        C2D_DrawTriangle(
            a.screenCoordinate[0], toTopScreenY(a.screenCoordinate[1]), toC2dColor(a.color),
            b.screenCoordinate[0], toTopScreenY(b.screenCoordinate[1]), toC2dColor(b.color),
            c.screenCoordinate[0], toTopScreenY(c.screenCoordinate[1]), toC2dColor(c.color),
            0.0f);
      }
    }
  }
}
#endif

class N3dsStubTexture : public Texture {
public:
  N3dsStubTexture(Vec2U size, TextureFiltering filtering, TextureAddressing addressing)
    : m_size(size), m_filtering(filtering), m_addressing(addressing) {}

  Vec2U size() const override { return m_size; }
  TextureFiltering filtering() const override { return m_filtering; }
  TextureAddressing addressing() const override { return m_addressing; }

private:
  Vec2U m_size;
  TextureFiltering m_filtering;
  TextureAddressing m_addressing;
};

class N3dsStubTextureGroup : public TextureGroup {
public:
  explicit N3dsStubTextureGroup(TextureFiltering filtering) : m_filtering(filtering) {}

  TextureFiltering filtering() const override { return m_filtering; }
  TexturePtr create(Image const& texture) override {
    return make_ref<N3dsStubTexture>(texture.size(), m_filtering, TextureAddressing::Clamp);
  }

private:
  TextureFiltering m_filtering;
};

class N3dsStubRenderBuffer : public RenderBuffer {
public:
  void set(List<RenderPrimitive>&) override {} // STUB: discard
};

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
  // PLACEHOLDER: return top-screen size until dual-screen target split is implemented.
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
void N3dsStubRenderer::setScissorRect(Maybe<RectI> const&) {}               // STUB
void N3dsStubRenderer::setSizeLimitEnabled(bool) {}                          // STUB
void N3dsStubRenderer::setMultiTexturingEnabled(bool) {}                     // STUB
void N3dsStubRenderer::setMultiSampling(unsigned) {}                         // STUB

TexturePtr N3dsStubRenderer::createTexture(Image const& texture, TextureAddressing addressing, TextureFiltering filtering) {
  // STUB: allocate a size-tracking stub texture with no GPU upload.
  return make_ref<N3dsStubTexture>(texture.size(), filtering, addressing);
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
  m_immediatePrimitives.append(std::move(primitive));
}
void N3dsStubRenderer::renderBuffer(RenderBufferPtr const&, Mat3F const&) {} // STUB: discard
void N3dsStubRenderer::flush(Mat3F const&) {
#ifdef STAR_PLATFORM_N3DS
  if (m_gpuReady && m_topTarget) {
    auto* topTarget = static_cast<C3D_RenderTarget*>(m_topTarget);
    auto* bottomTarget = static_cast<C3D_RenderTarget*>(m_bottomTarget);

    // PLACEHOLDER: minimal visible output while the full primitive path is
    // still stubbed.
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    bool flashPhase = (m_frameCounter / 30) % 2 == 0;
    u32 clearColor = flashPhase ? C2D_Color32(255, 24, 180, 255) : C2D_Color32(24, 220, 255, 255);
    C2D_TargetClear(topTarget, clearColor);
    C2D_SceneBegin(topTarget);
    C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, 400.0f, 18.0f, C2D_Color32(255, 255, 255, 255));
    C2D_DrawRectSolid(0.0f, 222.0f, 0.0f, 400.0f, 18.0f, C2D_Color32(0, 0, 0, 255));
    C2D_DrawRectSolid(24.0f, 96.0f, 0.0f, 352.0f, 48.0f, flashPhase ? C2D_Color32(0, 0, 0, 255) : C2D_Color32(255, 255, 255, 255));

    // PLACEHOLDER: first-pass primitive replay path. We currently draw
    // primitive bounds as solid blocks until full textured geometry is wired.
    for (auto const& primitive : m_immediatePrimitives)
      drawPrimitivePlaceholder(primitive, topTarget);

    if (bottomTarget) {
      C2D_TargetClear(bottomTarget, flashPhase ? C2D_Color32(32, 255, 96, 255) : C2D_Color32(255, 220, 24, 255));
      C2D_SceneBegin(bottomTarget);
      C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, 320.0f, 20.0f, C2D_Color32(0, 0, 0, 255));
      C2D_DrawRectSolid(0.0f, 220.0f, 0.0f, 320.0f, 20.0f, C2D_Color32(255, 255, 255, 255));
      C2D_DrawRectSolid(48.0f, 72.0f, 0.0f, 224.0f, 96.0f, flashPhase ? C2D_Color32(255, 255, 255, 255) : C2D_Color32(0, 0, 0, 255));
    }

    C3D_FrameEnd(0);
    ++m_frameCounter;
  }
#endif

  // STUB: Full texture/effect aware replay is not implemented yet.
  m_immediatePrimitives.clear();
}

} // namespace Star

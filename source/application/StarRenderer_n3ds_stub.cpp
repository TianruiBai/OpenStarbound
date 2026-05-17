// STUB/PLACEHOLDER: Nintendo 3DS phase1 renderer backend.
// All draw calls are accepted and discarded.  No GPU output occurs.
// Phase 3 will replace this with a citro3d-backed implementation.

#include "StarRenderer_n3ds_stub.hpp"
#include "StarLogging.hpp"

namespace Star {

// ---- Stub Texture -------------------------------------------------------

namespace {

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
  Logger::info("N3dsStubRenderer: phase1 stub renderer initialised (no GPU output)"); // STUB
}

N3dsStubRenderer::~N3dsStubRenderer() {}

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

void N3dsStubRenderer::render(RenderPrimitive) {}                          // STUB: discard
void N3dsStubRenderer::renderBuffer(RenderBufferPtr const&, Mat3F const&) {} // STUB: discard
void N3dsStubRenderer::flush(Mat3F const&) {
  m_immediatePrimitives.clear(); // STUB: drop frame
}

} // namespace Star

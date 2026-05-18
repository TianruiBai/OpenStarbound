// STUB/PLACEHOLDER: Nintendo 3DS renderer backend with a minimal citro-backed
// frame path for Phase 3 bring-up. Most RenderPrimitive paths are still
// placeholders.

#include "StarRenderer_n3ds_stub.hpp"
#include "StarLogging.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#ifdef STAR_PLATFORM_N3DS
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#endif

namespace Star {

// ---- Stub Texture -------------------------------------------------------

namespace {

constexpr size_t N3dsMaxQueuedPrimitives = 768;

#ifdef STAR_PLATFORM_N3DS
constexpr int N3dsTopScreenWidth = 400;
constexpr int N3dsTopScreenHeight = 240;

u32 toC2dColor(Vec4B const& color) {
  return C2D_Color32(color[0], color[1], color[2], color[3]);
}

inline float toTopScreenY(float yBottomOrigin) {
  return static_cast<float>(N3dsTopScreenHeight) - yBottomOrigin;
}

unsigned n3dsTextureExtent(unsigned value) {
  unsigned extent = 8;
  while (extent < value)
    extent <<= 1;
  return extent;
}

bool nearlyEqual(float a, float b) {
  return std::fabs(a - b) < 0.01f;
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
#endif

private:
#ifdef STAR_PLATFORM_N3DS
  void uploadImage(Image const& image) {
    if (image.empty())
      return;

    unsigned storageWidth = n3dsTextureExtent(image.width());
    unsigned storageHeight = n3dsTextureExtent(image.height());
    constexpr unsigned MaxTextureExtent = 512;
    constexpr size_t TextureUploadBudget = 2 * 1024 * 1024;
    if (storageWidth > MaxTextureExtent || storageHeight > MaxTextureExtent) {
      Logger::warn("N3dsStubTexture: skipping oversized texture {}x{}", image.width(), image.height());
      return;
    }

    size_t textureBytes = static_cast<size_t>(storageWidth) * storageHeight * 4;
    if (n3dsTextureBytesInUse() + textureBytes > TextureUploadBudget) {
      Logger::warn("N3dsStubTexture: skipping texture {}x{} because upload budget is exhausted", image.width(), image.height());
      return;
    }

    Image rgbaImage = image.pixelFormat() == PixelFormat::RGBA32 ? image : image.convert(PixelFormat::RGBA32);
    std::vector<uint8_t> uploadData(textureBytes, 0);
    for (unsigned y = 0; y < image.height(); ++y) {
      auto const* source = rgbaImage.data() + static_cast<size_t>(y) * image.width() * 4;
      auto* destination = uploadData.data() + (static_cast<size_t>(y) * storageWidth * 4);
      std::memcpy(destination, source, static_cast<size_t>(image.width()) * 4);
    }

    if (!C3D_TexInit(&m_texture, static_cast<u16>(storageWidth), static_cast<u16>(storageHeight), GPU_RGBA8)) {
      Logger::warn("N3dsStubTexture: C3D_TexInit failed for {}x{}", image.width(), image.height());
      return;
    }

    void* linearUploadData = linearAlloc(uploadData.size());
    if (!linearUploadData) {
      C3D_TexDelete(&m_texture);
      Logger::warn("N3dsStubTexture: linearAlloc failed for {} byte upload", uploadData.size());
      return;
    }

    std::memcpy(linearUploadData, uploadData.data(), uploadData.size());
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
  if (!texture || !texture->ready() || !isAxisAlignedQuad(quad))
    return false;

  float minX = std::min(std::min(quad.a.screenCoordinate[0], quad.b.screenCoordinate[0]), std::min(quad.c.screenCoordinate[0], quad.d.screenCoordinate[0]));
  float maxX = std::max(std::max(quad.a.screenCoordinate[0], quad.b.screenCoordinate[0]), std::max(quad.c.screenCoordinate[0], quad.d.screenCoordinate[0]));
  float minY = std::min(std::min(quad.a.screenCoordinate[1], quad.b.screenCoordinate[1]), std::min(quad.c.screenCoordinate[1], quad.d.screenCoordinate[1]));
  float maxY = std::max(std::max(quad.a.screenCoordinate[1], quad.b.screenCoordinate[1]), std::max(quad.c.screenCoordinate[1], quad.d.screenCoordinate[1]));

  float width = maxX - minX;
  float height = maxY - minY;
  if (width <= 0.0f || height <= 0.0f)
    return true;

  C2D_DrawParams params = {{minX, toTopScreenY(maxY), width, height}, {0.0f, 0.0f}, 0.0f, 0.0f};
  C2D_ImageTint tint;
  C2D_PlainImageTint(&tint, toC2dColor(quad.a.color), 1.0f);
  return C2D_DrawImage(texture->image(), &params, &tint);
}

void drawPrimitive(RenderPrimitive const& primitive, C3D_RenderTarget* target) {
  (void)target;

  if (auto tri = primitive.ptr<RenderTriangle>()) {
    drawUntexturedTriangle(tri->a, tri->b, tri->c);
  } else if (auto quad = primitive.ptr<RenderQuad>()) {
    if (drawTexturedQuad(*quad))
      return;

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
    applyScissorRect({});
    C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, 400.0f, 18.0f, C2D_Color32(255, 255, 255, 255));
    C2D_DrawRectSolid(0.0f, 222.0f, 0.0f, 400.0f, 18.0f, C2D_Color32(0, 0, 0, 255));
    C2D_DrawRectSolid(24.0f, 96.0f, 0.0f, 352.0f, 48.0f, flashPhase ? C2D_Color32(0, 0, 0, 255) : C2D_Color32(255, 255, 255, 255));

    // PLACEHOLDER: first-pass primitive replay path. Axis-aligned textured
    // quads now use uploaded C2D images; other primitives fall back to solid
    // triangle replay. Scissor changes are preserved as batches for GUI
    // clipping while full texture/effect aware geometry is still partial.
    size_t primitiveCount = 0;
    for (auto const& batch : m_primitiveBatches) {
      applyScissorRect(batch.scissorRect);
      for (auto const& primitive : batch.primitives) {
        if (primitiveCount >= N3dsMaxQueuedPrimitives)
          break;
        drawPrimitive(transformedPrimitive(primitive, transformation), topTarget);
        ++primitiveCount;
      }
    }
    applyScissorRect({});

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
  m_primitiveBatches.clear();
  m_queuedPrimitiveCount = 0;
}

} // namespace Star

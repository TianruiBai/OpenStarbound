#pragma once

// STUB/PLACEHOLDER: Nintendo 3DS renderer in transition from no-op stub to
// a minimal citro-backed frame path for Phase 3 bring-up.

#include "StarRenderer.hpp"

namespace Star {

STAR_CLASS(N3dsStubRenderer);

class N3dsStubRenderer : public Renderer {
public:
  N3dsStubRenderer();
  ~N3dsStubRenderer() override;

  String rendererId() const override;
  Vec2U screenSize() const override;

  void loadConfig(Json const& config) override;
  void loadEffectConfig(String const& name, Json const& effectConfig, StringMap<String> const& shaders) override;

  void setEffectParameter(String const& parameterName, RenderEffectParameter const& parameter) override;
  void setEffectScriptableParameter(String const& effectName, String const& parameterName, RenderEffectParameter const& parameter) override;
  Maybe<RenderEffectParameter> getEffectScriptableParameter(String const& effectName, String const& parameterName) override;
  Maybe<VariantTypeIndex> getEffectScriptableParameterType(String const& effectName, String const& parameterName) override;
  void setEffectTexture(String const& textureName, ImageView const& image) override;
  bool switchEffectConfig(String const& name) override;

  void setScissorRect(Maybe<RectI> const& scissorRect) override;

  TexturePtr createTexture(Image const& texture, TextureAddressing addressing, TextureFiltering filtering) override;
  void setSizeLimitEnabled(bool enabled) override;
  void setMultiTexturingEnabled(bool enabled) override;
  void setMultiSampling(unsigned multiSampling) override;
  TextureGroupPtr createTextureGroup(TextureGroupSize size, TextureFiltering filtering) override;
  RenderBufferPtr createRenderBuffer() override;

  List<RenderPrimitive>& immediatePrimitives() override;
  void render(RenderPrimitive primitive) override;
  void renderBuffer(RenderBufferPtr const& renderBuffer, Mat3F const& transformation) override;
  void flush(Mat3F const& transformation) override;

private:
  // PLACEHOLDER: Renderer API still reports the top screen while the backend
  // owns separate top/bottom citro targets internally.
  static constexpr unsigned N3DS_TOP_SCREEN_WIDTH  = 400;
  static constexpr unsigned N3DS_TOP_SCREEN_HEIGHT = 240;

  List<RenderPrimitive> m_immediatePrimitives;

  bool m_gpuReady = false;
  void* m_topTarget = nullptr;
  void* m_bottomTarget = nullptr;
  unsigned m_frameCounter = 0;
};

}

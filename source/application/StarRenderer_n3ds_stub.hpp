#pragma once

// STUB: Nintendo 3DS phase1 renderer — satisfies the Renderer interface with no-op
// implementations.  Real citro3d-backed rendering is Phase 3 work.

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
  // PLACEHOLDER: Real N3DS display resolution is 400x240 (top) / 320x240 (bottom).
  // Phase 3 will split this into dual-screen render targets.
  static constexpr unsigned N3DS_TOP_SCREEN_WIDTH  = 400;
  static constexpr unsigned N3DS_TOP_SCREEN_HEIGHT = 240;

  List<RenderPrimitive> m_immediatePrimitives;
};

}

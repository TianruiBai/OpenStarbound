#pragma once

// STUB/PLACEHOLDER: Nintendo 3DS renderer in transition from no-op stub to
// a minimal citro-backed frame path for Phase 3 bring-up.

#include "StarRenderer.hpp"

namespace Star {

STAR_CLASS(N3dsStubRenderer);

struct N3dsHandheldOverlayState {
  Vec2F pointerPosition = {200.0f, 120.0f};
  Vec2F touchPosition = {160.0f, 120.0f};
  unsigned selectedHotbarSlot = 0;
  unsigned selectedTitleMenuItem = 0;
  float healthFill = 1.0f;
  float energyFill = 1.0f;
  float breathFill = 1.0f;
  bool inWorld = false;
  bool titleMenuActive = false;
  // Title sub-state for bottom screen context:
  // 0=main menu, 1=char select, 2=char creation, 3=options, 4=mods
  unsigned titleSubState = 0;
  bool pointerPressed = false;
  bool touchPressed = false;
  bool circlePadActive = false;
  bool dpadActive = false;
  bool buttonA = false;
  bool buttonB = false;
  bool buttonX = false;
  bool buttonY = false;
  bool shoulderL = false;
  bool shoulderR = false;
  bool shoulderZL = false;
  bool shoulderZR = false;
  uint32_t startupDiagnostics = 0;
};

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

  void setHandheldOverlayState(N3dsHandheldOverlayState overlayState);
  void setHandheldGameplayState(bool inWorld, float healthFill, float energyFill, float breathFill);
  void setHandheldTitleMenuState(bool active);
  void setHandheldTitleSubState(unsigned subState);
  // Pre-load bottom-screen textures from the Starbound-packed assets so the
  // bottom screen can draw real images instead of coloured rectangles.
  // Caller provides Images keyed by logical name:
  //   "singleplayer", "multiplayer", "options", "exit" — title menu buttons
  //   "heart", "energy" — HUD status icons
  //   "hotbar" — action bar background
  // Pass an empty Image for any unavailable texture; drawing falls back to
  // coloured shapes.
  void preloadN3dsBottomTextures(StringMap<Image> const& images);

  // Legacy entry point kept for compatibility; delegates to preloadN3dsBottomTextures.
  void preloadN3dsTitleMenuTextures(
      Image const& imgSinglePlayer,
      Image const& imgMultiplayer,
      Image const& imgOptions,
      Image const& imgExit);

private:
  struct PrimitiveBatch {
    Maybe<RectI> scissorRect;
    List<RenderPrimitive> primitives;
  };

  void sealImmediatePrimitiveBatch();

  // PLACEHOLDER: Renderer API still reports the top screen while the backend
  // owns separate top/bottom citro targets internally.
  static constexpr unsigned N3DS_TOP_SCREEN_WIDTH  = 400;
  static constexpr unsigned N3DS_TOP_SCREEN_HEIGHT = 240;

  Maybe<RectI> m_scissorRect;
  List<RenderPrimitive> m_immediatePrimitives;
  List<PrimitiveBatch> m_primitiveBatches;
  size_t m_queuedPrimitiveCount = 0;

  bool m_gpuReady = false;
  void* m_topTarget = nullptr;
  void* m_bottomTarget = nullptr;
  unsigned m_frameCounter = 0;
  N3dsHandheldOverlayState m_handheldOverlayState;
};

void setN3dsTitleMenuInputActive(bool active);
bool n3dsTitleMenuInputActive();

}

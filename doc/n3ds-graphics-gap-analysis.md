# OpenStarbound N3DS Graphics Gap Analysis

Status: February 2025 (phase1-client build)
Renderer class: `N3dsStubRenderer` (source/application/StarRenderer_n3ds_stub.hpp/cpp)

---

## 1. What Works Today

### 1.1 GPU Backend
- Citro2D/Citro3D (`C2D_*` / `C3D_*`) on both screens:
  - Top: GFX_TOP 400×240, bottom: GFX_BOTTOM 320×240
- Single-buffered `C3D_FrameBegin`/`C3D_FrameEnd` with `gspWaitForVBlank`

### 1.2 Texture Pipeline
- RGBA32 → 3DS-native GPU_RGBA8 (ABGR byte order) in Morton tile order
- 6 MB upload budget with safety skip
- mip-maps: not generated; filter = GPU_LINEAR or GPU_NEAREST
- wrap = GPU_CLAMP_TO_EDGE or GPU_REPEAT
- Sub-texture region extraction (`imageForQuad`) with clamped texcoords
- Pre-loaded title-menu textures (4× PNG → `C3D_Tex`)

### 1.3 Primitive Rendering
- Untextured triangles (`C2D_DrawTriangle`) with per-vertex lighting
- Untextured quads (two triangles)
- Untextured polygon fan
- Axis-aligned textured quads (`C2D_DrawImage`, `C2D_PlainImageTint` for tint)
- Scissor clip (GPU_SCISSOR_NORMAL) preserved per batch

### 1.4 Lighting
- Single light map via `setEffectTexture("lightMap", ...)`
- Per-vertex lighting multiplication (`applyN3dsLighting`) based on `param1`
- Light map scale/offset/multiplier parameters

### 1.5 Bottom-Screen Overlay (handheld mode)
- Title menu: 4-button layout with real PNG textures (fallback to colored rects)
- In-world HUD: health/energy/breath bars, 10-slot hotbar, tool icons, movement pad, A/B/X/Y face buttons, L/R/ZL/ZR shoulders, mission card, pointer/touch cursors, startup diagnostic beads
- C2D_TextBuf text with system font

### 1.6 Framerate & Timing
- Fixed-timestep loop: update at 60 Hz, render at 30 fps
- Frame skip: max 5 updates per tick
- `gspWaitForVBlank` after each render

### 1.7 Audio
- ndsp primary with csnd fallback
- Stereo 16-bit 32 kHz PCM
- Triple-buffered streaming

### 1.8 Input
- Full key/button mapping (D-Pad, Circle Pad, A/B/X/Y, L/R/ZL/ZR, C-Stick pointer)
- Touch → mouse events with hotbar/title-menu hit testing

---

## 2. Renderer API Stubs (methods that accept calls but do nothing)

| Method | Status | Notes |
|---|---|---|
| `loadConfig` | STUB | No effect config loading |
| `loadEffectConfig` | STUB | No effect program loading or compilation |
| `setEffectScriptableParameter` | STUB | No scriptable param support |
| `getEffectScriptableParameter` | STUB | Always returns `{}` |
| `getEffectScriptableParameterType` | STUB | Always returns `{}` |
| `switchEffectConfig` | STUB | Always returns `false` |
| `setSizeLimitEnabled` | STUB | Ignored |
| `setMultiTexturingEnabled` | STUB | Ignored |
| `setMultiSampling` | STUB | Ignored |
| `setCursorHardware` | STUB | Ignored |
| `setAcceptingTextInput` | STUB | Ignored |
| `setTextArea` | STUB | Ignored |
| `openAudioInputDevice` | STUB | Always returns `false` |
| `closeAudioInputDevice` | STUB | Always returns `false` |

---

## 3. Major Missing Features

### 3.1 Effect/Shader System (critical)
Starbound's desktop renderer uses GLSL shaders loaded from `.effect` config files. The N3DS renderer has no shader compilation, no effect programs, and no render-to-texture pass architecture.

**Impact:** No bloom, no color grading, no distortion effects, no screen-space effects. The game will look flat with no post-processing.

**What's needed:**
- C3D shader compiler (DVLB/DVLE from `.vsh`/`.gsh` source or pre-compiled `.shbin`)
- Effect program registry matching `loadEffectConfig` contract
- Framebuffer/render-texture allocation per effect
- Render pass scheduling multi-pass effects (bloom needs downscale → blur → composite)

### 3.2 Text Rendering (critical)
Only the bottom screen uses `C2D_TextBuf` with the system font. The top screen has no text rendering path at all. Starbound's UI is text-heavy (menus, tooltips, chat, quest log).

**Impact:** Top screen shows only colored geometry; no text labels visible.

**What's needed:**
- Font atlas loading (`.ttf` → bitmap glyph cache)
- Text layout with Unicode support
- Top-screen text rendering integrated with the primitive replay path

### 3.3 Atlas / Sprite Batching (important)
The current textured-quad path creates one draw call per quad with `C2D_DrawImage`. On desktop, sprites from texture atlases are batched efficiently.

**Impact:** Poor GPU utilization; each textured quad is a separate GPU submission.

**What's needed:**
- Sprite atlas packing (pack many small textures into one GPU texture)
- Vertex buffer batching for sprites sharing the same atlas
- Automatic atlas generation from commonly-requested images

### 3.4 Non-Axis-Aligned Textured Quads (important)
`drawTexturedQuad()` only handles axis-aligned quads. Starbound's world rendering uses arbitrary-rotation textured quads for entity sprites.

**Impact:** Most world sprites are skipped entirely (only colored geometry renders).

**What's needed:**
- Arbitrary quad UV mapping for `C2D_DrawImage` (the `params` struct supports rotation)
- The code path exists but is untested and may have coordinate issues

### 3.5 World / Tile Rendering (critical)
There is no tile-map or world-chunk rendering path. All rendering goes through the generic `RenderPrimitive` list.

**What's needed:**
- Tile atlas with material-color tinting
- Chunk-based culling (only render visible sectors)
- Layer sorting (background → tiles → foreground → entities → liquids → overlay)
- Parallax background rendering

### 3.6 UI System Rendering (critical)
Starbound's UI uses a retained widget tree that emits primitives. The N3DS renderer has no awareness of UI widgets.

**Impact:** UI is unusable on the top screen.

**What's needed:**
- UI pane rendering with 9-slice scaling
- Button/label/text input rendering
- Scroll region clip support
- Inventory/crafting grid rendering

### 3.7 Render-to-Texture / Framebuffer Objects
No GPU render target support beyond the screen.

**Impact:** Cannot implement bloom, reflections, minimap, or any render-to-texture effect.

**What's needed:**
- C3D render target creation
- Render-to-texture pass
- Texture readback (for screenshots or minimap)

### 3.8 Texture Atlas & Group Size
`N3dsStubTextureGroup` ignores the `TextureGroupSize` parameter and always uses 1024×1024 max.

**What's needed:**
- Proper sized texture atlas allocation
- Atlas page management and eviction

### 3.9 RenderBuffer Optimization
`N3dsStubRenderBuffer` is a plain `List<RenderPrimitive>`. On desktop, render buffers are GPU-resident vertex buffers.

**What's needed:**
- GPU vertex buffer allocation
- Indexed triangle strips for quad rendering
- Buffer reuse across frames

### 3.10 Multi-Sampling (MSAA)
Not implemented.

**What's needed:**
- MSAA render target configuration (3DS GPU supports 2× MSAA)
- Resolve pass

### 3.11 Texture Format Support
Only GPU_RGBA8 is supported.

**What's needed:**
- Compressed formats (ETC1 for RGB, ETC1A4 for RGBA) to save VRAM
- 16-bit formats for normal maps
- Grayscale formats for font atlases

---

## 4. Architecture Gaps

### 4.1 No Dual-Screen Renderer Contract
`screenSize()` returns only top-screen dimensions (400×240). The renderer API has no concept of a second screen.

**What's needed:**
- Extend `Renderer` with `screenCount()` / `screenSize(int screen)`
- Per-screen `flush()` or screen-select state
- Client code must know about bottom-screen overlay rendering

### 4.2 Immediate-Mode Only
All rendering is immediate-mode through `m_immediatePrimitives`. No retained-mode batching with GPU-resident buffers.

**What's needed:**
- Sprite batch manager
- Tile chunk vertex cache
- Dirty-region tracking for partial redraws

### 4.3 No Asynchronous Texture Loading
Textures are loaded synchronously in `N3dsStubTexture` constructor. Starbound's desktop renderer uses `queueImages` for background loading.

**What's needed:**
- Deferred texture upload queue
- Stub texture placeholder while loading
- Budget-aware prioritization

### 4.4 No Depth/Stencil
3DS GPU supports depth/stencil but the renderer doesn't use it.

**What's needed:**
- Depth buffer for 3D effects (unlikely needed for Starbound)
- Stencil for UI clipping paths

---

## 5. Dead / Unreachable Code

### 5.1 `runN3dsFramebufferVisibilityProbe()`
Only runs with `--n3ds-graphics-probes` flag. Direct framebuffer writes bypassing citro2d. Useful for debugging but dead in normal operation.

### 5.2 `runN3dsCitroVisibilityProbe()`
Only runs with `--n3ds-graphics-probes` flag. Injects test primitives for capture validation. Dead in normal operation.

### 5.3 `writeN3dsFramebufferProbe()`
Called only from the visibility probes.

### 5.4 Top-Screen Diagnostic Overlay
Controlled by `N3dsDrawTopDiagnostics` (hardcoded `false`). Shows primitive count, textured quad count, batch count bars. Useful for profiling but shipping builds should have it off.

---

## 6. Priority Roadmap

### Phase A — Get the Game Playable (top screen visible)
1. **Text rendering** on top screen (font atlas + C2D text)
2. **Arbitrary textured quads** (fix rotation support in `drawTexturedQuad`)
3. **UI widget rendering** (9-slice, buttons, scroll regions)

### Phase B — Visual Quality
4. **Effect system** (shader loading, bloom, color grading)
5. **Render-to-texture** (framebuffer objects, multi-pass effects)
6. **Sprite atlas batching** (pack sprites, batch draw calls)

### Phase C — Polish & Performance
7. **MSAA** (2× anti-aliasing)
8. **Texture compression** (ETC1/ETC1A4)
9. **RenderBuffer GPU optimization** (vertex/index buffers)
10. **Asynchronous texture loading** (background upload with placeholders)
11. **Dual-screen renderer contract** (proper multi-screen API)

### Non-Priority (not needed for Starbound)
- 3D rendering
- Hardware cursor
- Audio input
- Clipboard integration

---

## 7. Physical Constraints

| Resource | Limit | Currently Used |
|---|---|---|
| GPU command buffer | C3D_DEFAULT_CMDBUF_SIZE (0x40000) | Default |
| C2D max objects | C2D_DEFAULT_MAX_OBJECTS (4096) | Default |
| Texture upload budget | 6 MB | Tracked dynamically |
| Max queued primitives | 768 | Enforced |
| Max texture extent | 1024×1024 | Enforced |
| App heap | 64 MB | ~11 MB during asset load |
| Linear heap | 8 MB | Textures use linear alloc |
| FCRAM (New3DS) | 256 MB total | ~75 MB used |
| VRAM | 6 MB (within FCRAM) | GPU textures |
| Screen resolution | 400×240 (top) + 320×240 (bottom) | Fixed |

---

## 8. Notes on "Stub" Naming

Despite the `_stub` filename suffix, the renderer is **not** a no-op stub. It is a functional citro2d/citro3d backend with texture, lighting, scissor, and bottom-screen overlay support. The "stub" label was appropriate during initial bring-up but is now misleading. Consider renaming to `StarRenderer_n3ds_citro` once the Phase A items above are completed.

---

*Generated from source code audit against commit ~a9eb7ff (phase1-client build).*

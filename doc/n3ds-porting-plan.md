# OpenStarbound N3DS Porting Plan

Version: Phase 1 Client — February 2025
Last verified run: verify36 (booted to title screen, 159+ frames at 60fps, ~11MB heap)

---

## 1. Current Status

### 1.1 What Works

| Component | Status | Notes |
|---|---|---|
| Build system | ✅ | CMake + Ninja + devkitARM; produces `dist/starbound` ELF and repacked CXI |
| Boot | ⚠️ | Intermittent; verify36 reached title screen; recent builds crash during asset init |
| Asset loading | ✅ | Lazy descriptor loading eliminates 50K eager allocations; ~6.4s load time |
| Packed.pak index | ✅ | 6.5MB index kept in memory; O(1) lazy lookups via HashMap |
| GPU rendering (top) | ✅ | Citro2D/Citro3D primitives; scissor clips; textured quads |
| GPU rendering (bottom) | ✅ | Full handheld overlay: title menu, HUD, hotbar, input viz |
| Title menu textures | ✅ | PNG → RGBA8 → Morton tile → C3D_Tex; 4 real button textures |
| 30fps framerate lock | ✅ | Fixed-timestep loop with accumulator + frame skip |
| Audio output | ✅ | ndsp primary (32kHz stereo) + csnd fallback |
| Input mapping | ✅ | Full key/button/Circle Pad/C-Stick/touch → Starbound input events |
| Bottom-screen touch UI | ✅ | Title menu item selection, hotbar slot selection, quick action buttons |
| Light map effect | ✅ | Single light map via setEffectTexture, per-vertex lighting |
| SDMC file logging | ✅ | Debug and crash diagnosis via starbound_n3ds.log |
| Memory budget | ✅ | 64MB app heap + 8MB linear heap; <12MB used during asset loading |

### 1.2 What's Missing (See doc/n3ds-graphics-gap-analysis.md for details)

| Feature | Severity | Blocking |
|---|---|---|
| Top-screen text rendering | Critical | Title screen / UI text invisible |
| Arbitrary-rotation textured quads | Critical | World sprites not rendered |
| Effect/shader system | High | No bloom, color grading, post-processing |
| UI widget rendering | Critical | Menus unusable on top screen |
| Sprite atlas batching | High | Poor GPU utilization |
| Render-to-texture | High | No multi-pass effects, no minimap |
| World/tile rendering | Critical | No actual gameplay rendering |
| Font atlas loading | High | Depends on text rendering |
| Texture compression | Medium | VRAM savings |
| MSAA | Medium | Visual quality |
| Async texture loading | Medium | Loading performance |
| Dual-screen renderer API | Medium | Clean multi-screen support |

---

## 2. Architecture Overview

```
N3DS Hardware
├── GPU (PICA200) ← Citro3D / Citro2D
│   ├── Top screen (400×240)   → Game viewport + UI
│   └── Bottom screen (320×240) → Handheld overlay / touch UI
├── FCRAM 256MB (New3DS)
│   ├── App heap 64MB
│   └── Linear heap 8MB (GPU textures)
├── CPU ARM11 MPCore 804MHz (New3DS)
├── Audio DSP ← ndsp / csnd
└── Storage
    ├── RomFS (read-only, embedded in CXI)
    └── SDMC (read/write, logs + mods + saves)
```

### 2.1 Key Source Files

| File | Purpose |
|---|---|
| `source/application/StarMainApplication_n3ds_stub.cpp` | Main loop, input, audio, startup |
| `source/application/StarRenderer_n3ds_stub.hpp/cpp` | GPU rendering backend |
| `source/base/StarAssets.hpp/cpp` | Asset system with lazy loading |
| `source/base/StarPackedAssetSource.hpp/cpp` | Packed.pak reader with O(1) lookup |
| `source/core/StarTls_n3ds_stub.cpp` | Thread-local storage, heap config |
| `source/client/StarClientApplication.cpp` | Client state machine, title screen |
| `source/game/StarRoot.cpp` | Asset source scanning, Root init |
| `scripts/ide/capture-n3ds-citra.ps1` | Citra capture/validation script |

---

## 3. Phase Plan

### Phase 1 — Boot & Stability ✅ DONE
- [x] Build system with devkitARM + Ninja
- [x] ELF → CXI repack with RomFS
- [x] SDMC logging for debugging
- [x] Citra validation pipeline
- [x] Lazy asset descriptor loading (eliminates 50K allocations)
- [x] Packed.pak metadata-only scan
- [x] Heap sizing (64MB app + 8MB linear)

### Phase 1b — Basic Rendering ✅ DONE
- [x] Citro2D/Citro3D initialization
- [x] Untextured primitive rendering (triangles, quads, polygons)
- [x] Texture upload pipeline (RGBA32 → Morton tile)
- [x] Scissor clip support
- [x] Bottom-screen handheld overlay
- [x] 30fps framerate lock
- [x] Input → event translation
- [x] Audio output (ndsp + csnd)
- [x] Title menu with real PNG textures

### Phase 2 - Top Screen Readability (DONE May 2026)
- [x] **Font atlas loading** - Load system font, build glyph cache (hobo.ttf + twemoji.woff2 from packed assets; N3dsStubTexture per glyph)
- [x] **Top-screen text rendering** - TextPainter pipeline verified; 20px glyphs with 2.5x font multiplier in GuiContext::setTextStyle()
- [x] **Arbitrary-rotation textured quads** - Fixed in verify93 (removed isAxisAlignedTextureRect check; imageForQuad uses UV bounding box)
- [x] **UI widget basics** - Pane manager renders on top screen for all states; ButtonWidget images via AssetTextureGroup; 9-slice via drawable system
- [x] **Title screen UI** - Visible menu buttons on top screen with embedded text images; bottom screen polished overlay
- [ ] Fix intermittent boot crash (asset init phase) - improved, still needs investigation

### Phase 3 — World Rendering
- [ ] **Tile atlas system** — Material-tinted tile rendering
- [ ] **Chunk-based culling** — Only render visible sectors
- [ ] **Layer sorting** — Background → tiles → entities → liquids
- [ ] **Parallax backgrounds** — Multi-layer sky/background
- [ ] **Entity sprite rendering** — Player, NPCs, monsters, items
- [ ] **Liquid rendering** — Water, lava, poison overlays

### Phase 4 — Visual Quality
- [ ] **Effect/shader system** — C3D shader compilation from GLSL or pre-compiled .shbin
- [ ] **Bloom** — Downscale → blur → composite pipeline
- [ ] **Color grading** — LUT-based color correction
- [ ] **Render-to-texture** — C3D render targets for multi-pass effects
- [ ] **Sprite atlas batching** — Pack sprites, batch draw calls
- [ ] **MSAA 2×** — Anti-aliased render targets

### Phase 5 — Optimisation & Polish
- [ ] **Texture compression** — ETC1/ETC1A4 for VRAM savings
- [ ] **RenderBuffer GPU optimization** — Vertex/index buffers
- [ ] **Async texture loading** — Background upload with placeholders
- [ ] **Dirty-region tracking** — Partial redraw for power saving
- [ ] **Dual-screen renderer contract** — Proper multi-screen API
- [ ] **Profile-guided optimization** — Identify CPU/GPU bottlenecks

### Phase 6 — Mod Support
- [ ] **SDMC mod loading** — Load .pak mods from SD card
- [ ] **sbinit.config parsing** — Merge user config with romfs defaults
- [ ] **Lazy reload** — Hot-reload modded assets

---

## 4. Memory Budget

| Category | Size | Notes |
|---|---|---|
| Packed.pak index | 6.5 MB | Resident in memory for O(1) lookups |
| Asset descriptors | 0 (lazy) | Built on first access |
| Arena (peak during loading) | ~12 MB | Strings, JSON, temporary buffers |
| GPU textures | ≤ 6 MB | Budget enforced, tracked dynamically |
| App heap total | 64 MB | __ctru_heap_size |
| Linear heap | 8 MB | Texture uploads, audio buffers |
| FCRAM overhead | ~60 MB | OS, GPU framebuffers, RomFS cache |

---

## 5. Known Issues

### 5.1 Boot Stability ⚠️ IMPROVED
**Status:** Fixed in verify46. Root cause was the hardcoded `N3dsBootConfiguration` in `StarRootLoader.cpp`:
1. `romfs:/` was being scanned as an asset directory, causing duplicate packed.pak scans and missing opensb root-level files like `preload.config`
2. `storageDirectory` was `sdmc:/OpenStarbound/storage` instead of `sdmc:/OpenStarbound/`
3. No mods directory configured

**Fix:** Changed assetDirectories to `["sdmc:/OpenStarbound/assets", "sdmc:/OpenStarbound/mods"]` and added `romfs:/opensb` as an `assetSource` (direct path, not scanned). Storage directory corrected.

### 5.2 Title Screen Asset Not Found (case sensitivity)
**Symptom:** `/interface/title/singleplayer.png` not found via lazy lookup.
**Root cause:** Case-sensitive HashMap lookup may not match packed.pak's stored path casing. Fallback case-insensitive iteration should catch it.
**Status:** Still present in verify46. Game falls back to lightweight title placeholder.

### 5.3 Non-Textured Quads Skip When Texture Pointer Exists
**Symptom:** `drawTexturedQuad` returns false → `drawPrimitive` skips the quad if `quad->texture` is non-null, even if texture conversion failed.
**Fix:** Modify `drawPrimitive` to fall through to untextured rendering when `drawTexturedQuad` returns false and there's a texture present. Currently the quad is just silently dropped.

### 5.4 ScreenSize Returns Single Screen
**Symptom:** `Renderer::screenSize()` returns (400, 240), the top screen only.
**Impact:** UI layout code that queries screen size gets wrong dimensions for bottom screen widgets.
**Fix:** Extend renderer API with dual-screen support.

---

## 6. Build & Test Commands

### Quick Launch (full cycle)
```powershell
# 1. Build
$env:DEVKITPRO='C:\devkitPro'; $env:DEVKITARM='C:\devkitPro\devkitARM'
$env:PATH="C:\devkitPro\devkitARM\bin;$env:PATH"
cd c:\Users\weyst\OpenStarbound\build\n3ds-phase1-client; ninja

# 2. Ensure SDMC directories exist
$sdmc = "$env:APPDATA\Citra\sdmc\OpenStarbound"
@("$sdmc\assets", "$sdmc\mods", "$sdmc\storage") | % {
  if (!(Test-Path $_)) { New-Item -ItemType Directory -Path $_ -Force | Out-Null }
}

# 3. Copy packed.pak to SDMC (only needed once, or when vanilla assets update)
# Copy-Item "path\to\vanilla\starbound\assets\packed.pak" "$sdmc\assets\packed.pak" -Force

# 4. Kill any existing Citra and run capture test
Stop-Process -Name "citra*" -Force -ErrorAction SilentlyContinue
Start-Sleep 3
cd c:\Users\weyst\OpenStarbound
.\scripts\ide\capture-n3ds-citra.ps1 -CaptureDir citra-captures\verifyN -TimeoutSeconds 360

# 5. Analyze results
Write-Host "=== Log size ==="
(Get-Item citra-captures\verifyN\starbound_n3ds.log).Length
Write-Host "=== Last 20 lines ==="
Get-Content citra-captures\verifyN\starbound_n3ds.log -Tail 20
Write-Host "=== State transitions ==="
Select-String -Path citra-captures\verifyN\starbound_n3ds.log -Pattern "OSBN3DSState|Error"
Write-Host "=== Screenshot ==="
Invoke-Item citra-captures\verifyN\window.png
```

### What the capture script does
1. Strips debug symbols from `dist/starbound` → `build/citra-repack/starbound-stripdebug.elf`
2. Copies `assets/opensb/` and `assets/sbinit.config` into a romfs staging dir
3. Uses `makerom` to build `starbound.cxi` with embedded romfs
4. Copies CXI to Citra SDMC (`%APPDATA%\Citra\sdmc\OpenStarbound\`)
5. Launches Citra Nightly 2104 with custom screen layout (top 400×240, bottom 320×240)
6. Waits for timeout, then captures screenshot and copies logs

### Quick Log Analysis
```powershell
# Check if boot reached title screen
Select-String -Path citra-captures\verifyN\starbound_n3ds.log -Pattern "OSBN3DSState.*Title"

# Check for errors
Select-String -Path citra-captures\verifyN\starbound_n3ds.log -Pattern "\[Error\]"

# Check heap usage
Select-String -Path citra-captures\verifyN\starbound_n3ds.log -Pattern "heapArena"

# Count update/renders
(Select-String -Path citra-captures\verifyN\starbound_n3ds.log -Pattern "update begin").Count
(Select-String -Path citra-captures\verifyN\starbound_n3ds.log -Pattern "render begin").Count
```

---

## 7. Key Lessons Learned

1. **Lazy descriptor loading is critical** — 50K eager allocations caused bad_alloc. Build descriptors only on first access.
2. **Metadata-only scan for .pak files** — Reading the full index (6.5MB → 3.1MB compressed) during scanning tripled memory usage. Use `PackedAssetSource::readMetadata()` instead.
3. **SDMC priority over RomFS** — RomFS file handles have issues with large files (third read always fails). Copy packed.pak to SD card.
4. **Case-insensitive lookups need fallback** — The packed index is case-sensitive but asset paths are case-insensitive. Lazy lookups must iterate all entries as fallback.
5. **3DS textures need Morton tile order + ABGR byte order** — Standard RGBA linear buffers will render as corrupt pink blocks.
6. **Don't gfxSwapBuffers after C3D_FrameEnd** — Citro presents during FrameEnd; extra swap rotates the displayed buffer away.
7. **Link order matters** — Append `libctru` after citro libs for GPU symbol resolution.

---

*Updated after verify36 milestone. Next validation target: verifyN with texture preloading + boot stability fix.*

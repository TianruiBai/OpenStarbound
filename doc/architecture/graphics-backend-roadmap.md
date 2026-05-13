# Graphics Backend Roadmap

This roadmap covers the client graphics API work needed to keep the current OpenGL renderer as the stable production path while adding an experimental Vulkan backend that can be built and shipped side by side.

The graphics backend work is separate from the server multicore plan. It can improve client frame time, driver overhead, frame pacing, and GPU diagnostics, but it should not be used as a substitute for world-thread or replication optimization.

## 1. Current Backend Shape

Important source boundaries:

- `source/application/StarRenderer.hpp` is the main backend-facing contract. Gameplay, UI, world painters, and text rendering mostly talk through `Renderer`, `Texture`, `TextureGroup`, `RenderBuffer`, and `RenderPrimitive` instead of direct OpenGL calls.
- `source/application/StarRenderer_opengl.*` is the only concrete renderer today. It owns OpenGL textures, texture atlases, render buffers, effect programs, framebuffers, immediate primitive flushing, and draw submission.
- `source/application/StarMainApplication_sdl.cpp` hard-wires the application to OpenGL by creating the SDL window with `SDL_WINDOW_OPENGL`, creating an `SDL_GLContext`, constructing `OpenGlRenderer`, initializing `imgui_impl_opengl3`, using `SDL_GL_SetSwapInterval`, and presenting with `SDL_GL_SwapWindow`.
- `source/client/StarClientApplication.cpp` loads `/rendering/opengl.config`, loads GLSL effect configs from `/rendering/effects/*.config`, switches between `world`, `interface`, and post-process effects, and keeps the higher-level render flow backend-neutral enough to preserve.
- `source/rendering/` and `source/windowing/` mostly consume the generic renderer API. That is the biggest reason Vulkan should start as a backend split instead of a UI/world-render rewrite.
- `source/extern/imgui_lua_bindings.cpp` assumes `ImTextureID` can be treated like an OpenGL integer texture id. Any Vulkan path that exposes ImGui images to Lua must replace that assumption with an opaque backend texture handle or a small compatibility adapter.

## 2. Distribution Stance

The first production-friendly packaging shape should be two client builds from the same source tree:

1. `starbound`: stable OpenGL client, current default, expected to remain the supported fallback.
2. `starbound_vulkan`: experimental Vulkan client, opt-in, clearly labeled in package scripts and diagnostics.

Later, once both backends can coexist cleanly in one binary, a runtime selector such as `--graphics-backend opengl|vulkan` or a config field can be added. The side-by-side executable approach is safer first because the current application object library directly includes OpenGL-specific window, renderer, and ImGui code.

OpenGL should not be removed until Vulkan has long-running parity across window modes, UI, post-process effects, modded assets, screenshots, alt-tab/resizing, driver variants, and crash diagnostics.

## 3. Backend Boundary Plan

Add an application-layer graphics backend boundary before writing the Vulkan renderer.

Recommended shape:

1. Introduce a small backend runtime interface owned by `source/application/`, for example `GraphicsBackendRuntime` or `SdlGraphicsBackend`.
2. Move backend-specific SDL setup into implementations:
   - OpenGL implementation: `SDL_WINDOW_OPENGL`, `SDL_GL_CreateContext`, `SDL_GL_SetSwapInterval`, ImGui OpenGL init/new-frame/render/shutdown, and `SDL_GL_SwapWindow`.
   - Vulkan implementation: `SDL_WINDOW_VULKAN`, SDL Vulkan instance extension/surface setup, swapchain present mode selection, ImGui Vulkan init/new-frame/render/shutdown, and swapchain present.
3. Store the renderer in `SdlPlatform` as `RendererPtr`, not `OpenGlRendererPtr`.
4. Promote frame lifecycle currently exposed only by `OpenGlRenderer` (`startFrame`, `finishFrame`) into either the generic renderer contract or the new backend runtime.
5. Keep `ClientApplication`, `GuiContext`, `WorldPainter`, `TilePainter`, `TextPainter`, and widgets talking to the same renderer primitives during the first Vulkan milestones.

This split keeps the high-level render flow stable while exposing the exact seams that Vulkan needs: window surface, device/swapchain lifetime, frame synchronization, ImGui backend, present, and shader pipeline selection.

## 4. Vulkan Bring-Up Milestones

### 4.1 Build and dependency gate

- Add CMake options such as `STAR_ENABLE_VULKAN_BACKEND` and `STAR_BUILD_VULKAN_CLIENT`.
- Add Vulkan headers/loader discovery through `find_package(Vulkan)` or vcpkg packages such as `vulkan-headers` and `vulkan-loader`.
- Add a shader compilation choice before landing effect parity: offline SPIR-V assets checked into the packed assets, build-time compilation, or runtime compilation through a tool such as shaderc/glslang. Prefer offline or build-time output for shipped builds.
- Add the ImGui Vulkan backend dependency only for Vulkan builds. The current manifest enables ImGui's SDL3 and OpenGL3 bindings; Vulkan builds will need the matching Vulkan binding as well.

### 4.2 Empty Vulkan client

- Create a Vulkan SDL window and surface.
- Create instance, physical device, logical device, queues, swapchain, command pool, and per-frame synchronization.
- Clear the swapchain to a visible color and present reliably.
- Report backend id, API version, selected adapter, queue family, present mode, and swapchain format in logs and diagnostics.

### 4.3 Renderer primitive parity

- Implement `VulkanRenderer` behind the existing `Renderer` API.
- Start with flat colored triangles/quads and the `interface` effect equivalent.
- Add texture upload for lone textures, then texture groups/atlases.
- Map `RenderBuffer` to Vulkan vertex/index buffers with per-frame staging and safe lifetime handling.
- Preserve coordinate conventions: screen and texture coordinates are pixel-based and bottom-left oriented in the existing renderer contract.

### 4.4 Effect and framebuffer parity

- Add `/rendering/vulkan.config` rather than overloading `/rendering/opengl.config`.
- Extend effect configs to select backend-specific shader sources or compiled SPIR-V while keeping effect names, parameter names, texture names, and scriptable parameters stable.
- Implement the world/interface shader equivalents, including multi-texture selection, vertex rounding, fullbright/light map behavior, and post-process passes.
- Map OpenGL framebuffers to Vulkan render targets or subpasses in a way that preserves the existing post-process layer order.

### 4.5 UI, ImGui, and Lua texture handles

- Use ImGui's Vulkan backend for the Vulkan client.
- Replace Lua/ImGui assumptions that an image id is a raw OpenGL texture integer with an opaque backend texture handle.
- Keep existing game UI rendering through `GuiContext`, `DrawablePainter`, and `TextPainter` unchanged where possible.
- Add screenshot and debug overlay paths for both backends.

### 4.6 Packaging and fallback

- Package OpenGL and Vulkan clients side by side.
- Keep OpenGL as the default shortcut/launcher target.
- Make Vulkan failures produce clear diagnostics and instructions to relaunch with OpenGL.
- Record the backend in crash reports, `/debug` renderer pages, logs, and any future diagnostic bundles.

## 5. Vulkan-Specific Design Rules

- Do not expose Vulkan objects to gameplay, UI widgets, Lua gameplay APIs, or asset loading unless there is no stable renderer-level alternative.
- Keep resource destruction deterministic; Vulkan texture, buffer, descriptor, and framebuffer lifetimes must account for frames in flight.
- Avoid unbounded per-draw descriptor allocation. Batch by texture groups and effect pipeline where possible.
- Treat swapchain recreation as a first-class path. Resize, fullscreen changes, minimization, and display scale changes must be tested early.
- Keep the OpenGL shader/effect path intact. Vulkan shader assets should be additive.
- Do not change save files, packet formats, or mod-visible gameplay behavior for a graphics backend change.

## 6. Diagnostics To Add

Minimum backend diagnostics:

- backend id: `opengl` or `vulkan`
- API version and driver/vendor strings
- selected GPU/adapter name
- window mode, drawable size, display scale, swapchain/image count, present mode, and vsync/adaptive-vsync state
- frame CPU time, GPU time when available, present wait time, command buffer count, draw calls, vertices, texture uploads, buffer uploads, descriptor/pipeline cache counts, and swapchain recreation count
- effect/shader load failures with backend and effect name

The renderer page in the debug overlay should become API-neutral: it should show OpenGL version fields for OpenGL and Vulkan instance/device/swapchain fields for Vulkan.

## 7. Acceptance Gates

Before Vulkan graduates from experimental:

1. The OpenGL client still builds and runs as the default.
2. Vulkan starts, renders title/menu UI, enters a world, renders tiles/entities/liquid/lighting, opens common panes, and exits cleanly.
3. Windowed, fullscreen, borderless, resize, high-DPI, vsync off, vsync on, and alt-tab paths survive repeated testing.
4. Screenshot or framebuffer comparison tests show stable output for fixed scenes within documented tolerance.
5. Post-process layers and scriptable renderer parameters behave the same at the effect contract level.
6. Modded assets and UI scripts that do not rely on raw OpenGL handles behave normally.
7. Crash reports and logs include enough backend context to diagnose driver and shader issues.
8. OpenGL remains available as an immediate fallback in every distributed package.

## 8. Suggested First Implementation Slice

The smallest useful code slice is not a full Vulkan renderer. It is the backend boundary split:

1. Extract OpenGL window/context/ImGui/present handling from `SdlPlatform` into an OpenGL backend runtime.
2. Store the renderer as `RendererPtr` and route frame lifecycle through the backend runtime.
3. Keep the produced OpenGL binary behavior identical.
4. Add a stub Vulkan backend option that fails gracefully with a clear log message if Vulkan is not built.

After that lands, the Vulkan executable can be brought up incrementally without risking the stable OpenGL client.
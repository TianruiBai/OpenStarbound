# Nintendo 3DS Port Feasibility

Date: 2026-05-14

Scope: assess whether OpenStarbound can be ported to Nintendo 3DS hardware, including New 3DS / New 2DS XL class devices, and whether local mod support is realistic.

## Executive Summary

Short version:

- Old 3DS is not a realistic target for a usable Starbound port.
- New 3DS is only plausible as a long-running research port with major scope cuts.
- A feature-parity client with local single-player and broad mod compatibility would require major rewrites of the renderer, UI/input layer, and likely the single-player runtime model.
- Local SD-card mods are structurally possible because OpenStarbound already loads directory and `.pak` asset sources by priority and dependency.
- Steam Workshop support is not realistic on 3DS and should be treated as out of scope.
- The most realistic experiment is a New 3DS-only remote-client prototype with a new renderer backend and a simplified UI.

The key reason is not the build system. The key reason is that the current client is built around SDL3 plus desktop OpenGL plus a desktop-style UI, while the 3DS ecosystem offers either:

- SDL on 3DS with software rendering only, or
- native 3DS GPU libraries (`citro2d` / `citro3d`) that do not match the current OpenGL renderer model.

## Decision Matrix

| Target | Verdict | Why |
| --- | --- | --- |
| Old 3DS full game | No | Renderer rewrite, small memory budget, low CPU headroom, local single-player server cost, and tiny-screen UI redesign all stack together badly. |
| New 3DS full single-player | Very high risk | Better clocks and memory help, but the renderer rewrite and local server cost still dominate. |
| New 3DS remote multiplayer client | Plausible research target | Avoids local universe simulation and keeps the effort concentrated on rendering, input, and UI. |
| Local mods from SD | Yes, with limits | The asset loader already supports directory and `.pak` sources, but memory and load-time budgets will require restrictions. |
| Steam Workshop mods | No | Current Workshop path depends on desktop/storefront services that do not map cleanly to 3DS. |

## Why The Current Codebase Is Hard To Move

### 1. The client bootstrap is strongly tied to SDL3 plus desktop OpenGL

The main client platform layer in `source/application/StarMainApplication_sdl.cpp` does all of the following during startup:

- initializes SDL video, gamepad, and audio
- creates an SDL window with `SDL_WINDOW_OPENGL`
- requests a GL 3.0 core context on most platforms and GL 3.2 core on macOS
- creates the OpenGL context and swaps through it
- opens a 44.1 kHz, 16-bit stereo audio stream
- initializes ImGui with the SDL3 and OpenGL backends
- translates keyboard, text-input, mouse, wheel, and controller events into the engine input model

This means the current platform layer is not just abstract "window creation". It assumes a desktop-style GL window and a desktop-style event model.

### 2. The renderer is a real portability wall

`source/application/StarRenderer_opengl.hpp` and `source/application/StarRenderer_opengl.cpp` show that the current renderer:

- depends on GLEW
- requires OpenGL 2.0 at minimum
- uses shaders, uniforms, framebuffers, texture atlases, and multi-texturing
- expects a conventional GL viewport and framebuffer flow

The constructor in `OpenGlRenderer` explicitly fails if OpenGL 2.0 is unavailable.

That is a hard mismatch with 3DS graphics tooling:

- the maintained 3DS GPU path is `citro3d`, which explicitly deviates from OpenGL
- the higher-level 2D path is `citro2d`, which is useful for sprites and UI, not as a drop-in OpenGL replacement
- historical OpenGL-like wrappers listed on 3dbrew such as `gl3ds` and `Caelina` are unmaintained

So this is not a "fix a few GL calls" job. It is a renderer replacement project.

### 3. Single-player currently starts a full local universe server

In `source/client/StarClientApplication.cpp`, single-player creates a local `UniverseServer` and connects the client to it via `addLocalClient()`.

That matters because a handheld single-player port inherits both halves of the runtime:

- the client render/UI/input workload
- the server world simulation, persistence, and networking workload

`source/game/StarUniverseServer.cpp` shows the server starts worker pools, persistence workers, and a connection server. Even if some of this can be tuned down, it is not a lightweight "single-thread toy server".

### 4. Root startup and asset loading expect a larger runtime budget

`source/game/StarRoot.cpp` shows `Root::fullyLoad()` warming a large number of gameplay databases in parallel. This includes behavior, AI, quests, terrain, particles, versioning, items, monsters, liquids, dungeons, tilesets, and more.

This is good news for desktop responsiveness, but bad news for a tight-memory handheld target.

### 5. Mod support exists because the asset loader is flexible

The good news is in `source/game/StarRoot.cpp`, `source/base/StarAssets.cpp`, and `source/frontend/StarModsMenu.cpp`:

- root boot scans asset directories and manual asset sources
- sources can be either directories or `.pak` files
- sources have metadata, dependency ordering, and priorities
- assets can be patched and even modified by on-load Lua scripts
- the mods menu already enumerates asset sources and their metadata

This is why local mod support is technically possible on 3DS, even though Steam Workshop is not.

## What The 3DS Homebrew Stack Gives You

### Toolchain and base libraries

The current devkitPro stack provides a real toolchain for 3DS development:

- `devkitARM`
- `libctru`
- the `3ds-dev` predefined package group in devkitPro pacman

`libctru` explicitly describes itself as a low-level foundation for 3DS homebrew, not a high-level substitute for SDL.

### SDL exists, but with a critical limitation

SDL has Nintendo 3DS build docs for both SDL2 and SDL3. The SDL3 3DS README is the important one here because OpenStarbound is already on SDL3.

The critical note is:

- currently only software rendering is supported

That changes the practical boundary:

- the SDL3-facing parts of the OpenStarbound platform layer may be partly reusable
- the current OpenGL renderer is not reusable on top of that SDL port

The same SDL note also states:

- `SDL3_main` should be used so ROMFS is enabled
- New 2DS / New 3DS higher clocks and L2 cache are enabled by default
- the 3DS uses a cooperative threading model on a single core in this SDL environment, so threads must yield explicitly

That threading note is especially important for this codebase because OpenStarbound relies on worker pools, background loading, network workers, and a local server thread model.

### Native graphics options exist, but they are not OpenGL replacements

The maintained 3DS graphics libraries are:

- `citro2d`: optimized 2D drawing on PICA200
- `citro3d`: stateful PICA200 interface

`citro3d` explicitly says it deviates from OpenGL. That makes it useful for a custom renderer backend, but not for mechanically porting the existing `OpenGlRenderer`.

### Filesystem, networking, and input are available

`libctru` provides the basic platform services a port would need:

- filesystem APIs with `ARCHIVE_SDMC` support, file/directory enumeration, read/write, and SD card detection
- socket support through `socInit()` and standard socket headers
- HID support for buttons, Circle Pad, touch input, and New 3DS-only C-stick / ZL / ZR
- RomFS mounting for packaged read-only data

So the platform can support:

- SD-card save data
- SD-card mod directories
- remote multiplayer networking
- touch plus controller-driven UI

The weak point is not access to these services. The weak point is fitting OpenStarbound's runtime and renderer onto the hardware.

## Platform Constraints That Matter Most

### CPU and scheduling

From 3dbrew hardware notes:

- Old 3DS: ARM11 at roughly 268 MHz
- New 3DS: up to 804 MHz, with optional 2 MB L2 cache

From the SDL3 3DS README:

- the SDL environment on 3DS is cooperative and single-core from the app's point of view unless threads yield explicitly

That is a poor fit for a codebase that uses multiple worker pools and a local server thread model.

### Memory

3dbrew memory layout notes show large differences between Old and New 3DS application memory modes:

- Old 3DS application memory modes top out around 64 MB to 96 MB depending on configuration
- New 3DS supports larger application regions, including 124 MB by default in newer modes and up to 178 MB in the larger mode

Exact availability depends on how the title is built and launched, but the high-level conclusion is clear:

- Old 3DS memory is a severe constraint
- New 3DS memory is better, but still far below a typical desktop budget for a game with dynamic asset loading, Lua, and local world simulation

### GPU and VRAM

3dbrew hardware notes describe:

- PICA200 GPU at 268 MHz
- 6 MB VRAM
- a graphics model that is closer to OpenGL ES 1.1 plus proprietary PICA-specific facilities than to desktop OpenGL 2.0+

That is fundamentally mismatched with the current renderer's shader-oriented GL path.

### Displays and input model

3dbrew hardware notes describe:

- top screen: 400x240 usable pixels per eye
- bottom screen: 320x240 touch screen
- resistive touch
- Circle Pad on all models
- C-stick plus ZL/ZR on New 3DS only

OpenStarbound's current GUI and event flow are strongly desktop-shaped:

- mouse hover and drag assumptions
- keyboard text input
- resizable window and scale handling
- many panes laid out for desktop aspect ratios

That does not make a port impossible, but it does make a direct UI carry-over unrealistic.

### Audio

3dbrew hardware notes list the DSP at 32728 Hz sampling characteristics, while the SDL platform code in OpenStarbound opens 44.1 kHz stereo audio.

This is not necessarily a blocker, but it is a sign that the audio backend needs real testing and probably resampling care. Optional features like voice chat should be treated as later work or disabled initially.

## Mod Support Assessment

## What already works in principle

OpenStarbound already has the right architectural idea for local mods:

- `README.md` tells users to copy `packed.pak` from a legal Starbound install and optionally copy local `mods/`
- `sbinit.config` examples already separate `assetDirectories` and `storageDirectory`
- `Root::scanForAssetSources()` accepts both directories and `.pak` files
- asset sources are ordered by priority and dependency
- the mods menu already exposes mod metadata

That means a 3DS build can, in principle, load:

- the user's owned base `packed.pak`
- OpenStarbound overlay assets
- extra local mods stored on SD

## What needs to be dropped or replaced

Steam-specific UGC support is not realistic:

- `UserGeneratedContentService` currently maps to PC storefront behavior
- the mods menu contains a workshop link path
- `DesktopService` URL opening is also desktop-specific

On 3DS, the realistic approach is:

- no Workshop integration
- no desktop browser open behavior
- local file-based mods only

## Practical mod verdict

Local mod support is feasible, but not at desktop parity.

Reasonable expectations for a first 3DS-capable design:

- support SD-card directory mods and `.pak` mods
- prefer curated or prepacked mods over huge loose-file installs
- avoid claiming broad compatibility with heavy Lua or asset-generation mods until memory and load-time profiling exists
- keep the requirement that users provide their own legal `packed.pak`

## Feasibility By Port Strategy

### 1. Old 3DS full client with local single-player

Verdict: not recommended.

The combination of:

- renderer rewrite
- small memory budget
- weak CPU headroom
- cooperative threading model
- local `UniverseServer` cost
- tiny-screen UI redesign

pushes this out of practical range.

### 2. New 3DS full client with local single-player

Verdict: theoretically possible only after major rewrites, but still a very poor near-term bet.

New 3DS helps with clocks, L2 cache, extra controls, and larger memory modes. It does not remove the main blockers:

- SDL3 on 3DS still only offers software rendering
- the current renderer still has to be replaced
- the local server still has to run acceptably
- the UI still has to be redesigned

### 3. New 3DS remote multiplayer client

Verdict: best research target.

This avoids the biggest non-rendering burden: local universe simulation.

A remote-client-first plan lets the port focus on:

- booting on hardware
- asset loading from SD / RomFS
- a new renderer backend
- controller/touch UI
- network play against a desktop server

### 4. 3DS dedicated server

Verdict: not a sensible target.

The dedicated server gains little from the handheld form factor and still inherits the world simulation and persistence cost. If the goal is "Starbound on 3DS", server work should stay on desktop and the handheld should be treated as a client.

## Recommended Research Path

If anyone seriously attempts this, the order should be:

1. Target New 3DS only first.
2. Bring up a minimal SDL3 or libctru bootstrap with RomFS and SD-card access.
3. Replace `OpenGlRenderer` with a dedicated 3DS backend. Do not try to force the current GLEW/OpenGL renderer through.
4. Stub non-portable platform services: Steam, Workshop UGC, Discord, desktop URL opening, clipboard, and probably voice chat for the first pass.
5. Rebuild the title/menu UI for gamepad plus touch on 400x240 and 320x240 screens.
6. Connect only to a remote server first.
7. After the remote client works, test loading the base `packed.pak` plus a tiny SD mod directory.
8. Only then decide whether local single-player is worth attempting.

## Overall Conclusion

OpenStarbound on Nintendo 3DS is not impossible in the narrow sense that the toolchain, filesystem, sockets, input, and even SDL3 support exist. But the existing codebase does not line up with the platform in the way that would make this a straightforward port.

The dominant blocker is the renderer. The current client expects desktop OpenGL and an SDL3 OpenGL window. The 3DS ecosystem offers software-rendered SDL and native GPU libraries that are intentionally not OpenGL. That turns the port into a renderer rewrite plus UI redesign project before gameplay performance is even measured.

So the realistic answer is:

- Old 3DS: effectively no.
- New 3DS: maybe, but only as a heavily scoped research port.
- Mod support: local mods yes, Workshop no.
- Best path: New 3DS-only remote client first, local single-player later if profiling gives a reason to believe it can work.

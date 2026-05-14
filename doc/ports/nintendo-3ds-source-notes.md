# Nintendo 3DS Port Research Notes

Date: 2026-05-14

This appendix records the codebase anchors and external references used to assess Nintendo 3DS / New 3DS feasibility.

## Codebase Anchors

| Area | Source anchor | What it shows | Port implication |
| --- | --- | --- | --- |
| Dependency stack | `source/vcpkg.json` | Client build depends on SDL3, GLEW, ImGui SDL3/OpenGL bindings, `cpr`, and desktop-oriented third-party libraries. | A 3DS port cannot keep the current dependency stack unchanged. |
| SDL platform bootstrap | `source/application/StarMainApplication_sdl.cpp` | Initializes SDL video/gamepad/audio, creates an OpenGL SDL window, creates GL context, opens 44.1 kHz stereo audio, and starts ImGui SDL3/OpenGL backends. | The platform layer is not abstract enough to retarget without real work. |
| OpenGL renderer contract | `source/application/StarRenderer_opengl.hpp` | Renderer owns texture atlases, framebuffers, render buffers, uniforms, and GL texture objects. | The renderer is specialized to desktop-style GL. |
| OpenGL minimum requirement | `source/application/StarRenderer_opengl.cpp` | `OpenGlRenderer` fails if OpenGL 2.0 is unavailable. | The current renderer cannot be a direct fit for 3DS graphics APIs. |
| Application controller | `source/application/StarApplicationController.hpp` | Exposes fullscreen/window resize, cursor control, text input, clipboard, desktop service, and audio input/output. | Some controller features need stubs or redesign on 3DS. |
| Windowing and GUI | `source/windowing/StarGuiContext.cpp`, `source/windowing/StarPane.cpp`, `source/windowing/StarCanvasWidget.cpp` | GUI uses mouse position, window size, interface scaling, and mouse events heavily. | UI needs a gamepad/touch-first redesign for 3DS screen constraints. |
| Single-player startup | `source/client/StarClientApplication.cpp` | Single-player creates `UniverseServer`, starts it, and locally connects a `UniverseClient`. | Full handheld single-player inherits both client and server cost. |
| Server runtime | `source/game/StarUniverseServer.cpp` | Server creates worker pools, persistence worker pools, and a connection server. | Local single-player is substantially heavier than just rendering the world. |
| Root warmup | `source/game/StarRoot.cpp` | `Root::fullyLoad()` warms a large set of gameplay databases in parallel. | Startup memory and scheduling pressure are significant for a small device. |
| Asset loading | `source/base/StarAssets.cpp` | Asset sources can be directories or packed files, patch assets, run on-load scripts, and maintain source metadata. | Local mods are structurally possible on 3DS if SD storage is wired in. |
| Asset discovery and ordering | `source/game/StarRoot.cpp` | `scanForAssetSources()` accepts directory and `.pak` inputs, sorts by priority, and resolves dependencies. | The mod system is already file-based, which helps a non-Steam target. |
| Mods menu | `source/frontend/StarModsMenu.cpp` | Enumerates asset sources and shows their metadata, including optional Workshop link behavior. | Mod enumeration survives; Workshop-specific behavior should be removed or stubbed. |
| Legal asset requirement | `README.md` | Users must own Starbound and copy `packed.pak`; local `mods/` are already part of the documented install flow. | A 3DS build would still need the user to provide legal base assets. |

## External Platform References

### Toolchain and base platform

- 3dbrew Homebrew Libraries and Tools
  - https://www.3dbrew.org/wiki/Homebrew_Libraries_and_Tools
  - Confirms `libctru`, `citro2d`, `citro3d`, `devkitARM`, and historical OpenGL-like libraries such as `gl3ds` and `Caelina`.
  - Important note: `gl3ds` and `Caelina` are listed as unmaintained.

- devkitPro pacman
  - https://devkitpro.org/wiki/devkitPro_pacman
  - Confirms the `3ds-dev` predefined package group and the supported installation flow.

- libctru main docs
  - https://libctru.devkitpro.org/
  - Describes `libctru` as a low-level foundation for 3DS homebrew, not a high-level replacement for SDL.

### SDL on 3DS

- SDL3 3DS README
  - https://wiki.libsdl.org/SDL3/README-n3ds
  - Key findings:
    - only software rendering is currently supported
    - `SDL3_main` should be used so RomFS is enabled
    - New 2DS / New 3DS speedup and L2 cache are enabled by default
    - threading is cooperative and must yield explicitly

- SDL2 3DS README
  - https://wiki.libsdl.org/SDL2/README-n3ds
  - Mostly confirms the same platform notes for the older SDL generation.

- devkitPro SDL fork
  - https://github.com/devkitPro/SDL
  - Shows there is an active devkitPro SDL fork lineage, but the SDL wiki note about software rendering is still the important constraint.

### Graphics libraries

- citro2d docs
  - https://citro2d.devkitpro.org/
  - `citro2d` is optimized for 2D drawing, sprites, text, texture atlases, and mixed `citro2d` / `citro3d` usage.

- citro2d repository
  - https://github.com/devkitPro/citro2d
  - Confirms current maintenance and packaging.

- citro3d repository
  - https://github.com/devkitPro/citro3d
  - Important note: `citro3d` "deviates from OpenGL" by design.

### Hardware, memory, and input

- 3dbrew Hardware
  - https://www.3dbrew.org/wiki/Hardware
  - Key findings:
    - Old 3DS ARM11 at roughly 268 MHz
    - New 3DS up to 804 MHz with optional 2 MB L2 cache
    - PICA200 GPU at 268 MHz
    - 6 MB VRAM
    - top screen 400x240 per eye, bottom screen 320x240 touch
    - New 3DS has C-stick and ZL / ZR

- 3dbrew Memory Layout
  - https://www.3dbrew.org/wiki/Memory_layout
  - Key findings:
    - Old 3DS application memory modes around 64 MB to 96 MB
    - New 3DS larger application memory modes, including 124 MB and 178 MB configurations
    - exact availability depends on build / launch mode

- 3dbrew New 3DS
  - https://www.3dbrew.org/wiki/New_3DS
  - Confirms New 3DS extra memory and hardware controls.

### Filesystem, networking, and input services

- libctru filesystem services
  - https://libctru.devkitpro.org/fs_8h.html
  - Key findings:
    - `ARCHIVE_SDMC` exists
    - file and directory create/read/write/delete APIs are available
    - SD card detection, writability checks, and SDMC archive resource queries are available

- libctru sockets service
  - https://libctru.devkitpro.org/soc_8h.html
  - Key findings:
    - `socInit()` enables socket communication
    - standard socket headers are usable after initialization
    - IP and routing information can be queried

- libctru RomFS support
  - https://libctru.devkitpro.org/romfs_8h.html
  - Key findings:
    - RomFS can be mounted from the current title or embedded image
    - useful for packaged read-only assets or config data

- libctru HID support
  - https://libctru.devkitpro.org/hid_8h.html
  - Key findings:
    - touch input is available
    - Circle Pad input is available
    - New 3DS C-stick and ZL / ZR are available
    - accelerometer and gyro APIs exist

## Derived Takeaways

1. The best reusable part of the current client is the high-level game logic, not the current rendering or UI platform layer.

2. SDL3 on 3DS helps with bootstrapping, input, and some platform services, but it does not preserve the current OpenGL renderer because the 3DS SDL port only supports software rendering.

3. The 3DS-native graphics path points toward a new `citro2d` / `citro3d` backend, not a thin compatibility shim around `OpenGlRenderer`.

4. New 3DS should be the only serious initial target. Old 3DS should not drive the first design.

5. A remote-multiplayer-first target is materially easier than a full single-player target because OpenStarbound currently starts a full local `UniverseServer` for single-player.

6. Local mod support is realistic because OpenStarbound's asset system is already filesystem-driven and supports both directories and `.pak` files.

7. Workshop support is not realistic on 3DS because the current path depends on PC storefront services and desktop URL opening.

8. Heavy mod packs, Lua-driven asset generation, and large loose-file installs are the first things likely to collapse under 3DS memory and load-time limits.

9. If a port ever proceeds, it should get its own platform preset, its own renderer backend, and a reduced platform-service implementation rather than trying to preserve every desktop-facing subsystem.

10. The renderer rewrite is the controlling task. Everything else is secondary until there is a way to draw the game efficiently on real 3DS hardware.

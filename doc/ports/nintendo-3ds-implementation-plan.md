# Nintendo 3DS Port Implementation Plan

Date: 2026-05-17

Scope: phased implementation plan for a New 3DS-focused OpenStarbound research port, including environment setup and early build-system bootstrap.

## Ground Rules

- Initial target is New 3DS / New 2DS XL only.
- Initial product target is remote-multiplayer client, not local single-player.
- Workshop and desktop service integrations are out of scope for early phases.
- Rendering work is the controlling critical path.

## Phase 0 - Environment Setup And Branch Hygiene

Goal: establish repeatable build/dev environment for 3DS experiments.

Deliverables:
- Dedicated branch and naming policy for 3DS work.
- devkitPro + devkitARM + 3ds-dev installation instructions for Windows and Linux.
- CMake preset for Nintendo 3DS bootstrap configuration.
- Toolchain file for arm-none-eabi cross-compile discovery.
- Developer helper script(s) to set DEVKITPRO / DEVKITARM in shell sessions.

Implementation tasks:
- Add a new configure/build preset in source/CMakePresets.json for 3DS bootstrap.
- Add toolchains/nintendo-3ds-devkitarm.cmake with compiler discovery and root path setup.
- Add scripts/ide/setup-devkitpro-env.ps1 for Windows shell setup.
- Document expected environment variables and recommended package list.

Definition of done:
- cmake --preset=n3ds-devkitarm-bootstrap configures on a machine with devkitPro installed.
- Preset fails fast with clear error messages when DEVKITPRO/devkitARM is missing.

Risk checks:
- Verify no accidental coupling to vcpkg desktop toolchain in the 3DS preset.
- Keep this phase focused on configure-time bootstrap, not full link success.

## Phase 1 - Platform Abstraction Baseline

Goal: create explicit platform seams so 3DS code can be introduced without destabilizing desktop targets.

Deliverables:
- Build option and compile definitions for STAR_PLATFORM_N3DS.
- Clear split of desktop-only services (Steam, Discord, desktop URL opening, clipboard, voice capture path).
- N3DS stub implementations for unsupported platform features.

Implementation tasks:
- Audit application controller and platform service APIs for desktop assumptions.
- Introduce compile-time feature gates and runtime capability reporting.
- Add non-fatal stubs for unsupported services.

Definition of done:
- Existing desktop presets still build unchanged.
- 3DS preset can compile shared core/serverless pieces with stubs enabled.

Risk checks:
- Avoid leaking desktop assumptions into platform-neutral headers.
- Keep API contracts stable to avoid broad refactors in one pass.

## Phase 2 - Runtime Profile Reduction (Remote Client First)

Goal: reduce runtime scope to what is needed for a remote client prototype.

Deliverables:
- A client startup mode that never starts local UniverseServer.
- Config flag/profile for remote-only mode.
- Startup and memory telemetry hooks for constrained targets.

Implementation tasks:
- Split single-player bootstrap from client initialization path.
- Add remote-only launch pipeline and guard local-server code.
- Add instrumentation around Root::fullyLoad and early asset warmup.

Definition of done:
- Desktop build can launch in remote-only profile without local server creation.
- Profile exposes measurable startup memory/time metrics.

Risk checks:
- Keep network protocol compatibility untouched.
- Ensure remote-only profile does not regress default desktop behavior.

## Phase 3 - 3DS Rendering Backend Spike

Goal: prove drawing viability on hardware/emulator with a new backend.

Deliverables:
- New renderer backend skeleton targeting citro2d/citro3d concepts.
- Minimal frame loop rendering test scene and UI primitives.
- Texture upload and atlas strategy adapted to 3DS VRAM limits.

Implementation tasks:
- Define backend-agnostic renderer interface boundary from OpenGlRenderer usage sites.
- Implement first-pass command translation for sprites/text/quads.
- Add top-screen render target baseline and profiling counters.

Definition of done:
- A simple in-engine render path displays stable frames on New 3DS class target.
- Frame time and memory metrics are collected for baseline decisions.

Risk checks:
- Prevent API drift from desktop OpenGL assumptions.
- Track shader/pipeline mismatches early; do not force OpenGL semantics.

## Phase 4 - Input/UI Adaptation For Dual Screens

Goal: make core game navigation usable with Circle Pad/buttons/touch on 400x240 + 320x240.

Deliverables:
- Input mapping layer for Circle Pad, D-pad, ABXY, shoulder buttons, touch.
- Reduced-complexity UI layout profile for handheld screens.
- On-screen keyboard/text-entry strategy for login/chat fields.

Implementation tasks:
- Introduce pointer emulation and focus navigation model.
- Redesign critical panes (title/menu/options/inventory/chat) for small screens.
- Decide top-vs-bottom screen role split and enforce consistent UX rules.

Definition of done:
- User can boot, navigate menus, connect to server, and perform basic in-game actions.
- No keyboard/mouse hard dependency in critical path.

Risk checks:
- Prevent one-off per-pane hacks; use shared layout/input policy.
- Measure touch latency and cursor precision issues.

## Phase 5 - Asset/Mod Loading Constraints

Goal: preserve local file-based mods with explicit constraints suitable for 3DS resources.

Deliverables:
- SD-based asset path profile for packed.pak + mods directory.
- Mod compatibility tiers (small-safe, medium-risk, unsupported-heavy).
- Load-time and memory guardrails with user-facing warnings.

Implementation tasks:
- Configure platform-specific assetDirectories/storage defaults.
- Add mod scanning limits (file count/size/script budget) for safety.
- Add diagnostics in mods menu for constrained-mode compatibility.

Definition of done:
- Base packed.pak + at least one small mod directory can load on target profile.
- Unsupported Workshop paths are hidden or clearly disabled.

Risk checks:
- Watch Lua on-load scripts and loose-file storms for memory collapse.
- Ensure clear messaging when mods are rejected by constraints.

## Phase 6 - Networked Play Validation And Optimization

Goal: validate practical online play quality for the remote-client strategy.

Deliverables:
- Stable connect/play loop against desktop OpenStarbound server.
- Latency and packet handling measurements on target.
- CPU/memory optimization backlog prioritized by measured hotspots.

Implementation tasks:
- Validate socket init/lifecycle and reconnect paths.
- Profile render/input/network threads under cooperative scheduling constraints.
- Apply targeted optimizations (tick budget, update culling, decoding paths).

Definition of done:
- Play sessions are stable for defined target duration under representative content.
- Top bottlenecks are identified with data and tracked.

Risk checks:
- Detect desync triggers and frame hitches under network stress.
- Confirm no blocking operations on frame-critical loop.

## Phase 7 - Optional Local Single-Player Feasibility Revisit

Goal: re-evaluate whether local UniverseServer is viable after remote-client milestone.

Deliverables:
- Measured single-player prototype viability report.
- Go/no-go decision with explicit perf and memory thresholds.

Implementation tasks:
- Add experimental local-server mode behind hard feature gate.
- Benchmark startup, memory peak, and sustained play stability.

Definition of done:
- Data-driven decision: continue, narrow scope heavily, or reject local single-player.

Risk checks:
- Avoid sunk-cost bias; prioritize user-playable remote-client success.

## Environment Setup Checklist

Windows (PowerShell):
- Install devkitPro using the official Windows installer (default root is C:/devkitPro).
- Open the devkitPro MSYS2 shell and install packages:
	- dkp-pacman -Syu
	- dkp-pacman -S --needed 3ds-dev
- In a PowerShell terminal for OpenStarbound, run:
	- powershell -ExecutionPolicy Bypass -File scripts/ide/setup-devkitpro-env.ps1
- Validate toolchain visibility:
	- Test-Path "$env:DEVKITPRO\\devkitARM\\bin\\arm-none-eabi-gcc.exe"
	- arm-none-eabi-gcc --version
- Configure from source/:
	- cmake --preset=n3ds-devkitarm-bootstrap

Linux:
- Install devkitPro pacman tooling and initialize package metadata.
- Install packages:
	- sudo dkp-pacman -Syu
	- sudo dkp-pacman -S --needed 3ds-dev
- Export environment variables (shell init):
	- export DEVKITPRO=/opt/devkitpro
	- export DEVKITARM=$DEVKITPRO/devkitARM
	- export PATH=$DEVKITARM/bin:$PATH
- Validate toolchain:
	- test -x "$DEVKITARM/bin/arm-none-eabi-gcc"
	- arm-none-eabi-gcc --version
- Configure from source/:
	- cmake --preset=n3ds-devkitarm-bootstrap

Common verification:
- cmake --list-presets includes n3ds-devkitarm-bootstrap.
- The n3ds bootstrap preset uses STAR_N3DS_BOOTSTRAP_ONLY=ON and should configure/build without desktop dependency resolution.
- To start real dependency integration work in Phase 1+, turn STAR_N3DS_BOOTSTRAP_ONLY off and begin gating/replacing desktop libraries.

## Immediate Next Milestones (Current Iteration)

1. Validate preset configuration on a machine with devkitPro installed.
2. Add a first platform macro integration pass in CMake/source headers.
3. Create a small compile target matrix for "core + base + platform stubs" under 3DS preset.
4. Begin renderer abstraction extraction plan before any full backend implementation.

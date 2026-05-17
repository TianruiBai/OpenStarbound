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

### Dual-Screen UI Concepts (Detailed)

This section defines concrete UI operating models for Nintendo 3DS so implementation can proceed without ad-hoc per-screen decisions.

#### 1) Screen Responsibility Model

Primary model (recommended default):
- Top screen (400x240): world view, combat readability, short-lived overlays.
- Bottom screen (320x240 touch): persistent interaction layer (inventory, crafting, hotbar controls, quick actions, mission snippets).

Alternative model (menu-heavy contexts):
- Top screen: static or slowed world preview/background.
- Bottom screen: full interactive menu pane (options, codex, party, map, admin/debug panes).

Switching policy:
- Never move the world render to bottom screen in gameplay.
- Only switch to "bottom-screen-focused" mode for explicit menu contexts.
- Enter/exit transitions should be sub-120 ms and preserve focus state.

#### 2) UI Layout Profiles

Define three global profiles instead of one-off pane scaling:
- Profile A (Exploration):
	- Top: minimal HUD (health/energy/oxygen/status icons).
	- Bottom: compact hotbar + radial quick actions + tap shortcuts.
- Profile B (Combat):
	- Top: larger critical bars, clearer debuff timers, reduced text noise.
	- Bottom: weapon/item quick swap, heal/utility buttons, lock-on and movement aids.
- Profile C (Management):
	- Top: dimmed world or paused backdrop.
	- Bottom: inventory/crafting/quest/chat panels with larger touch targets.

Profile change triggers:
- Exploration <-> Combat from threat/attack state with hysteresis to avoid flicker.
- Any pane that captures text/list navigation forces Management profile.

#### 3) Control And Focus Model

Inputs must be first-class for both buttons and touch:
- Circle Pad: movement or cursor emulation (context-dependent).
- D-pad: deterministic focus navigation in bottom-screen widgets.
- A/B: confirm/cancel in focused widget tree.
- X/Y: context shortcuts (interact, quick stack, compare item).
- L/R: hotbar page shift or tab cycling.
- ZL/ZR (New 3DS): secondary quick actions and pane layer toggles.
- Touch: direct activation, drag-and-drop for item management, scroll regions.

Focus rules:
- Maintain a single authoritative focus owner.
- Touch input steals focus immediately.
- D-pad navigation restores last focus ring in current pane.
- World-action buttons never consume text-entry focus unless explicitly confirmed.

#### 4) Touch-Target And Typography Constraints

Bottom-screen usability constraints:
- Minimum touch target: 24x24 px (absolute minimum), preferred 28x28 px.
- Adjacent action targets need 2-4 px spacing to reduce mis-taps.
- Long-press (>=250 ms) for secondary item actions (split stack, inspect, favorite).

Text constraints:
- Base UI text should remain readable at arm-length with no subpixel assumptions.
- Prioritize iconography + short labels on bottom screen.
- Tooltip strategy: compact two-line summary first, expanded detail on hold.

#### 5) HUD Partition Strategy

Separate gameplay-critical from management-heavy information:
- Keep only critical survivability and immediate combat info on top screen.
- Move dense inventory/meta systems to bottom screen.
- Collapse low-priority counters into expandable bottom-screen clusters.

Recommended top-screen persistent elements:
- HP/Energy/Oxygen bars.
- Active status effects (icon + small timer ring).
- Tiny objective marker and interaction prompt.

Recommended bottom-screen persistent elements:
- Hotbar with touch-select.
- Quick consume and utility actions.
- Context-sensitive interaction button set.
- Expand/collapse mission card.

#### 6) Inventory And Crafting Interaction Patterns

Inventory pattern:
- Grid interaction is touch-first, button-navigable second.
- Tap = select, drag = move, double-tap = quick transfer.
- Hold item opens radial action menu (drop, split, inspect, favorite, move-to-container).

Crafting pattern:
- Left region: category tabs and filters.
- Right region: recipe preview and craft controls.
- Bottom row: queue controls (craft 1/5/max, cancel queue).

Failure handling:
- If recipe list is too long, prioritize search presets over full keyboard entry.
- Keep all destructive actions behind confirmation affordances.

#### 7) Quest, Chat, And Notifications

Quest UX:
- Bottom-screen mission card with single-tap expand/collapse.
- Pin/unpin objective from quick mission tray.
- Route guidance stays top screen as minimal iconized hints.

Chat UX:
- Chat is off by default in handheld profile unless enabled.
- Quick-chat templates should be mapped to radial wheel and shoulder modifiers.
- On-screen keyboard invoked only on explicit user action.

Notifications:
- World-critical alerts on top screen.
- System/log/mod notices to bottom-screen feed.
- Batch non-critical notifications to fixed intervals to reduce distraction.

#### 8) Rendering And Performance Policy For UI

UI rendering constraints:
- Prefer sprite-atlas UI elements; avoid frequent texture state churn.
- Keep bottom-screen UI animations to low-cost transforms/fades.
- Use capped update rates for non-critical widgets when frame budget is tight.

Budget targets (initial guidance):
- UI update + draw should stay within a bounded per-frame budget independent of world complexity.
- Graceful degradation path: reduce animation density, then shadow/outline quality, then non-critical overlays.

#### 9) Accessibility And Ergonomics Considerations

- Offer left-handed control swap for shoulder and quick-action mappings.
- Provide at least two bottom-screen scale presets (compact, comfort).
- Add optional high-contrast icon pack for status and quick-action symbols.
- Add hold-to-confirm option for destructive inventory actions.

#### 10) Implementation Breakdown (Phase 4 Sub-Phases)

Phase 4A - Foundation:
- Add UI profile state machine (Exploration, Combat, Management).
- Implement shared focus manager with touch + D-pad arbitration.
- Add dual-screen layout descriptors and safe-area constants.

Phase 4B - Core interaction:
- Port hotbar + quick actions to bottom screen.
- Implement inventory touch interactions and radial action menu.
- Add basic mission card and compact notifications.

Phase 4C - Menu and text workflows:
- Implement options/crafting/profile panes in Management profile.
- Add chat quick templates and explicit keyboard invocation.
- Integrate focus persistence across pane transitions.

Phase 4D - Polish and tuning:
- Optimize animation/update budget under stress.
- Tune target sizes and spacing from hardware playtests.
- Finalize profile switch heuristics to avoid mode thrashing.

Definition of done for dual-screen UI concept implementation:
- Gameplay loop is fully operable without keyboard/mouse assumptions.
- Inventory and crafting are touch-usable with acceptable error rate.
- Top screen remains readable during combat while bottom screen keeps interaction depth.
- UI profile transitions are stable and do not drop critical inputs.

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
	- pacman -S --needed 3ds-zlib 3ds-libpng 3ds-freetype 3ds-curl 3ds-libogg 3ds-libopus 3ds-libvorbisidec 3ds-libzstd
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
- Use cmake --preset=n3ds-devkitarm-phase1 for non-bootstrap dependency-gating checks.

## Immediate Next Milestones (Current Iteration)

1. Validate preset configuration on a machine with devkitPro installed.
2. Add a first platform macro integration pass in CMake/source headers.
3. Create a small compile target matrix for "core + base + platform stubs" under 3DS preset.
4. Begin renderer abstraction extraction plan before any full backend implementation.

## Phase 1 Progress Snapshot (Branch 3ds-port)

Completed:
- STAR_PLATFORM_N3DS is now a first-class CMake option and compile definition.
- Nintendo 3DS platform mode now force-disables Steam/Discord/Qt desktop integrations.
- Runtime platform capability reporting API has been added to ApplicationController.
- Mods menu desktop-link behavior now checks capability reporting instead of direct service probing.
- Placeholder platform services have been added for statistics, workshop UGC, and desktop URL open.
- 3DS audio input capture path now has explicit STUB/PLACEHOLDER markers and capability-off behavior.

Stub tracking rule (mandatory):
- Every stubbed behavior must include a code comment containing either STUB or PLACEHOLDER near the implementation.
- Every new stub must be listed in this document under "Stub Inventory" until replaced.
- When replacing a stub with a real implementation, remove the marker comment and update the inventory entry status.

Stub Inventory (current):
- application/StarPlatformServices_stub.cpp: PlaceholderStatisticsService (PLACEHOLDER)
- application/StarPlatformServices_stub.cpp: PlaceholderUserGeneratedContentService (PLACEHOLDER)
- application/StarPlatformServices_stub.cpp: StubDesktopService (STUB)
- application/StarMainApplication_sdl.cpp: openAudioInputDevice/closeAudioInputDevice/supportsAudioInput for STAR_PLATFORM_N3DS (STUB/PLACEHOLDER)
- frontend/StarOptionsMenu.cpp: showVoiceSettings button disabled when audio input capability is unavailable (STUB)
- frontend/StarOptionsMenu.cpp: displayVoiceSettings early return for unsupported audio input path (PLACEHOLDER)
- frontend/StarVoiceLuaBindings.cpp: fallback voice callback set when Voice singleton is unavailable (STUB)
- core/StarHttpClient_stub.cpp: HTTP request path replaced with platform placeholder responses for STAR_PLATFORM_N3DS (STUB)
- core/StarAudio_stub.cpp: handheld phase1 audio decode path is placeholder-only, returning empty reads (STUB/PLACEHOLDER)

Next in Phase 1:
- Expand capability-gated UI affordances in options/voice/settings panes.
- Add a non-bootstrap n3ds configure profile used for progressive dependency gating tests.
- Replace placeholder services incrementally with real 3DS-native implementations as subsystems come online.

Phase 1 preset set:
- n3ds-devkitarm-bootstrap: toolchain/bootstrap-only validation path.
- n3ds-devkitarm-phase1: non-bootstrap dependency-gating path used to progressively replace desktop assumptions.

Current phase1 dependency blockers (if preset fails):
- pkg-config not found in PATH (requires devkitPro msys2/usr/bin visibility).
- missing 3ds portlibs packages (zlib/libpng/freetype/curl/libogg/libopus/libvorbisidec/libzstd).

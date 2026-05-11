# OpenStarbound Client UI Architecture

This document expands the client presentation and interface parts of `doc/ARCHITECTURE.md`.

It is intended for engineers working on menus, HUD behavior, panes, interface rendering, input flow, or client-only presentation logic.

## 1. Scope

The client UI stack is not a browser-style view layer. It is a game-native UI stack built directly on the runtime engine.

It combines:

- application and window lifecycle
- renderer initialization
- GUI context and widget toolkit
- pane management and modal layering
- game-specific screens and HUD elements
- direct coupling to live gameplay objects such as `UniverseClient`, `WorldClient`, `Player`, and `WorldPainter`

## 2. Top-Level Composition

### 2.1 `ClientApplication`

`source/client/StarClientApplication.*` is the client composition root.

It owns or coordinates:

- `Root`
- `MainMixer`
- `GuiContext`
- `Input`
- `Voice`
- `WorldPainter`
- `UniverseClient`
- `TitleScreen`
- `MainInterface`
- optional local `UniverseServer` for single-player

It also manages the client application state machine.

### 2.2 State machine

The client state machine includes:

- `Startup`
- `SteamFlatpakWarning`
- `Mods`
- `ModsWarning`
- `Splash`
- `Error`
- `Title`
- `SinglePlayer`
- `MultiPlayer`

Planning implication: many UI changes are really state-transition changes. If a screen should appear before or after a connection or world boot event, `ClientApplication` is usually part of the solution.

## 3. Layered UI Stack

### 3.1 Application and renderer layer

Relevant code lives in:

- `source/application/`
- `source/client/StarClientApplication.*`

Responsibilities:

- create the OS window and graphics context
- configure fullscreen, borderless, maximized, vsync, and cursor mode
- initialize audio and renderer state
- bootstrap mods before asset-dependent UI work starts

### 3.2 GUI context layer

`source/windowing/StarGuiContext.*` is the client-wide GUI service singleton.

Responsibilities include:

- access to renderer, mixer, and application controller
- interface scale and display scaling
- keybinding lookup and input-to-interface action mapping
- interface-space drawing primitives
- text rendering and font selection
- clipboard access and cursor handling
- GUI audio playback

Planning implication: this is the central place for shared UI services, not the place for feature-specific screen logic.

### 3.3 Widget and pane framework

`source/windowing/` provides the reusable UI framework.

Core abstractions:

- `Widget`
- `Pane`
- `PaneManager`
- `RegisteredPaneManager`
- layout widgets and controls
- `GuiReader` and widget parsing helpers

Planning implication: use this layer when changing reusable UI primitives or pane lifecycle behavior.

### 3.4 Game-specific interface layer

`source/frontend/` turns the framework into the actual Starbound interface set.

Important examples:

- `TitleScreen`
- `MainInterface`
- `InventoryPane`
- `CraftingPane`
- `OptionsMenu`
- `QuestLogInterface`
- `Chat`
- `TeleportDialog`
- `WirePane`
- `RadioMessagePopup`

Planning implication: feature-specific UI work usually starts here, but often depends on live runtime objects from `source/game/`.

## 4. Main Runtime Screens

### 4.1 Title and pre-game flow

Title and pre-game screens live near the client app state machine.

They are responsible for:

- title flow
- single-player and multiplayer launch selection
- settings handoff into live gameplay
- mod and warning screens before gameplay begins

### 4.2 Main in-game interface

`source/frontend/StarMainInterface.*` is the in-game interface coordinator.

Responsibilities include:

- HUD rendering
- handling interface input and focus
- pane display and dismissal
- chat and command entry
- script-pane lifecycle
- quest and radio overlays
- cursor and tooltip behavior
- bridge between world rendering and interface rendering

Its main dependencies show why this layer is tightly coupled:

- `UniverseClient`
- `WorldPainter`
- `Cinematic`
- pane and widget types
- chat, command, wire, quest, and item UI systems

Planning implication: many UI features that appear independent are actually coordinated through `MainInterface`.

## 5. Pane and Layer Model

`PaneManager` defines several pane layers:

- `Tooltip`
- `ModalWindow`
- `Window`
- `Hud`
- `World`

This is an important architectural decision. The UI does not only consist of windows. It also includes:

- persistent HUD elements
- world-adjacent UI such as wiring overlays
- blocking modal windows
- transient tooltips

`PaneManager` responsibilities include:

- z-order management within a layer
- pane display and dismissal
- keyboard capture routing
- hit testing
- render and update ordering
- repositioning behavior when interface scale changes

Planning implication: when changing interface behavior, identify the correct pane layer first. A HUD issue and a modal-window issue usually belong in different parts of the system.

## 6. Input Flow

At a high level, input flows through the following steps:

1. OS and SDL input enters the application layer
2. `ClientApplication` receives or forwards input
3. `GuiContext` maps keys to interface actions and tracks interface-scale-aware mouse positions
4. `PaneManager` routes events to the focused or topmost relevant pane
5. `MainInterface` or another screen decides whether gameplay, chat, or UI should consume the event

Important consequence:

- input ownership is shared between gameplay and UI, not isolated in the widget layer alone

Planning implication: a change to hotkeys, chat capture, or cursor behavior often spans `ClientApplication`, `GuiContext`, `PaneManager`, and `MainInterface` together.

## 7. Rendering Flow

The client UI stack is mixed with world rendering rather than rendered as a separate detached surface.

Typical high-level ordering is:

1. gameplay and client state update
2. world render data preparation
3. render world scene with `WorldPainter`
4. render in-world interface elements such as bubbles or markers
5. render panes, HUD, overlays, messages, cursor, and debug UI

Planning implication: UI changes can affect world readability, and world-render changes can affect UI anchoring or interaction assumptions.

## 8. Data and Asset Dependencies

The UI is heavily asset-driven.

Important dependency surfaces include:

- `interface.config` and related asset JSON
- textures, fonts, sounds, and pane definitions under `assets/`
- OpenStarbound overlay assets under `assets/opensb/`
- Lua bindings for widget and interface behavior

This means many UI changes require coordinated changes in:

- `frontend/` C++ code
- `windowing/` framework behavior
- asset definitions and images
- optionally Lua callbacks and script panes

## 9. Common Change Scenarios

### 9.1 Add a new HUD element

Usually touches:

- `MainInterface`
- a new or existing pane/widget type
- `GuiContext` drawing helpers only if shared rendering support is needed
- asset definitions or textures

### 9.2 Add a new window or menu

Usually touches:

- a new frontend pane class
- pane registration or display logic
- assets for layout, buttons, and strings
- relevant runtime object APIs on `UniverseClient`, `Player`, or `WorldClient`

### 9.3 Change input handling or focus behavior

Usually touches:

- `ClientApplication`
- `GuiContext`
- `PaneManager`
- target pane or `MainInterface`

### 9.4 Refactor interface rendering

Usually touches:

- `MainInterface`
- pane classes
- widget-level drawing assumptions
- `WorldPainter` only if world-space UI is involved

## 10. Review Questions

- Is this change part of title flow, in-game HUD, or an independent modal/window pane?
- Does the change belong in `ClientApplication`, `GuiContext`, `PaneManager`, or `frontend/` feature code?
- Does the feature need world-space rendering or interface-space rendering?
- Which object owns the data: `UniverseClient`, `WorldClient`, `Player`, or a pane-local model?
- Does the feature need asset updates, Lua bindings, or only C++ changes?
- What captures input while this UI is open?
- Does the change affect interface scale, cursor behavior, or controller input assumptions?

## 11. Practical Takeaways

- `ClientApplication` owns screen-level state and process-level orchestration.
- `GuiContext` is the shared UI service layer.
- `PaneManager` is the pane lifecycle and focus layer.
- `MainInterface` is the in-game interface coordinator and a major coupling hub.
- UI work is usually a mix of C++ logic, assets, and live gameplay object integration.
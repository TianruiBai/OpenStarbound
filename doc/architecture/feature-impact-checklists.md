# OpenStarbound Feature-Impact Checklists

This document is meant to help the team estimate scope before making code changes.

The goal is not to predict every file up front. The goal is to force the right architectural questions early enough that the team does not discover half the change surface after implementation has started.

## 1. How To Use This Document

For each planned change:

1. classify the change by primary surface
2. run the matching checklist
3. mark required secondary surfaces before coding starts
4. record which validation path will prove the change is safe

Use these primary-surface tags:

- gameplay simulation
- networking
- persistence/versioning
- client UI
- assets and scripting
- platform integration
- tooling and pipeline

## 2. Baseline Checklist For Any Change

- [ ] Is the change client-only, server-only, or both?
- [ ] Does it need to work in both dedicated server and single-player?
- [ ] Does it touch a `Root`-owned database or configuration value?
- [ ] Does it affect assets, Lua bindings, or `assets/opensb/` overlay content?
- [ ] Does it alter persisted data, even indirectly?
- [ ] Does it alter packet shape, sync behavior, or compatibility assumptions?
- [ ] Does it cross a thread boundary?
- [ ] Is there an existing narrow test or validation path for the touched slice?

## 3. Adding a New Entity

Use this when introducing a new runtime entity type or a substantially new persistent/runtime variant of an existing one.

- [ ] Decide whether the new entity is authoritative server state, client-only, or both.
- [ ] Identify the base interface family it belongs to in `source/game/interfaces/`.
- [ ] Check `EntityFactory` registration and load/store behavior.
- [ ] Decide whether the entity must persist to disk.
- [ ] Decide whether the entity needs versioned store/load support.
- [ ] Check whether the entity needs asset-driven data definitions, database registration, or both.
- [ ] Check whether the entity needs Lua bindings or message handling.
- [ ] Check whether the entity is visible to UI, quest logic, chat logic, or world interaction code.
- [ ] Check whether the entity emits networked state or receives replicated commands.
- [ ] Check whether world generation, placement, or spawn systems must know about it.
- [ ] Decide how the entity behaves when assets are missing or old saves are loaded.
- [ ] Plan validation for spawn, persistence, reload, and multiplayer behavior.

### Typical impact areas

- `source/game/StarEntityFactory.*`
- entity class and related interfaces
- relevant content databases in `source/game/`
- `assets/` or `assets/opensb/` entity definitions
- packet or sync code if replicated
- versioning paths if persisted
- frontend/UI if surfaced to players

## 4. Changing Multiplayer Sync

Use this when changing packet structure, replicated fields, connection flow, or world-state sync semantics.

- [ ] Decide whether this is a handshake, universe, world, or system-world change.
- [ ] Identify the owning packet or sync structure.
- [ ] Update both read and write paths on both sides.
- [ ] Check whether `StarProtocolVersion` or compatibility assumptions must change.
- [ ] Check whether local single-player uses the same path.
- [ ] Identify which server thread or world thread owns the authoritative mutation.
- [ ] Check whether packet ordering or world-switch timing can invalidate the change.
- [ ] Check whether any UI assumes immediate local effects for a server-authoritative action.
- [ ] Check whether the same state is also saved to disk and therefore needs migration review.
- [ ] Check whether scripting surfaces expose or depend on the replicated shape.
- [ ] Plan validation for connect, disconnect, world transfer, and error handling.

### Typical impact areas

- `source/game/StarNetPackets.*`
- `source/game/StarUniverseConnection.*`
- `source/game/StarUniverseServer.*`
- `source/game/StarUniverseClient.*`
- `source/game/StarWorldServer.*`
- `source/game/StarWorldClient.*`

## 5. Refactoring UI

Use this when changing pane structure, HUD layout, menu flow, cursor/input routing, or reusable widget behavior.

- [ ] Decide whether the change belongs in `ClientApplication`, `GuiContext`, `PaneManager`, or a specific `frontend/` pane.
- [ ] Decide whether the UI is title-flow, modal window, regular window, HUD, or world-layer UI.
- [ ] Check keyboard and text-input capture behavior.
- [ ] Check controller and mouse behavior.
- [ ] Check interface scale and window-resize behavior.
- [ ] Check whether assets, fonts, icons, or pane JSON need changes.
- [ ] Check whether Lua bindings or script panes depend on the old UI contract.
- [ ] Check whether the UI is reading directly from `UniverseClient`, `WorldClient`, or `Player` and whether those assumptions remain valid.
- [ ] Check whether the change affects debug overlays or in-world markers.
- [ ] Plan validation for interaction, focus, resizing, and gameplay coexistence.

### Typical impact areas

- `source/client/StarClientApplication.*`
- `source/windowing/`
- `source/frontend/`
- `source/rendering/` if world-space interface rendering is involved
- `assets/opensb/interface/` or related asset configs

## 6. Changing Save Format or Versioning

Use this when modifying persistent JSON shape, file naming, migration behavior, or versioned entity storage.

- [ ] Identify whether the persisted surface is player, shipworld, world, universe, or embedded versioned JSON.
- [ ] Identify the logical version identifier used by `VersioningDatabase`.
- [ ] Decide whether migration is required for old data.
- [ ] Update store and load paths together.
- [ ] Check whether backup, delete, or metadata bookkeeping must change.
- [ ] Check whether utility tools read or write the same payload family.
- [ ] Check whether the changed persisted state is also replicated over the network.
- [ ] Plan validation against both fresh saves and older saves.

## 7. Changing Asset or Mod Loading

Use this when changing how assets are discovered, patched, cached, or exposed to runtime systems.

- [ ] Decide whether the change belongs in `RootLoader`, `Root`, `Assets`, or asset overlay content.
- [ ] Check whether source ordering or patch ordering changes.
- [ ] Check digest and multiplayer compatibility implications.
- [ ] Check hot-reload and mod reload behavior.
- [ ] Check missing-asset fallback behavior.
- [ ] Check whether asset changes alter Lua or UI surfaces indirectly.
- [ ] Plan validation for clean boot, reload, and mixed asset source sets.

## 8. Changing World Generation or Storage

Use this when changing sector generation, activation, unload, or world file structure.

- [ ] Decide whether the change belongs in `WorldServer`, `WorldStorage`, generation helpers, or biome/dungeon content.
- [ ] Check sector generation stage assumptions.
- [ ] Check TTL, unload, and sync behavior.
- [ ] Check unique-entity indexing assumptions.
- [ ] Check persisted world metadata versioning.
- [ ] Check server performance and worker-pool load implications.
- [ ] Plan validation for world creation, revisit, unload, and reload.

## 9. Exit Criteria Template

Use this short template before merging a feature.

- [ ] The owning subsystem and secondary subsystems were identified before coding.
- [ ] All packet, persistence, asset, and UI surfaces touched by the change were reviewed.
- [ ] A focused validation path was run.
- [ ] Backward-compatibility or migration impact was explicitly decided.
- [ ] Single-player and dedicated-server expectations were explicitly checked where relevant.
- [ ] The team can name the rollback plan if the change breaks saves, networking, or assets.
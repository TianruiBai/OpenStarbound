# OpenStarbound Persistence and Versioning Architecture

This document expands the persistence and migration parts of `doc/ARCHITECTURE.md`.

It is intended for engineers changing saved gameplay state, world storage, player files, or versioned content migration behavior.

## 1. Scope

Persistence in OpenStarbound is not a single subsystem with one storage format. It is a group of related mechanisms:

- runtime configuration persistence
- player and ship persistence
- universe and celestial persistence
- world paging and sector-backed persistence
- versioned JSON wrappers and migration scripts
- entity-specific versioned store and load behavior

The most important design fact is that persistence and versioning are distributed across gameplay and infrastructure code, but they all converge through `Root`, `VersioningDatabase`, and storage helpers.

## 2. Main Storage Surfaces

### 2.1 Runtime config and root-owned settings

`Root` is responsible for loading configuration and optionally writing changed runtime configuration back to disk.

This is not the same as gameplay save data. It is the persistence surface for:

- client and server runtime settings
- log and storage paths
- boot-time merged configuration state

Planning implication: config changes can be persistent even if no gameplay save format changes are involved.

### 2.2 Player storage

`source/game/StarPlayerStorage.*` is the main player-file manager.

Responsibilities include:

- scanning the player storage directory
- loading `.player` files as `VersionedJson`
- validating and caching player JSON by UUID
- loading player runtime objects through `EntityFactory`
- writing `.player` files on save
- storing and patching `.shipworld` data via world-chunk update helpers
- maintaining `metadata` ordering and player list state
- managing backup rotation

Practical file surfaces managed here include:

- `<uuid or fileName>.player`
- `<uuid or fileName>.shipworld`
- `metadata`
- backup copies under the backup directory

Planning implication: player-facing changes often need both player JSON handling and shipworld handling reviewed.

### 2.3 World storage

`source/game/StarWorldStorage.*` provides disk-backed world paging and staged sector persistence.

This class is not only a serializer. It also coordinates:

- sector load and unload
- sector generation progression
- entity storage per sector
- tile storage per sector
- unique entity indexing
- TTL-based unload behavior
- commit and sync behavior for the underlying database

Important structural detail:

- worlds are stored through a `BTreeDatabase`
- tiles, entities, unique indexes, and metadata are separate store types
- metadata itself is wrapped in `VersionedJson`

Planning implication: changing what the world stores or when it stores it can affect generation, runtime memory pressure, and failure recovery at once.

### 2.4 Universe and celestial persistence

`UniverseServer` owns the storage directory for universe-level files and acquires `universe.lock` for exclusive access.

Universe-level persistence includes:

- universe settings
- temporary world index data
- celestial chunk storage such as `universe.chunks`
- world files under the universe storage root

Planning implication: server lifecycle changes can affect storage locking, cleanup, and file ownership assumptions.

## 3. Versioned JSON Model

### 3.1 `VersionedJson`

`source/game/StarVersioningDatabase.hpp` defines `VersionedJson`.

It stores:

- identifier
- version number
- content JSON
- sub-version map

It supports:

- binary file read and write with a magic header
- JSON embedding for nested versioned structures
- expected identifier validation
- sub-version serialization support

This allows the project to persist gameplay objects in a format that can be migrated forward instead of requiring one frozen schema forever.

### 3.2 `VersioningDatabase`

`VersioningDatabase` is the migration engine.

Responsibilities:

- map logical identifiers to current versions
- create current version wrappers for outgoing saved content
- detect whether saved content is current
- run update scripts to migrate older content forward
- load versioned JSON and return up-to-date content

Important internal detail:

- it owns a `LuaRoot`
- migration behavior is script-driven rather than entirely hardcoded in C++

Planning implication: schema evolution is deliberately data-driven. Migration work is part C++ integration, part versioning-script maintenance.

## 4. Where Versioning Is Applied

Versioning is not only for one save file type.

Examples visible in the codebase include:

- player entities
- item descriptors
- quest descriptors
- celestial chunks
- world metadata
- entity persistence through `EntityFactory`
- Lua-exposed versioning helpers via root bindings

This means a change to the versioning system itself has broad blast radius, even when the immediate feature looks local.

## 5. Entity Persistence Pipeline

The entity persistence path is one of the most important integration flows.

High-level path:

1. gameplay entity produces disk store JSON
2. `EntityFactory` wraps it in a `VersionedJson` using a logical entity identifier
3. storage layer writes the versioned payload
4. on load, the versioned payload is read
5. `VersioningDatabase` updates it if necessary
6. `EntityFactory` reconstructs the entity from the migrated JSON

Planning implication: entity changes need to be reviewed as both runtime behavior changes and serialized schema changes.

## 6. Player Storage Lifecycle

The player storage lifecycle is worth treating separately because it combines caching, validation, migration, and backup behavior.

### 6.1 Startup load

At construction, `PlayerStorage`:

- ensures the storage directory exists
- optionally clears player files if configured
- reads each `.player` file as `VersionedJson`
- loads the player JSON through `EntityFactory`
- caches valid player data by UUID
- validates that loaded player runtime UUID matches stored identity
- reads `metadata` to restore ordering

### 6.2 Save path

When saving a player, `PlayerStorage`:

- gets disk-store JSON from the player object
- compares it against cached JSON
- uses `EntityFactory` to wrap it as versioned player data
- writes the versioned `.player` file
- preserves stable file naming through UUID-to-file-name mapping

### 6.3 Ship data path

Ship persistence is kept separate from player JSON.

It uses:

- `WorldStorage::getWorldChunksFromFile`
- `WorldStorage::applyWorldChunksUpdateToFile`

Planning implication: player save changes and ship save changes are related, but not identical, workstreams.

### 6.4 Backups and deletion

`PlayerStorage` also owns:

- backup cycle rotation for player, shipworld, and metadata files
- delete behavior for current and backup copies

Planning implication: when changing file names, storage layout, or metadata semantics, cleanup and backup logic must be reviewed too.

## 7. World Storage Lifecycle

World persistence behaves differently from player persistence because worlds are live, paged simulations.

### 7.1 Construction modes

`WorldStorage` can be constructed from:

- new world size and device
- existing device-backed world
- in-memory `WorldChunks`

### 7.2 Stored domains

The store types include:

- metadata
- tile sectors
- entity sectors
- unique entity indexes
- sector-unique mappings

### 7.3 Runtime behavior tied to persistence

`WorldStorage` persistence is tightly coupled with runtime behavior such as:

- generation queue advancement
- TTL expiry
- lazy load and full activation
- unloading all sectors
- sync and commit behavior

Planning implication: storage is part of simulation control flow, not only end-of-session save.

## 8. Versioning Change Types

The following change categories are useful for planning.

### 8.1 Additive JSON change

Examples:

- add an optional field with safe default
- add metadata not required by older loaders

Risk:

- medium, but still requires migration review if older saves must surface the field consistently

### 8.2 Semantic state change

Examples:

- change meaning of an existing field
- split one field into several
- move data to another structure

Risk:

- high, because migration must preserve old behavior or intentionally transform it

### 8.3 Storage layout change

Examples:

- rename files
- move data between `.player` and `.shipworld`
- change world sector representation

Risk:

- very high, because migration must cover both schema and file layout expectations

## 9. Recommended Edit Workflow for Persistence Changes

1. Identify the persisted surface: config, player, shipworld, world, universe, celestial, or embedded versioned JSON.
2. Identify the logical identifier used by `VersioningDatabase`.
3. Decide whether old saves must load losslessly, approximately, or not at all.
4. Update C++ store and load paths together.
5. Update migration scripts or version tables where needed.
6. Review any utility tools that read or write the same payload family.
7. Review backup, delete, or metadata-side behavior if file layout changed.
8. Run narrow validation on both fresh saves and old saves when possible.

## 10. Review Questions

- Which file or store type is the source of truth for this data?
- Is the data wrapped in `VersionedJson`, and under what identifier?
- Do old saves need migration, or can the field safely default?
- Does this data also cross the network at runtime?
- Does the UI assume the field exists after load?
- Do backup or delete paths need updating?
- Do tooling commands or Lua bindings expose the same serialized shape?

## 11. Practical Takeaways

- Persistence is distributed, but versioning is centralized around `VersionedJson` plus `VersioningDatabase`.
- Player persistence and world persistence have different operational models and should not be treated as the same kind of save work.
- World storage is a live paging subsystem, not just a save file serializer.
- Entity and content changes often need migration support even when the runtime change looks small.
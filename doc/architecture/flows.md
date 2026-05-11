# OpenStarbound Architecture Flows

This document adds concrete flow diagrams to the higher-level architecture guide.

The diagrams are intentionally operational rather than theoretical. They are meant to help engineers trace ownership and lifecycle across the most expensive change paths.

## 1. Single-Player Boot Flow

This flow shows how the client boots, loads content, and then starts a local server while still using the shared client/server networking model.

```mermaid
flowchart TD
    A[Process start] --> B[ClientApplication.startup]
    B --> C[RootLoader builds Root]
    C --> D[ClientApplication.applicationInit]
    D --> E[loadMods before asset-heavy UI init]
    E --> F[Create mixer, GuiContext, Input, Voice, WorldPainter]
    F --> G[ClientApplication.renderInit]
    G --> H[Enter title and pre-game states]
    H --> I[Player chooses single-player]
    I --> J[Create PlayerStorage and client runtime state]
    J --> K[Create local UniverseServer]
    K --> L[UniverseServer.addLocalClient]
    L --> M[LocalPacketSocket pair created]
    M --> N[UniverseClient connects to local UniverseConnection]
    N --> O[Handshake and connect packets]
    O --> P[UniverseServer accepts client and creates context]
    P --> Q[World or system world becomes active]
    Q --> R[MainInterface and WorldPainter render live gameplay]
```

Critical notes:

- Single-player still uses most of the same packet and routing paths as multiplayer.
- Mod loading happens before asset-heavy client initialization.
- Client UI and world rendering are booted before gameplay begins, but after root and configuration setup.

## 2. Server World Lifecycle

This flow shows how `UniverseServer`, `WorldServerThread`, and `WorldServer` cooperate to create, run, and retire active worlds.

```mermaid
flowchart TD
    A[UniverseServer running] --> B[Client needs world access or warp target]
    B --> C[UniverseServer resolve target WorldId]
    C --> D{World already active?}
    D -- Yes --> E[Reuse existing WorldServerThread]
    D -- No --> F[Trigger world creation via worker pool]
    F --> G[Create or load WorldStorage]
    G --> H[Construct WorldServer]
    H --> I[Wrap in WorldServerThread]
    I --> J[Start world thread]
    E --> K[Attach client to world thread]
    J --> K
    K --> L[WorldServerThread queues incoming packets]
    L --> M[WorldServer authoritative update loop]
    M --> N[Outgoing packets and world update callbacks]
    N --> O[UniverseServer forwards packets to clients]
    M --> P{No clients or expiry condition?}
    P -- No --> L
    P -- Yes --> Q[Unload or sync sectors]
    Q --> R[World thread stops]
    R --> S[UniverseServer removes world from active set]
```

Critical notes:

- World persistence and world simulation are coupled through `WorldStorage`.
- `UniverseServer` decides world ownership and routing, but `WorldServer` owns authoritative world mutation.
- Unload behavior is part of the runtime loop, not just process shutdown behavior.

## 3. Asset and Mod Loading Flow

This flow shows how boot-time content sources become live assets and runtime databases.

```mermaid
flowchart TD
    A[Boot config and command-line options] --> B[RootLoader merges defaults and boot settings]
    B --> C[Root settings built]
    C --> D[Scan asset directories and asset sources]
    D --> E[Construct Assets with source list and cache settings]
    E --> F[Client or server code requests config/assets]
    F --> G[Assets resolve source precedence]
    G --> H[Directory or packed source opened]
    H --> I{Patch or post-process needed?}
    I -- Yes --> J[Apply JSON or image patch chain]
    I -- No --> K[Use raw asset content]
    J --> L[Cache asset with TTL metadata]
    K --> L
    L --> M[Root lazy-loads databases using asset data]
    M --> N[Gameplay, UI, scripting, and rendering consume databases]
    C --> O[Optional mod directories loaded or reloaded]
    O --> D
```

Critical notes:

- Asset source ordering matters because it determines override behavior.
- `Assets` handles patching and caching, not just raw file reads.
- Many runtime systems only become concrete when `Root` lazy-loads a database on first access.

## 4. How To Use These Diagrams In Planning

When a change is proposed, start by choosing the flow it belongs to most strongly.

- Single-player boot and startup behavior: use the single-player boot diagram.
- World creation, routing, and unload logic: use the server world lifecycle diagram.
- Content bootstrap, mods, patches, and runtime data availability: use the asset and mod loading diagram.

If a feature crosses two diagrams, assume the change is medium or high risk and plan validation earlier.
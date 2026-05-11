# OpenStarbound Networking Architecture

This document drills into the networking subsystem described at a high level in `doc/ARCHITECTURE.md`.

It is intended for engineers changing multiplayer behavior, packet formats, connection handling, world sync, or client/server coordination.

## 1. Scope

The networking subsystem in OpenStarbound spans more than the socket layer. It includes:

- packet transport and framing
- protocol definition
- handshake and compatibility checks
- universe-level routing
- world-level routing
- local single-player loopback connections
- packet-driven client state updates
- server-side multi-threaded receive and send processing

The main architectural point is that network logic is split across several layers with different responsibilities.

## 2. Main Components

### 2.1 Transport and packet framing

The lowest networking abstractions used by game code are centered around:

- `source/core/StarTcp.*`
- `source/core/StarSocket.*`
- `source/game/StarNetPacketSocket.*`
- `source/game/StarUniverseConnection.hpp`

`UniverseConnection` is the primary game-facing connection abstraction. It wraps a `PacketSocket` and owns explicit send and receive queues. It supports:

- queued packet send
- queued packet receive
- blocking and non-blocking flush/receive methods
- packet statistics when available from the socket implementation

This is the abstraction used by both remote multiplayer and local in-process single-player connections.

### 2.2 Connection multiplexing

`UniverseConnectionServer` manages many `UniverseConnection` instances concurrently.

Key roles:

- owns worker threads for packet processing
- tracks per-connection queues and last activity time
- invokes asynchronous packet receive callbacks
- hides most transport concurrency from `UniverseServer`

This means `UniverseServer` is not directly polling raw sockets for every packet; it reacts to packet batches delivered through the connection server.

### 2.3 Protocol definition

`source/game/StarNetPackets.hpp` defines the logical protocol.

The protocol is grouped into several traffic classes:

- handshake packets
- universe server to client packets
- universe client to server packets
- bidirectional universe packets
- world server to client packets
- world client to server packets
- bidirectional world packets
- system world packets
- OpenStarbound-specific packets

This grouping matters because the same connection carries different categories of traffic during different gameplay phases.

### 2.4 Server routing hub

`source/game/StarUniverseServer.*` is the main server-side coordination hub for networking.

It is responsible for:

- accepting remote and local connections
- assigning client IDs
- maintaining client context
- forwarding packet traffic to active worlds or system worlds
- handling chat, commands, disconnects, warps, and context updates
- broadcasting pause, clock, and server info updates

The server owns the connection-to-world mapping indirectly through client context and active world assignment.

### 2.5 Client routing hub

`source/game/StarUniverseClient.*` is the main client-side routing hub.

It is responsible for:

- connecting to a `UniverseConnection`
- processing incoming packet batches
- maintaining current `WorldClient` or `SystemWorldClient`
- tracking disconnect reason and server info
- exposing gameplay-facing actions such as warp, fly ship, and chat

The client does not treat the network as a separate service boundary; networking is tightly tied to the active gameplay model.

### 2.6 World-level packet ownership

Once a client is associated with a world, a large share of traffic becomes world-scoped.

Relevant types:

- `source/game/StarWorldServer.hpp`
- `source/game/StarWorldServerThread.hpp`
- `source/game/StarWorldClient.hpp`

`WorldServerThread` owns:

- incoming packet queues per client for a given world
- outgoing packet queues per client for that world
- execution of the `WorldServer` simulation in its own thread
- update callbacks back to `UniverseServer`

This is the key boundary between universe-level coordination and authoritative world simulation.

## 3. Packet Taxonomy

The following split is useful when planning changes.

### 3.1 Session and handshake packets

Examples:

- `ProtocolRequest`
- `ProtocolResponse`
- `ClientConnect`
- `ConnectSuccess`
- `ConnectFailure`
- `HandshakeChallenge`
- `HandshakeResponse`
- `ServerDisconnect`

These packets establish compatibility, authentication-like challenge flow, and initial session state.

### 3.2 Universe packets

Examples:

- `UniverseTimeUpdate`
- `CelestialResponse`
- `PlayerWarpResult`
- `PlanetTypeUpdate`
- `Pause`
- `ServerInfo`
- `ClientContextUpdate`
- `PlayerWarp`
- `FlyShip`
- `ChatSend`
- `ChatReceive`

These packets exist above any one world and deal with player routing, celestial data, team and universe state, and shared session information.

### 3.3 World packets

Examples:

- `WorldStart`
- `WorldStop`
- `WorldLayoutUpdate`
- `WorldParametersUpdate`
- `TileArrayUpdate`
- `TileUpdate`
- `EntityCreate`
- `EntityUpdateSet`
- `EntityDestroy`
- `EntityInteract`
- `DamageRequest`
- `StepUpdate`

These packets drive the active-world gameplay loop.

### 3.4 System world packets

Examples:

- `SystemWorldStart`
- `SystemWorldUpdate`
- `SystemObjectCreate`
- `SystemShipCreate`

These support star-system level views and movement outside normal world surfaces.

### 3.5 OpenStarbound extension packets

Examples:

- `ReplaceTileList`
- `UpdateWorldTemplate`

These are examples of protocol expansion surfaces already used by the fork.

## 4. End-to-End Data Flow

### 4.1 Remote multiplayer connect path

The typical remote connect path is:

1. client creates a `UniverseConnection`
2. client sends protocol and connect packets
3. `UniverseServer` accepts the connection and registers client state
4. `UniverseConnectionServer` delivers packet batches to `UniverseServer::packetsReceived`
5. `UniverseServer` validates, updates client context, and either responds directly or forwards world-scoped packets
6. once a world is active, the server pushes packet batches into `WorldServerThread`
7. `WorldServerThread` executes `WorldServer` updates and exposes outgoing packets
8. `UniverseServer` forwards outgoing packets back through `UniverseConnectionServer`
9. `UniverseClient` receives packet batches and updates `WorldClient`, `SystemWorldClient`, and client-facing state

### 4.2 Local single-player connect path

Single-player uses the same higher-level networking API but avoids remote transport.

`UniverseServer::addLocalClient()` creates a `LocalPacketSocket` pair and returns one side to the client while retaining the other on the server.

This has two important consequences:

- single-player still exercises most packet and routing code paths
- multiplayer and single-player gameplay behavior remain structurally close, which reduces duplicate logic but increases shared coupling

### 4.3 World packet path

World packet handling is effectively a second routing stage.

Universe level decides:

- which client is active
- which world they belong to
- whether a world exists yet
- whether packets should be queued, rejected, or routed

World level decides:

- how gameplay state mutates
- what outgoing authoritative updates must be emitted
- whether clients are errored and should be removed

## 5. Threading and Ownership

Networking changes need a precise ownership model.

### 5.1 Main ownership boundaries

- `UniverseServer` runs as its own thread
- `UniverseConnectionServer` owns worker threads for asynchronous packet processing
- each active `WorldServerThread` owns a world thread
- `UniverseClient` runs in the client process update loop

### 5.2 Practical implications

- connection-level state is not the same thing as world-level state
- some packet handling happens in universe scope, some in world scope
- client operations that look synchronous may correspond to queued packet work on the server
- world unload timing matters for in-flight requests and delayed results

### 5.3 Failure modes worth watching

- packet handled after the client changed worlds
- world unloaded while a promise or queued message is still pending
- packet format changed on only one side of the connection
- server-side client context changed without matching client assumptions
- local single-player behavior diverged from remote multiplayer expectations

## 6. Compatibility Model

The protocol has a pinned version through `StarProtocolVersion` and packet-type definitions.

Compatibility-sensitive areas include:

- packet binary read and write behavior
- packet JSON representations where used for tooling or scripting
- net compatibility rules
- authoritative entity state sync
- assets digest handling during connect

When planning protocol changes, assume both client and server need coordinated deployment unless the change is intentionally backward-compatible.

## 7. Typical Change Scenarios

### 7.1 Add a new replicated gameplay action

Usually touches:

- packet definition in `StarNetPackets.*`
- server receive path in `UniverseServer` or `WorldServer`
- client send path in `UniverseClient`, `WorldClient`, or UI code
- any resulting entity/world state sync
- tests or targeted validation flow

### 7.2 Change an existing replicated state field

Usually touches:

- a `StarNetElement*` structure or equivalent serialized state
- packet readers and writers if explicit packets carry the state
- server-side authoritative mutation logic
- client interpolation or display code
- protocol compatibility assumptions

### 7.3 Add a server-only admin or query feature

Usually touches:

- `source/server/`
- `UniverseServer`
- command or chat processor
- possibly configuration and Lua bindings

## 8. Recommended Edit Workflow for Networking Changes

1. Decide whether the change is connection-level, universe-level, world-level, or system-world-level.
2. Identify whether the state is authoritative on server only or needs client prediction or display state.
3. Map the exact packet or sync structure involved.
4. Verify thread ownership for the mutation path.
5. Check if local single-player uses the same path.
6. Check save and versioning impact if the networked state is also persisted.
7. Add or run a narrow validation path before expanding scope.

## 9. Risks and Review Questions

Use these questions during design and review:

- Does this change alter `StarProtocolVersion` expectations?
- Are both read and write paths updated on both sides?
- Is this a universe packet or a world packet, and is that split still correct?
- Can the packet arrive when the target world is not loaded, unloading, or already changed?
- Does single-player still use the same path?
- Does the change require versioning or persistence updates as well?
- Is the UI assuming instant application of a server-authoritative action?

## 10. Practical Takeaways

- The networking subsystem is a routing stack, not only a transport stack.
- `UniverseServer` is the main orchestration hub, but `WorldServerThread` is where much of the authoritative packet-driven simulation boundary lives.
- Single-player and multiplayer intentionally share most high-level packet paths.
- Packet and sync changes are expensive because they interact with gameplay, threading, and compatibility at once.
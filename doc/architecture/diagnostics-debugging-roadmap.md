# OpenStarbound Diagnostics And Debugging Roadmap

This roadmap defines a comprehensive debugging and observability system for OpenStarbound. It is meant to support day-to-day development, live server administration, mod debugging, crash triage, and the multicore optimization work described in `multicore-engineering-plan.md`.

The desired experience is a practical version of a Minecraft F3-style status view plus solid crash reports and structured logs: developers and server operators should be able to answer what the client, server, world, network, Lua runtime, renderer, assets, and storage layers were doing when a problem happened.

## Existing Foundations

OpenStarbound already has useful pieces that should be extended instead of replaced:

- `Logger` and `FileLogSink` in `source/core/StarLogging.*` provide multi-sink text logging.
- `LogMap` stores high-frequency key/value debug values and is already rendered by the client debug overlay.
- `SpatialLogger` records debug points, lines, polygons, and text for world and screen overlays.
- `/debug` toggles the client debug display and `/debug hud` toggles the text layer.
- Client render/update rate, player position, velocity, aim point, liquid level, and dungeon id are already sent to `LogMap`.
- `StarException` captures stack traces on supported platforms, and fatal exception paths log stack traces.
- Lua profiling can write `.luaprofile` summaries.
- `/servernetstats` exposes network worker counters added during the Phase 1 multicore work.

The main gap is that these pieces are not yet unified into a diagnostics model with categories, retention, crash bundling, redaction, command access, and machine-readable output.

## Goals

- Give players, modders, developers, and server operators enough context to reproduce and report issues.
- Make the F3-style overlay useful without requiring a debugger or log digging.
- Make crash reports self-contained enough to diagnose common failures from logs and metadata alone.
- Make multicore work measurable through low-overhead counters, histograms, and per-thread status.
- Keep diagnostic overhead bounded and configurable.
- Avoid leaking secrets, account data, passwords, private tokens, or full IP addresses by default.

## Non-Goals

- Upload diagnostics automatically without explicit user consent.
- Change save, packet, or mod APIs as part of the diagnostics foundation.
- Replace external profilers such as Visual Studio Profiler, WPA, `perf`, or Tracy-style tools.
- Turn release builds into permanently verbose debug builds.

## Diagnostic Data Model

Introduce a shared diagnostic model that can feed logs, overlays, commands, crash reports, and tests.

Core concepts:

- **Counters**: monotonic values such as packets processed, bytes read, wakeups, Lua errors, loaded worlds, and saves completed.
- **Gauges**: current values such as FPS, update rate, active worlds, clients, memory, queue depth, pending writes, and current world id.
- **Timers**: scoped durations with recent, min, max, average, p50, p95, and p99 summaries.
- **Events**: timestamped state changes such as connect, disconnect, warp, world load, asset reload, save start, save finish, and exception caught.
- **Context fields**: stable metadata such as version, source id, protocol, platform, renderer, GPU, asset digest, mod list, server UUID, player UUID, world id, and thread name.

Recommended implementation shape:

- Add a small `DiagnosticsRegistry` in `source/core/` for counters, gauges, timers, event rings, and snapshot export.
- Keep `LogMap` as the client overlay feed, but have it consume selected diagnostics instead of becoming the only diagnostics store.
- Add a bounded in-memory ring buffer for recent logs and diagnostic events.
- Export snapshots as JSON so server commands, crash reports, and future tools can use the same data.
- Prefer subsystem namespaces such as `client.render.fps`, `server.network.worker.0.packets`, `world.tick.p95Ms`, and `storage.pendingWrites`.

## F3-Style Client Status Overlay

The current `/debug` overlay should become the primary quick-status surface.

Pages should be added incrementally:

| Page | Data |
| --- | --- |
| Basic | FPS, update rate, frame/update ms, player position, velocity, aim, biome/world id, server UUID, player UUID, admin state. |
| Renderer | OpenGL version, renderer id, window mode, resolution, texture atlas sizes, draw calls if available, frame timings. |
| World | world id, template, gravity, weather, liquid at cursor, dungeon id, protected state, entity counts, tile sector info. |
| Network | connection id, protocol, server player count, ping/latency once measured, packet rates, pending send/receive queues. |
| Lua/Mods | loaded mods, asset digest, active Lua contexts, recent Lua errors, Lua profile summary when enabled. |
| Performance | client update/render timing p95/p99, slow frames, world client packet handling time, debug toggles. |

Controls:

- `/debug` toggles all debug rendering.
- `/debug hud` toggles the text layer.
- Add `/debug page <name|next|prev>` when multiple pages exist.
- Add `/debug copy` to copy a redacted JSON diagnostic snapshot to the clipboard.
- Add `/debug dump` to write a local diagnostic bundle without crashing.

Acceptance criteria:

- The overlay does not allocate heavily every frame.
- Long values are truncated or paged so the overlay remains readable.
- The overlay is useful on both single-player and remote-server sessions.
- Private data is either omitted or redacted in copy/dump paths.

## Server Status And Admin Commands

Server diagnostics should be available through admin commands and console output.

Recommended commands:

| Command | Purpose |
| --- | --- |
| `/serverstatus` | Summary of uptime, tick rate, connected players, active worlds, pending handshakes, storage queue depth, memory, and build identifiers. |
| `/servernetstats` | Existing network worker ownership, scan, wakeup, wait, packet, and callback counters. |
| `/worldstats` | Per-world command queue age/counts, tick timing, packet-prep counters, Phase 6 counters and mutation gate status, and system-world command/client/instance counts. |
| `/serverprofile [seconds]` | Short diagnostic sample window with p50/p95/p99 summaries for universe, world, network, and storage phases. |
| `/dumpdiag [scope]` | Writes a redacted diagnostic bundle to disk and returns the path. |

The dedicated server console should also support safe equivalents where practical, especially `status`, `netstats`, `worldstats`, and `dumpdiag`.

## Logging System Improvements

The current text logger is simple and reliable. Keep it, but add structure around it.

Implementation tasks:

1. Add subsystem/category names to log calls without forcing every existing call site to change at once.
2. Add a bounded ring buffer of recent log lines for crash reports and `/dumpdiag`.
3. Add log rotation by size and count so long-running servers do not grow unbounded logs.
4. Add optional JSON-lines structured logs for tools, disabled by default.
5. Add per-subsystem log levels, for example `network=debug`, `assets=info`, `lua=warn`.
6. Add rate limiting for repeated warnings and repeated exception summaries.
7. Add explicit redaction helpers for passwords, auth tokens, private hostnames, full IP addresses, and platform ids.

Compatibility and safety rules:

- Existing text logs should remain available.
- Do not log packet payloads or full player configs by default.
- Do not log secrets even at debug level unless a developer build opts in locally.

## Crash Reports

Crash reports should be written to `crashes/` or the configured storage/log directory before aborting where the platform allows it.

Report contents:

- crash id and timestamp
- executable name, version, source id, architecture, protocol, build type, and enabled integrations
- OS, CPU count, memory summary, GPU/renderer/audio backend where available
- exception message and full stack trace
- current thread name and, later, all-thread stack snapshots where supported
- last N log lines and last N diagnostic events
- asset digest, asset sources, mod list, and failed asset paths if known
- current client world/player context or server universe/world summary
- network worker stats and pending handshake counts on server crashes
- storage queue and last-save status when persistence work exists
- optional platform dump file, such as Windows minidump, when enabled

Report paths should be printed to the log and shown in the crash dialog when possible.

Redaction:

- Redact passwords, tokens, auth headers, platform secrets, and local absolute paths where feasible.
- Redact or hash account ids and public IPs by default.
- Provide an explicit `includePrivateDiagnostics` config for local developer machines.

## Multicore Integration

The diagnostics system is part of the multicore roadmap, not a separate luxury. Every multicore phase needs measurement and crash context.

Required metrics by phase:

| Phase | Required diagnostics |
| --- | --- |
| Phase 0 | Universe/world/network timing summaries, p95/p99 tick data, LogMap/server command snapshots. |
| Phase 1 | Network worker ownership, scans, stale scans, wakeups, waits, packet counts, callback time. |
| Phase 2 | Pending handshake states, timeouts, deadlines, slow-client counts, rejection reasons. |
| Phase 3 | Persistence queue depth, write duration, compression duration, flush wait time, failure counts. |
| Phase 4 | World command queue depth, command latency, blocked promise wait time, world mutex wait time during migration. |
| Phase 5 | Snapshot creation time, job queue time, job execution time, merge time, packet equivalence counters. |
| Phase 6 | Serial-vs-parallel subsystem duration, divergence counters, fallback counts, region boundary warnings. |

Go/no-go decisions should use these numbers instead of subjective smoothness alone.

## Phased Implementation

### D0: Inventory And First Status Feed

- Document existing `LogMap`, `SpatialLogger`, `/debug`, `Logger`, stack trace, Lua profile, and `/servernetstats` surfaces.
- Add basic world/server/player identifiers to the client debug overlay.
- Add help text for debug/status commands.
- Keep all behavior opt-in through existing debug toggles.

### D1: Diagnostics Registry

- Add counters, gauges, timers, and event ring buffers in `source/core/`.
- Add JSON snapshot export.
- Connect network worker stats and client overlay values to the registry.
- Add unit tests for registry updates, snapshots, and bounded retention.

### D2: Better Overlay And Server Commands

- Add `/debug page`, `/debug copy`, and `/debug dump`.
- Add `/serverstatus`, `/worldstats`, and `/dumpdiag`; `/serverstatus` and `/worldstats` are started, while `/dumpdiag` remains future work.
- Add readable grouping and truncation for long overlay values.
- Add admin permission checks for server diagnostic commands.

### D3: Crash Report Bundles

- Add crash report writer with redaction.
- Capture log/event ring buffers.
- Include asset/mod/build/platform context.
- Add Windows minidump support behind config.
- Add tests around report JSON generation and redaction.

### D4: Structured Logging And Rotation

- Add log categories and per-category levels.
- Add log rotation.
- Add optional JSON-lines sink.
- Add warning rate limiting.
- Keep existing text logs as the default operator-friendly output.

### D5: Profiling And Regression Workflow

- Add sampled profile windows for server commands.
- Extend `world_benchmark` to emit diagnostic summaries.
- Add CI-friendly comparison output for before/after performance runs.
- Add a standard diagnostic bundle attachment process for bug reports.

## Test Plan

- Unit tests for diagnostics registry counters, gauges, timers, ring buffers, and JSON snapshots.
- Redaction tests for logs, crash reports, and diagnostic bundles.
- Client smoke test for `/debug`, `/debug hud`, and future page commands.
- Server command tests for permissions and stable output.
- Crash-report generation test using a controlled fatal test path in a non-shipping test executable.
- Long-running server test for log rotation and bounded memory growth.
- Multicore benchmark runs that compare p50/p95/p99 before and after each optimization phase.

## Immediate Tickets

- Add more F3-style context to the current `LogMap` overlay: server UUID, player UUID, world id, admin state, player count, and ship/flight state.
- Add `/serverstatus` as a compact counterpart to `/servernetstats`. Started with uptime, player count, active world counts, pending queue sizes, TCP state, aggregate network counters, and aggregate world-command age/counts.
- Add `/worldstats` for per-world diagnostics. Started with world command queue age/counts, packet-prep counters, Phase 6 counters and mutation gate status, timing summaries, and system-world command/client/instance counts.
- Add a bounded recent-log ring buffer to `Logger`. Started with `Logger::recentLogMessages()` and configurable in-memory retention for formatted log lines.
- Add a crash report JSON writer that can be called from fatal exception paths. Started with a core `StarDiagnostics` writer that emits version metadata, fatal context, and recent logs into `crashes/`.
- Add a redaction helper and tests before writing diagnostic bundles. Started with redaction for password/token/auth text, IPv4 addresses, and common user-home path forms.
- Add universe-loop and world-tick timing counters that can feed both `/serverstatus` and crash reports.

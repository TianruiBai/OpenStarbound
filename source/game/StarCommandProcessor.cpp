#include "StarCommandProcessor.hpp"
#include "StarLexicalCast.hpp"
#include "StarJsonExtra.hpp"
#include "StarNpc.hpp"
#include "StarWorldServer.hpp"
#include "StarWarping.hpp"
#include "StarUniverseServer.hpp"
#include "StarUniverseSettings.hpp"
#include "StarRoot.hpp"
#include "StarItemDatabase.hpp"
#include "StarConfiguration.hpp"
#include "StarItemDrop.hpp"
#include "StarTreasure.hpp"
#include "StarLogging.hpp"
#include "StarPlayer.hpp"
#include "StarMonster.hpp"
#include "StarStagehand.hpp"
#include "StarVehicleDatabase.hpp"
#include "StarStagehandDatabase.hpp"
#include "StarLiquidsDatabase.hpp"
#include "StarChatProcessor.hpp"
#include "StarAssets.hpp"
#include "StarWorldLuaBindings.hpp"
#include "StarUniverseServerLuaBindings.hpp"
#include "StarString.hpp"

namespace Star {

namespace {

String packetPrepEntitySerializationSummary(HashMap<EntityType, WorldServer::EntitySerializationStats> const& stats) {
  StringList parts;
  List<EntityType> entityTypes{
      EntityType::Plant,
      EntityType::Object,
      EntityType::Vehicle,
      EntityType::ItemDrop,
      EntityType::PlantDrop,
      EntityType::Projectile,
      EntityType::Stagehand,
      EntityType::Monster,
      EntityType::Npc,
      EntityType::Player};

  for (auto entityType : entityTypes) {
    if (auto typeStats = stats.ptr(entityType)) {
      if (typeStats->totalCalls() != 0) {
        parts.append(strf("{}=store:{}/{} first:{}/{} delta:{}/{}",
            EntityTypeNames.getRight(entityType),
            typeStats->createStoreCalls,
            typeStats->createStoreBytes,
            typeStats->initialNetStateCalls,
            typeStats->initialNetStateBytes,
            typeStats->deltaNetStateCalls,
            typeStats->deltaNetStateBytes));
      }
    }
  }

  if (parts.empty())
    return "none";
  return parts.join(",");
}

String worldStorageTimingSummary(WorldStorageTimingStats const& stats) {
  return strf("sync:{}/{} entity:{}/{}/{}/{} tile:{}/{}/{} copy:{}/{} compress:{}/{}/{}/{} btree:{}/{}/{}/{} commit:{}/{} snapshot:{}/{}/{}/{}",
      stats.syncs,
      stats.syncedSectors,
      stats.entityStoreSectors,
      stats.entityStoreEntities,
      stats.entityStoreBytes,
      stats.entityStoreMicroseconds,
      stats.tileStoreSectors,
      stats.tileStoreBytes,
      stats.tileStoreMicroseconds,
      stats.sectorCopies,
      stats.sectorCopyMicroseconds,
      stats.compressionCalls,
      stats.compressionInputBytes,
      stats.compressionOutputBytes,
      stats.compressionMicroseconds,
      stats.btreeInserts,
      stats.btreeInserts + stats.btreeInsertSkips,
      stats.btreeInsertBytes + stats.btreeInsertSkipBytes,
      stats.btreeInsertMicroseconds,
      stats.commits,
      stats.commitMicroseconds,
      stats.fullSnapshotExports,
      stats.fullSnapshotChunks,
      stats.fullSnapshotBytes,
      stats.fullSnapshotExportMicroseconds);
}

}

CommandProcessor::CommandProcessor(UniverseServer* universe, LuaRootPtr luaRoot)
  : m_universe(universe) {
  auto assets = Root::singleton().assets();
  m_scriptComponent.addCallbacks("universe", LuaBindings::makeUniverseServerCallbacks(m_universe));
  m_scriptComponent.addCallbacks("CommandProcessor", makeCommandCallbacks());
  m_scriptComponent.setScripts(jsonToStringList(assets->json("/universe_server.config:commandProcessorScripts")));
  luaRoot->luaEngine().setNullTerminated(false);
  m_scriptComponent.setLuaRoot(luaRoot);
  m_scriptComponent.init();
}

String CommandProcessor::adminCommand(String const& command, String const& argumentString) {
  MutexLocker locker(m_mutex);
  return handleCommand(ServerConnectionId, command, argumentString);
}

String CommandProcessor::userCommand(ConnectionId connectionId, String const& command, String const& argumentString) {
  MutexLocker locker(m_mutex);
  if (connectionId == ServerConnectionId)
    throw StarException("CommandProcessor::userCommand called with ServerConnectionId");
  return handleCommand(connectionId, command, argumentString);
}

String CommandProcessor::help(ConnectionId connectionId, String const& argumentString) {
  auto arguments = m_parser.tokenizeToStringList(argumentString);

  auto assets = Root::singleton().assets();
  auto basicCommands = assets->json("/help.config:basicCommands");
  auto openSbCommands = assets->json("/help.config:openSbCommands");
  auto adminCommands = assets->json("/help.config:adminCommands");
  auto debugCommands = assets->json("/help.config:debugCommands");
  auto openSbDebugCommands = assets->json("/help.config:openSbDebugCommands");

  if (arguments.size()) {
    if (arguments.size() >= 1) {
      if (auto helpText = basicCommands.optString(arguments[0]).orMaybe(openSbCommands.optString(arguments[0])).orMaybe(adminCommands.optString(arguments[0])).orMaybe(debugCommands.optString(arguments[0])).orMaybe(openSbDebugCommands.optString(arguments[0])))
        return *helpText;
    }
  }

  String res = "";

  auto commandDescriptions = [&](Json const& commandConfig) {
      StringList commandList = commandConfig.toObject().keys();
      sort(commandList);
      return "/" + commandList.join(", /");
    };

  String basicHelpFormat = assets->json("/help.config:basicHelpText").toString();
  res = res + strf(basicHelpFormat.utf8Ptr(), commandDescriptions(basicCommands));

  String openSbHelpFormat = assets->json("/help.config:openSbHelpText").toString();
  res = res + "\n" + strf(openSbHelpFormat.utf8Ptr(), commandDescriptions(openSbCommands));

  if (!adminCheck(connectionId, "")) {
    String adminHelpFormat = assets->json("/help.config:adminHelpText").toString();
    res = res + "\n" + strf(adminHelpFormat.utf8Ptr(), commandDescriptions(adminCommands));

    String debugHelpFormat = assets->json("/help.config:debugHelpText").toString();
    res = res + "\n" + strf(debugHelpFormat.utf8Ptr(), commandDescriptions(debugCommands));

    String openSbDebugHelpFormat = assets->json("/help.config:openSbDebugHelpText").toString();
    res = res + "\n" + strf(openSbDebugHelpFormat.utf8Ptr(), commandDescriptions(openSbDebugCommands));
  }

  res = res + "\n" + basicCommands.getString("help");

  return res;
}

String CommandProcessor::admin(ConnectionId connectionId, String const& argumentString) {
  auto config = Root::singleton().configuration();
  auto arguments = m_parser.tokenizeToStringList(argumentString);

  ConnectionId targetClientId = connectionId;

  if (!arguments.empty()) {
    if (auto errorMsg = adminCheck(connectionId, "admin a user"))
      return *errorMsg;

    auto targetCid = playerCidFromCommand(arguments[0], m_universe);
    if (!targetCid)
      return strf("No user with specifier {} found.", arguments[0]);

    targetClientId = *targetCid;
  } else {
    if (!m_universe->canBecomeAdmin(connectionId) && !m_universe->isAdmin(connectionId))
      return "Insufficient privileges to make self admin.";
  }

  if (targetClientId == ServerConnectionId)
    return "Invalid client state";

  if (!config->get("allowAdminCommands").toBool())
    return "Admin commands disabled on this server.";

  bool wasAdmin = m_universe->isAdmin(targetClientId);
  m_universe->setAdmin(targetClientId, !wasAdmin);

  if (!wasAdmin)
    return strf("Admin privileges now given to {}", m_universe->clientNick(targetClientId));
  else
    return strf("Admin privileges taken away from {}", m_universe->clientNick(targetClientId));
}

String CommandProcessor::pvp(ConnectionId connectionId, String const&) {
  if (!m_universe->isPvp(connectionId)) {
    m_universe->setPvp(connectionId, true);
    if (m_universe->isPvp(connectionId))
      m_universe->adminBroadcast(strf("Player {} is now PVP", m_universe->clientNick(connectionId)));
  } else {
    m_universe->setPvp(connectionId, false);
    if (!m_universe->isPvp(connectionId))
      m_universe->adminBroadcast(strf("Player {} is a big wimp and is no longer PVP", m_universe->clientNick(connectionId)));
  }

  if (m_universe->isPvp(connectionId))
    return "PVP active";
  else
    return "PVP inactive";
}

String CommandProcessor::whoami(ConnectionId connectionId, String const&) {
  return strf("Server: You are {}. You are {}an Admin",
      m_universe->clientNick(connectionId),
      m_universe->isAdmin(connectionId) ? "" : "not ");
}

String CommandProcessor::warp(ConnectionId connectionId, String const& argumentString) {
  if (auto errorMsg = adminCheck(connectionId, "do the space warp again"))
    return *errorMsg;

  try {
    m_universe->clientWarpPlayer(connectionId, parseWarpAction(argumentString));
    return "Lets do the space warp again";
  } catch (StarException const& e) {
    Logger::warn("Could not parse warp target: {}", outputException(e, false));
    return strf("Could not parse the argument {} as a warp target", argumentString);
  }
}

String CommandProcessor::warpRandom(ConnectionId connectionId, String const& typeName) {
  if (auto errorMsg = adminCheck(connectionId, "warp to random world"))
    return *errorMsg;

	Vec2I size = {2, 2};
	auto& celestialDatabase = m_universe->celestialDatabase();
	Maybe<CelestialCoordinate> target = {};

	auto validPlanet = [&celestialDatabase, &typeName](CelestialCoordinate const& p) {
			if (auto celestialParams = celestialDatabase.parameters(p)) {
				if (auto visitableParams = celestialParams->visitableParameters()) {
					if (visitableParams->typeName == typeName)
						return true;
				}
			}
			return false;
		};

	while (target.isNothing()) {
		RectI region = RectI::withSize(Vec2I(Random::randi32(), Random::randi32()), size);

		while (!celestialDatabase.scanRegionFullyLoaded(region)) {
			celestialDatabase.scanSystems(region);
		}
		auto systems = celestialDatabase.scanSystems(region);
		for (auto s : systems) {
			for (auto planet : celestialDatabase.children(s)) {
				if (validPlanet(planet))
					target = planet;
				if (target.isNothing()) {
					for (auto moon : celestialDatabase.children(planet)) {
						if (validPlanet(moon)) {
							target = moon;
							break;
						}
					}
				}
			}
		}

		if (size.magnitude() > 1024)
			return "could not find a matching world";
		size *= 2;
	}

	m_universe->clientWarpPlayer(connectionId, WarpToWorld(CelestialWorldId(*target)));
	return strf("warping to {}", *target);
}

String CommandProcessor::timewarp(ConnectionId connectionId, String const& argumentsString) {
  if (auto errorMsg = adminCheck(connectionId, "do the time warp again"))
    return *errorMsg;

  auto arguments = m_parser.tokenizeToStringList(argumentsString);
  if (arguments.empty())
    return "Not enough arguments to /timewarp";

  try {
    auto time = lexicalCast<double>(arguments.at(0));
    if (time == 0.0)
      return "You suck at time travel.";
    else if (time < 0.0 && (arguments.size() < 2 || arguments[1] != "please"))
      return "Great Scott! We can't go back in time!";

    m_universe->universeClock()->adjustTime(time);
    return time > 0.0 ? "It's just a jump to the left..." : "And then a step to the right...";
  } catch (BadLexicalCast const&) {
    return strf("Could not parse the argument {} as a time adjustment", arguments[0]);
  }
}

String CommandProcessor::timescale(ConnectionId connectionId, String const& argumentsString) {
  if (auto errorMsg = adminCheck(connectionId, "mess with time"))
    return *errorMsg;

  auto arguments = m_parser.tokenizeToStringList(argumentsString);

  if (arguments.empty())
    return strf("Current timescale is {:6.6f}x", GlobalTimescale);

  float timescale = clamp(lexicalCast<float>(arguments[0]), 0.001f, 32.0f);
  m_universe->setTimescale(timescale);
  return strf("Set timescale to {:6.6f}x", timescale);
}

String CommandProcessor::tickrate(ConnectionId connectionId, String const& argumentsString) {
  if (auto errorMsg = adminCheck(connectionId, "change the tick rate"))
    return *errorMsg;

  auto arguments = m_parser.tokenizeToStringList(argumentsString);

  if (arguments.empty())
    return strf("Current tick rate is {:4.2f}Hz", 1.0f / ServerGlobalTimestep);

  float tickRate = clamp(lexicalCast<float>(arguments[0]), 5.f, 500.f);
  m_universe->setTickRate(tickRate);
  return strf("Set tick rate to {:4.2f}Hz", tickRate);
}

String CommandProcessor::serverStatus(ConnectionId connectionId, String const&) {
  if (auto errorMsg = adminCheck(connectionId, "view server status"))
    return *errorMsg;

  auto status = m_universe->serverStatus();
  StringList lines;
  lines.append(strf("Server status: uptime={}, players={}/{}, tickRate={:4.2f}Hz, timescale={:4.2f}x, paused={}",
      Time::printDuration(status.uptime),
      status.clients,
      status.maxClients,
      status.tickRate,
      status.timescale,
      status.paused ? "true" : "false"));
  lines.append(strf("Worlds: active={}, system={}", status.activeWorlds, status.systemWorlds));
  lines.append(strf("TCP: listening={}, failed={}, pendingAccepts={}, deadConnections={}",
      status.listeningTcp ? "true" : "false",
      status.tcpListenFailed ? "true" : "false",
      status.pendingConnectionAccepts,
      status.deadConnections));
  lines.append(strf("Handshakes: pending={} (protocol={}, send={}, client={}, password={}, finalize={}, reject={}), accepted={}, finalized={}, rejected={}, timedOut={}",
      status.pendingHandshakes,
      status.pendingHandshakeAwaitProtocolRequest,
      status.pendingHandshakeSendProtocolResponse,
      status.pendingHandshakeAwaitClientConnect,
      status.pendingHandshakeAwaitHandshakeResponse,
      status.pendingHandshakeFinalizeClient,
      status.pendingHandshakeRejectAndFlush,
      status.pendingHandshakeAccepted,
      status.pendingHandshakeFinalized,
      status.pendingHandshakeRejected,
      status.pendingHandshakeTimedOut));
  lines.append(strf("Pending: warps={}, queuedFlights={}, flights={}, arrivals={}, disconnects={}, celestialRequests={} ({} clients), chatMessages={} ({} clients), worldMessages={} ({} worlds)",
      status.pendingPlayerWarps,
      status.queuedFlights,
      status.pendingFlights,
      status.pendingArrivals,
      status.pendingDisconnections,
      status.pendingCelestialRequests,
      status.pendingCelestialRequestClients,
      status.pendingChatMessages,
      status.pendingChatClients,
      status.pendingWorldMessages,
      status.pendingWorldMessageWorlds));
  lines.append(strf("Network: workers={}, ownedConnections={}, queueOnly={}, packets={}, wakeups={}, idleWaits={}, sends=q:{}/{} eager:{}/{} worker:{}/{} writeUs:{}/{}",
      status.networkWorkers,
      status.networkOwnedConnections,
      status.networkQueueOnlySends,
      status.networkPacketsProcessed,
      status.networkWakeups,
      status.networkIdleTimedWaits,
      status.networkQueuedSendBatches,
      status.networkQueuedSendPackets,
      status.networkEagerSendBatches,
      status.networkEagerSendPackets,
      status.networkWorkerSendBatches,
      status.networkWorkerSendPackets,
      status.networkEagerWriteTimeMicroseconds,
      status.networkWorkerWriteTimeMicroseconds));
  lines.append(strf("World commands: pending={}, oldestPendingUs={}, processed={}, direct={}, failed={}, waitUs={}",
      status.worldCommandQueueDepth,
      status.worldCommandOldestPendingAgeMicroseconds,
      status.worldCommandsProcessed,
      status.worldCommandsDirect,
      status.worldCommandsFailed,
      status.worldCommandWaitMicroseconds));
  lines.append(strf("World packet prep: ticks={}, regions={}/{}/{}/{}, sectorCache={}/{}, entityStoreCache={}/{}, netStateCache={}/{}, sectorFanout={}/{}/{}, entitySerialize={}",
      status.worldPacketPrepTicks,
      status.worldPacketPrepMonitoringRegionBuilds,
      status.worldPacketPrepMonitoringRegionRects,
      status.worldPacketPrepMonitoringRegionSplitRects,
      status.worldPacketPrepMonitoringRegionReuses,
      status.worldPacketPrepSectorCacheHits,
      status.worldPacketPrepSectorCacheMisses,
      status.worldPacketPrepEntityStoreCacheHits,
      status.worldPacketPrepEntityStoreCacheMisses,
      status.worldPacketPrepNetStateCacheHits,
      status.worldPacketPrepNetStateCacheMisses,
      status.worldPacketPrepSectorClientFanoutLookups,
      status.worldPacketPrepSectorClientFanoutRecipients,
      status.worldPacketPrepSectorClientFanoutMisses,
      packetPrepEntitySerializationSummary(status.worldPacketPrepEntitySerializationStats)));
  lines.append(strf("World storage: storageTiming={}", worldStorageTimingSummary(status.worldStorageTimingStats)));
  lines.append(strf("Phase 6 storage planning: enabledWorlds={}, ticks={}, serialTicks={}, parallelTicks={}, sectors={}, serialUs={}, parallelUs={}, mergeUs={}, fallbacks={}, diffWorlds={}, diffChecks={}, diffUs={}, divergences={}",
      status.phase6StorageGenerationPlanningEnabledWorlds,
      status.phase6StorageGenerationPlanningTicks,
      status.phase6StorageGenerationPlanningSerialTicks,
      status.phase6StorageGenerationPlanningParallelTicks,
      status.phase6StorageGenerationPlanningSectors,
      status.phase6StorageGenerationPlanningSerialMicroseconds,
      status.phase6StorageGenerationPlanningParallelMicroseconds,
      status.phase6StorageGenerationPlanningMergeMicroseconds,
      status.phase6StorageGenerationPlanningFallbacks,
      status.phase6StorageGenerationPlanningDifferentialCheckEnabledWorlds,
      status.phase6StorageGenerationPlanningDifferentialChecks,
      status.phase6StorageGenerationPlanningDifferentialMicroseconds,
      status.phase6StorageGenerationPlanningDivergences));
  lines.append(strf("Phase 6 packet sector prefill: enabledWorlds={}, ticks={}, serialTicks={}, parallelTicks={}, sectors={}, serialUs={}, parallelUs={}, mergeUs={}, fallbacks={}, diffWorlds={}, diffChecks={}, diffUs={}, divergences={}",
      status.phase6PacketPreparationSectorPrefillEnabledWorlds,
      status.phase6PacketPreparationSectorPrefillTicks,
      status.phase6PacketPreparationSectorPrefillSerialTicks,
      status.phase6PacketPreparationSectorPrefillParallelTicks,
      status.phase6PacketPreparationSectorPrefillSectors,
      status.phase6PacketPreparationSectorPrefillSerialMicroseconds,
      status.phase6PacketPreparationSectorPrefillParallelMicroseconds,
      status.phase6PacketPreparationSectorPrefillMergeMicroseconds,
      status.phase6PacketPreparationSectorPrefillFallbacks,
      status.phase6PacketPreparationSectorPrefillDifferentialCheckEnabledWorlds,
      status.phase6PacketPreparationSectorPrefillDifferentialChecks,
      status.phase6PacketPreparationSectorPrefillDifferentialMicroseconds,
      status.phase6PacketPreparationSectorPrefillDivergences));
    lines.append(strf("Phase 6 subsystem baselines: enabledWorlds={}, liquidTicks={}, liquidActiveCells={}, liquidRegions={}, liquidCache={}/{}/{}/{}/{}/{}/{}, fallingTicks={}, fallingPending={}, fallingProcessed={}, fallingMoved={}, wiringTicks={}, wiringInitial={}, wiringLoaded={}, wiringNetworkLoads={}, wiringEvaluated={}, entityTicks={}, entityUpdated={}, entityTile={}, entityDestroyed={}, luaTicks={}, luaContexts={}, luaUpdates={}",
      status.phase6SubsystemBaselineMetricsEnabledWorlds,
      status.phase6LiquidBaselineTicks,
      status.phase6LiquidActiveCells,
      status.phase6LiquidMonitoringRegions,
      status.phase6LiquidNoProcessingLimitRegionCacheBuilds,
      status.phase6LiquidNoProcessingLimitRegionCacheRebuildSkips,
      status.phase6LiquidNoProcessingLimitRegionCacheRegions,
      status.phase6LiquidNoProcessingLimitRegionCacheBuckets,
      status.phase6LiquidNoProcessingLimitRegionCacheLookups,
      status.phase6LiquidNoProcessingLimitRegionCacheCandidates,
      status.phase6LiquidNoProcessingLimitRegionCacheHits,
      status.phase6FallingBlocksBaselineTicks,
      status.phase6FallingBlocksPendingPositions,
      status.phase6FallingBlocksProcessedPositions,
      status.phase6FallingBlocksMovedBlocks,
      status.phase6WiringBaselineTicks,
      status.phase6WiringInitialEntities,
      status.phase6WiringLoadedEntities,
      status.phase6WiringNetworkLoads,
      status.phase6WiringEvaluatedEntities,
      status.phase6EntityBaselineTicks,
      status.phase6EntityUpdatedEntities,
      status.phase6EntityTileEntities,
      status.phase6EntityDestroyedEntities,
      status.phase6LuaBaselineTicks,
      status.phase6LuaScriptContexts,
      status.phase6LuaScriptUpdates));
  lines.append(strf("Persistence: asyncEnabled={}, pendingBatches={}, pendingSnapshots={}, oldestPendingMs={}, completedBatches={}, snapshots={}, snapshotBuildUs={}, writeUs={}, celestialCommitUs={}, celestialCommits={}, failures={}, retries={}, syncFallbacks={}, queueFullFallbacks={}",
      status.persistenceAsyncEnabled,
      status.persistenceBatchesPending,
      status.persistenceSnapshotsPending,
      status.persistenceOldestPendingAgeMilliseconds,
      status.persistenceBatchesCompleted,
      status.persistenceSnapshotsWritten,
      status.persistenceSnapshotBuildTimeMicroseconds,
      status.persistenceWriteTimeMicroseconds,
      status.persistenceCelestialCommitTimeMicroseconds,
      status.persistenceCelestialCommits,
      status.persistenceFailures,
      status.persistenceWriteRetries,
      status.persistenceSynchronousFallbacks,
      status.persistenceQueueFullFallbacks));
  StringList timingParts;
  for (auto const& timing : status.universeTimings) {
    if (timing.samples != 0)
      timingParts.append(strf("{}={}/{}/{}/{}/{}", timing.name, timing.averageMicroseconds, timing.p50Microseconds, timing.p95Microseconds, timing.p99Microseconds, timing.maxMicroseconds));
  }
  if (!timingParts.empty())
    lines.append(strf("Universe timings us avg/p50/p95/p99/max: {}", timingParts.join(", ")));

  auto appendTimingLine = [&lines](String const& label, List<UniverseServer::ServerStatus::TimingStatus> const& timings) {
    StringList parts;
    for (auto const& timing : timings) {
      if (timing.samples != 0)
        parts.append(strf("{}={}/{}/{}/{}/{}", timing.name, timing.averageMicroseconds, timing.p50Microseconds, timing.p95Microseconds, timing.p99Microseconds, timing.maxMicroseconds));
    }
    if (!parts.empty())
      lines.append(strf("{} timings us avg/p50/p95/p99/max: {}", label, parts.join(", ")));
  };
  appendTimingLine("World thread", status.worldThreadTimings);
  appendTimingLine("World update", status.worldUpdateTimings);

  return lines.join("\n");
}

String CommandProcessor::worldStats(ConnectionId connectionId, String const&) {
  if (auto errorMsg = adminCheck(connectionId, "view world stats"))
    return *errorMsg;

  auto summary = m_universe->worldStats();
  StringList lines;
  lines.append(strf("World stats: active={}, system={}", summary.worlds.size(), summary.systemWorlds.size()));

  for (auto const& world : summary.worlds) {
    String state = world.loaded ? "loaded" : (world.loading ? "loading" : "errored");
    auto const& commands = world.commandStats;
    auto const& packetPrep = world.packetPreparationStats;
    auto const& phase6 = world.phase6WorldParallelismStats;
    lines.append(strf("world {}: state={}, clients={}, commands=pending:{} oldestPendingUs:{} processed:{} direct:{} failed:{} waitUs:{}, packetPrep=ticks:{} regions:{}/{}/{}/{} sectorCache:{}/{} entityStoreCache:{}/{} netStateCache:{}/{} sectorFanout:{}/{}/{} entitySerialize:{} storageTiming={}, phase6=storage:{}/{}/{} sectors:{} fallbacks:{} divergences:{} packetPrefill:{}/{}/{} sectors:{} fallbacks:{} divergences:{} baselines:liquid:{} liquidCache:{}/{}/{}/{}/{}/{}/{} falling:{} wiring:{} entity:{} lua:{} mutation=requested:{} blocked:fixedSeed:{} dependency:{} modVisibility:{} implementation:{}",
        printWorldId(world.worldId),
        state,
        world.clients,
        commands.pending,
        commands.oldestPendingAgeMicroseconds,
        commands.processed,
        commands.direct,
        commands.failed,
        commands.waitMicroseconds,
        packetPrep.ticks,
        packetPrep.monitoringRegionBuilds,
        packetPrep.monitoringRegionRects,
        packetPrep.monitoringRegionSplitRects,
        packetPrep.monitoringRegionReuses,
        packetPrep.sectorPacketCacheHits,
        packetPrep.sectorPacketCacheMisses,
        packetPrep.entityStoreCacheHits,
        packetPrep.entityStoreCacheMisses,
        packetPrep.entityNetStateCacheHits,
        packetPrep.entityNetStateCacheMisses,
        packetPrep.sectorClientFanoutLookups,
        packetPrep.sectorClientFanoutRecipients,
        packetPrep.sectorClientFanoutMisses,
        packetPrepEntitySerializationSummary(packetPrep.entitySerializationStats),
        worldStorageTimingSummary(world.storageTimingStats),
        phase6.storageGenerationPlanningTicks,
        phase6.storageGenerationPlanningSerialTicks,
        phase6.storageGenerationPlanningParallelTicks,
        phase6.storageGenerationPlanningSectors,
        phase6.storageGenerationPlanningFallbacks,
        phase6.storageGenerationPlanningDivergences,
        phase6.packetPreparationSectorPrefillTicks,
        phase6.packetPreparationSectorPrefillSerialTicks,
        phase6.packetPreparationSectorPrefillParallelTicks,
        phase6.packetPreparationSectorPrefillSectors,
        phase6.packetPreparationSectorPrefillFallbacks,
        phase6.packetPreparationSectorPrefillDivergences,
        phase6.liquidBaselineTicks,
        phase6.liquidNoProcessingLimitRegionCacheBuilds,
        phase6.liquidNoProcessingLimitRegionCacheRebuildSkips,
        phase6.liquidNoProcessingLimitRegionCacheRegions,
        phase6.liquidNoProcessingLimitRegionCacheBuckets,
        phase6.liquidNoProcessingLimitRegionCacheLookups,
        phase6.liquidNoProcessingLimitRegionCacheCandidates,
        phase6.liquidNoProcessingLimitRegionCacheHits,
        phase6.fallingBlocksBaselineTicks,
        phase6.wiringBaselineTicks,
        phase6.entityBaselineTicks,
        phase6.luaBaselineTicks,
        phase6.mutationParallelismRequested,
        phase6.mutationParallelismBlockedByFixedSeedGate,
        phase6.mutationParallelismBlockedByDependencyGate,
        phase6.mutationParallelismBlockedByModVisibilityGate,
        phase6.mutationParallelismBlockedByImplementationGate));

    StringList threadTimingParts;
    for (auto const& timing : world.threadTimings) {
      if (timing.samples != 0)
        threadTimingParts.append(strf("{}={}/{}/{}/{}/{}", timing.name, timing.averageMicroseconds, timing.p50Microseconds, timing.p95Microseconds, timing.p99Microseconds, timing.maxMicroseconds));
    }
    if (!threadTimingParts.empty())
      lines.append(strf("world {} thread timings us avg/p50/p95/p99/max: {}", printWorldId(world.worldId), threadTimingParts.join(", ")));

    StringList worldTimingParts;
    for (auto const& timing : world.worldTimings) {
      if (timing.samples != 0)
        worldTimingParts.append(strf("{}={}/{}/{}/{}/{}", timing.name, timing.averageMicroseconds, timing.p50Microseconds, timing.p95Microseconds, timing.p99Microseconds, timing.maxMicroseconds));
    }
    if (!worldTimingParts.empty())
      lines.append(strf("world {} update timings us avg/p50/p95/p99/max: {}", printWorldId(world.worldId), worldTimingParts.join(", ")));
  }

  for (auto const& systemWorld : summary.systemWorlds) {
    auto const& commands = systemWorld.commandStats;
    lines.append(strf("system {}: clients={}, activeInstances={}, commands=pending:{} oldestPendingUs:{} processed:{} direct:{} failed:{} waitUs:{}",
        systemWorld.location,
        systemWorld.clients,
        systemWorld.activeInstanceWorlds,
        commands.pending,
        commands.oldestPendingAgeMicroseconds,
        commands.processed,
        commands.direct,
        commands.failed,
        commands.waitMicroseconds));
  }

  return lines.join("\n");
}

String CommandProcessor::serverNetStats(ConnectionId connectionId, String const&) {
  if (auto errorMsg = adminCheck(connectionId, "view server network stats"))
    return *errorMsg;

  StringList lines;
  auto stats = m_universe->connectionWorkerStats();
  lines.append(strf("Network workers: {}", stats.size()));

  uint64_t totalPackets = 0;
  uint64_t totalScans = 0;
  uint64_t totalQueuedSendBatches = 0;
  uint64_t totalQueuedSendPackets = 0;
  uint64_t totalEagerSendBatches = 0;
  uint64_t totalEagerSendPackets = 0;
  uint64_t totalEagerWriteTimeMicroseconds = 0;
  uint64_t totalWorkerSendBatches = 0;
  uint64_t totalWorkerSendPackets = 0;
  uint64_t totalWorkerWriteTimeMicroseconds = 0;
  uint64_t totalWakeups = 0;
  uint64_t totalTimedWaits = 0;
  uint64_t totalIdleTimedWaits = 0;

  for (size_t i = 0; i < stats.size(); ++i) {
    auto const& workerStats = stats[i];
    totalPackets += workerStats.packetsProcessed;
    totalScans += workerStats.connectionScans;
    totalQueuedSendBatches += workerStats.queuedSendBatches;
    totalQueuedSendPackets += workerStats.queuedSendPackets;
    totalEagerSendBatches += workerStats.eagerSendBatches;
    totalEagerSendPackets += workerStats.eagerSendPackets;
    totalEagerWriteTimeMicroseconds += workerStats.eagerWriteTimeMicroseconds;
    totalWorkerSendBatches += workerStats.workerSendBatches;
    totalWorkerSendPackets += workerStats.workerSendPackets;
    totalWorkerWriteTimeMicroseconds += workerStats.workerWriteTimeMicroseconds;
    totalWakeups += workerStats.wakeups;
    totalTimedWaits += workerStats.timedWaits;
    totalIdleTimedWaits += workerStats.idleTimedWaits;

    uint64_t averageCallbackMicroseconds = workerStats.callbackGroupsProcessed
        ? workerStats.callbackTimeMicroseconds / workerStats.callbackGroupsProcessed
        : 0;

    lines.append(strf("worker {}: owned={}, handled={}, scans={}, stale={}, packets={}, queued={}/{}, eager={}/{}/{}, workerSend={}/{}/{}, callbacks={}, avgCallbackUs={}, wakeups={}, waits={}, idleWaits={}",
        i,
        workerStats.ownedConnections,
        workerStats.lastHandledConnections,
        workerStats.connectionScans,
        workerStats.staleConnectionScans,
        workerStats.packetsProcessed,
        workerStats.queuedSendBatches,
        workerStats.queuedSendPackets,
        workerStats.eagerSendBatches,
        workerStats.eagerSendPackets,
        workerStats.eagerWriteTimeMicroseconds,
        workerStats.workerSendBatches,
        workerStats.workerSendPackets,
        workerStats.workerWriteTimeMicroseconds,
        workerStats.callbackGroupsProcessed,
        averageCallbackMicroseconds,
        workerStats.wakeups,
        workerStats.timedWaits,
        workerStats.idleTimedWaits));
  }

  lines.append(strf("totals: scans={}, packets={}, queued={}/{}, eager={}/{}/{}, workerSend={}/{}/{}, wakeups={}, waits={}, idleWaits={}",
      totalScans,
      totalPackets,
      totalQueuedSendBatches,
      totalQueuedSendPackets,
      totalEagerSendBatches,
      totalEagerSendPackets,
      totalEagerWriteTimeMicroseconds,
      totalWorkerSendBatches,
      totalWorkerSendPackets,
      totalWorkerWriteTimeMicroseconds,
      totalWakeups,
      totalTimedWaits,
      totalIdleTimedWaits));
  return lines.join("\n");
}

String CommandProcessor::setTileProtection(ConnectionId connectionId, String const& argumentString) {
  if (auto errorMsg = adminCheck(connectionId, "modify world properties")) {
    return *errorMsg;
  }

  auto arguments = m_parser.tokenizeToStringList(argumentString);

  if (arguments.size() < 2)
    return "Not enough arguments to /settileprotection. Use /settileprotection <dungeonId> <protected>";

  try {
    bool isProtected = Json::parse(arguments.takeLast()).toBool();
    List<DungeonId> dungeonIds;
    for (auto& banana : arguments) {
      auto slices = banana.split("..");
      auto it = slices.begin();
      DungeonId previous = 0;
      while (it != slices.end()) {
        DungeonId current = lexicalCast<DungeonId>(*it);
        dungeonIds.append(current);
        if (it++ != slices.begin() && previous != current) {
          if (current < previous) swap(previous, current);
          for (DungeonId id = previous + 1; id != current; ++id)
            dungeonIds.append(id);
        }
        previous = current;
      }
    }
    size_t changed = 0;
    if (!m_universe->executeForClient(connectionId, [&](WorldServer* world, PlayerPtr const&) {
       changed = world->setTileProtection(dungeonIds, isProtected);
      })) {
      return "Invalid client state";
    }
    String output = strf("{} {} dungeon IDs", isProtected ? "Protected" : "Unprotected", changed);
    return changed < dungeonIds.size() ? strf("{} ({} unchanged)", output, dungeonIds.size() - changed) : output;
  } catch (BadLexicalCast const&) {
    return strf("Could not parse /settileprotection parameters. Use /settileprotection <dungeonId...> <protected>", argumentString);
  }
}

String CommandProcessor::setDungeonId(ConnectionId connectionId, String const& argumentString) {
  if (auto errorMsg = adminCheck(connectionId, "set dungeon id")) {
    return *errorMsg;
  }

  auto arguments = m_parser.tokenizeToStringList(argumentString);
  if (arguments.size() < 1)
    return "Not enough arguments to /setdungeonid. Use /setdungeonid <dungeonId>";

  try {
    DungeonId dungeonId = lexicalCast<DungeonId>(arguments.at(0));

    bool done = m_universe->executeForClient(connectionId, [dungeonId](WorldServer* world, PlayerPtr const& player) {
        world->setDungeonId(RectI::withSize(Vec2I(player->aimPosition()), Vec2I(1, 1)), dungeonId);
      });

    return done ? "" : "Failed to set dungeon id.";
  } catch (BadLexicalCast const&) {
    return strf("Could not parse /setdungeonid parameters. Use /setdungeonid <dungeonId>!", argumentString);
  }
}

String CommandProcessor::setPlayerStart(ConnectionId connectionId, String const&) {
  if (auto errorMsg = adminCheck(connectionId, "modify world properties"))
    return *errorMsg;

  m_universe->executeForClient(connectionId, [](WorldServer* world, PlayerPtr const& player) {
      world->setPlayerStart(player->position() + player->feetOffset());
    });

  return "";
}

String CommandProcessor::spawnItem(ConnectionId connectionId, String const& argumentString) {
  if (auto errorMsg = adminCheck(connectionId, "spawn items"))
    return *errorMsg;

  auto arguments = m_parser.tokenizeToStringList(argumentString);

  if (arguments.empty())
    return "Not enough arguments to /spawnitem";

  try {
    String kind = arguments.at(0);
    Json parameters = JsonObject();
    uint64_t amount = 1;
    Maybe<float> level;
    Maybe<uint64_t> seed;

    if (arguments.size() >= 2)
      amount = lexicalCast<uint64_t>(arguments.at(1));

    if (arguments.size() >= 3)
      parameters = Json::parse(arguments.at(2));

    if (arguments.size() >= 4)
      level = lexicalCast<float>(arguments.at(3));

    if (arguments.size() >= 5)
      seed = lexicalCast<uint64_t>(arguments.at(4));

    bool done = m_universe->executeForClient(connectionId, [&](WorldServer* world, PlayerPtr const& player) {
        auto itemDatabase = Root::singleton().itemDatabase();
        world->addEntity(ItemDrop::createRandomizedDrop(itemDatabase->item(ItemDescriptor(kind, amount, parameters), level, seed, true), player->aimPosition()));
      });

    return done ? "" : "Invalid client state";
  } catch (JsonParsingException const& exception) {
    Logger::warn("Error while processing /spawnitem '{}' command. Json parse problem: {}", arguments.at(0), outputException(exception, false));
    return "Could not parse item parameters";
  } catch (ItemException const& exception) {
    Logger::warn("Error while processing /spawnitem '{}' command. Item instantiation problem: {}", arguments.at(0), outputException(exception, false));
    return strf("Could not load item '{}'", arguments.at(0));
  } catch (BadLexicalCast const& exception) {
    Logger::warn("Error while processing /spawnitem command. Number expected. Got something else: {}", outputException(exception, false));
    return strf("Could not load item '{}'", arguments.at(0));
  } catch (StarException const& exception) {
    Logger::warn("Error while processing /spawnitem command '{}', exception caught: {}", argumentString, outputException(exception, false));
    return strf("Could not load item '{}'", arguments.at(0));
  }
}

String CommandProcessor::spawnTreasure(ConnectionId connectionId, String const& argumentString) {
  if (auto errorMsg = adminCheck(connectionId, "spawn items"))
    return *errorMsg;

  auto arguments = m_parser.tokenizeToStringList(argumentString);

  if (arguments.empty())
    return "Not enough arguments to /spawntreasure";

  try {
    String treasurePool = arguments.at(0);
    float level = 1;

    if (arguments.size() >= 2)
      level = lexicalCast<float>(arguments.at(1));

    bool done = m_universe->executeForClient(connectionId, [&](WorldServer* world, PlayerPtr const& player) {
        auto treasureDatabase = Root::singleton().treasureDatabase();
        for (auto const& treasureItem : treasureDatabase->createTreasure(treasurePool, level, Random::randu64()))
          world->addEntity(ItemDrop::createRandomizedDrop(treasureItem, player->aimPosition()));
      });

    return done ? "" : "Invalid client state";
  } catch (JsonParsingException const& exception) {
    Logger::warn("Error while processing /spawntreasure '{}' command. Json parse problem: {}", arguments.at(0), outputException(exception, false));
    return "Could not parse item parameters";
  } catch (ItemException const& exception) {
    Logger::warn("Error while processing /spawntreasure '{}' command. Item instantiation problem: {}", arguments.at(0), outputException(exception, false));
    return strf("Could not load item '{}'", arguments.at(0));
  } catch (BadLexicalCast const& exception) {
    Logger::warn("Error while processing /spawntreasure command. Number expected. Got something else: {}", outputException(exception, false));
    return strf("Could not load item '{}'", arguments.at(0));
  } catch (StarException const& exception) {
    Logger::warn("Error while processing /spawntreasure command '{}', exception caught: {}", argumentString, outputException(exception, false));
    return strf("Could not load item '{}'", arguments.at(0));
  }
}

String CommandProcessor::spawnMonster(ConnectionId connectionId, String const& argumentString) {
  if (auto errorMsg = adminCheck(connectionId, "spawn monsters"))
    return *errorMsg;

  try {
    auto arguments = m_parser.tokenizeToStringList(argumentString);

    auto monsterDatabase = Root::singleton().monsterDatabase();
    MonsterPtr monster;

    float level = 1;
    if (arguments.size() >= 2)
      level = lexicalCast<float>(arguments.at(1));

    Json parameters = JsonObject();
    if (arguments.size() >= 3)
      parameters = parameters.setAll(Json::parse(arguments.at(2)).toObject());

    monster = monsterDatabase->createMonster(monsterDatabase->randomMonster(arguments.at(0), parameters.toObject()), level);
    bool done = m_universe->executeForClient(connectionId,
        [&](WorldServer* world, PlayerPtr const& player) {
          monster->setPosition(player->aimPosition());
          world->addEntity(monster);
        });

    return done ? "" : "Invalid client state";
  } catch (StarException const& exception) {
    Logger::warn("Could not spawn Monster of type '{}', exception caught: {}", argumentString, outputException(exception, false));
    return strf("Could not spawn Monster of type '{}'", argumentString);
  }
}

String CommandProcessor::spawnNpc(ConnectionId connectionId, String const& argumentString) {
  if (auto errorMsg = adminCheck(connectionId, "spawn NPCs"))
    return *errorMsg;

  auto arguments = m_parser.tokenizeToStringList(argumentString);

  try {
    auto npcDatabase = Root::singleton().npcDatabase();
    float npcLevel = 1;
    uint64_t seed = Random::randu64();
    Json overrides;

    if (arguments.size() < 2)
      return "You must specify a species and NPC type to spawn.";

    if (arguments.size() >= 3)
      npcLevel = lexicalCast<float>(arguments.at(2));
    if (arguments.size() >= 4)
      seed = lexicalCast<uint64_t>(arguments.at(3));
    if (arguments.size() >= 5)
      overrides = Json::parse(arguments.at(4)).toObject();

    auto npc = npcDatabase->createNpc(npcDatabase->generateNpcVariant(arguments.at(0), arguments.at(1), npcLevel, seed, overrides));
    bool done = m_universe->executeForClient(connectionId, [&](WorldServer* world, PlayerPtr const& player) {
        npc->setPosition(player->aimPosition());
        world->addEntity(npc);
      });

    return done ? "" : "Invalid client state";
  } catch (StarException const& exception) {
    Logger::warn("Could not spawn NPC of species '{}', exception caught: {}", argumentString, outputException(exception, true));
    return strf("Could not spawn NPC of species '{}'", argumentString);
  }
}

String CommandProcessor::spawnVehicle(ConnectionId connectionId, String const& argumentString) {
  if (auto errorMsg = adminCheck(connectionId, "spawn vehicles"))
    return *errorMsg;

  try {
    auto vehicleDatabase = Root::singleton().vehicleDatabase();
    auto arguments = m_parser.tokenizeToStringList(argumentString);

    VehiclePtr vehicle;

    String name = arguments.at(0);

    Json parameters = JsonObject();
    if (arguments.size() >= 2)
      parameters = Json::parse(arguments.at(1)).toObject();

    vehicle = vehicleDatabase->create(name, parameters);
    bool done = m_universe->executeForClient(connectionId,
        [&](WorldServer* world, PlayerPtr const& player) {
          vehicle->setPosition(player->aimPosition());
          world->addEntity(std::move(vehicle));
        });

    return done ? "" : "Invalid client state";
  } catch (StarException const& exception) {
    Logger::warn("Could not spawn vehicle, exception caught: {}", outputException(exception, false));
    return strf("Could not spawn vehicle");
  }
}

String CommandProcessor::spawnStagehand(ConnectionId connectionId, String const& argumentString) {
  if (auto errorMsg = adminCheck(connectionId, "spawn stagehands"))
    return *errorMsg;

  try {
    auto arguments = m_parser.tokenizeToStringList(argumentString);

    auto stagehandDatabase = Root::singleton().stagehandDatabase();

    Json parameters = JsonObject();
    if (arguments.size() >= 2)
      parameters = Json::parse(arguments.at(1)).toObject();

    auto stagehand = stagehandDatabase->createStagehand(arguments.at(0), parameters);
    bool done = m_universe->executeForClient(connectionId, [&](WorldServer* world, PlayerPtr player) {
        stagehand->setPosition(player->aimPosition());
        world->addEntity(stagehand);
      });

    return done ? "" : "Invalid client state";
  } catch (StarException const& exception) {
    Logger::warn("Could not spawn Stagehand of type '{}', exception caught: {}", argumentString, outputException(exception, false));
    return strf("Could not spawn Stagehand of type '{}'", argumentString);
  }
}

String CommandProcessor::clearStagehand(ConnectionId connectionId, String const&) {
  if (auto errorMsg = adminCheck(connectionId, "remove stagehands"))
    return *errorMsg;

  unsigned removed = 0;
  bool done = m_universe->executeForClient(connectionId,
      [&](WorldServer* world, PlayerPtr player) {
        auto queryRect = RectF::withCenter(player->aimPosition(), Vec2F{2, 2});
        for (auto stagehand : world->query<Stagehand>(queryRect)) {
          world->removeEntity(stagehand->entityId(), true);
          ++removed;
        }
      });
  return done ? strf("Removed {} stagehands", removed) : "Invalid client state";
}

String CommandProcessor::spawnLiquid(ConnectionId connectionId, String const& argumentString) {
  if (auto errorMsg = adminCheck(connectionId, "spawn liquid"))
    return *errorMsg;

  try {
    auto arguments = m_parser.tokenizeToStringList(argumentString);

    auto liquidsDatabase = Root::singleton().liquidsDatabase();

    if (!liquidsDatabase->isLiquidName(arguments.at(0)))
      return strf("No such liquid {}", arguments.at(0));

    LiquidId liquid = liquidsDatabase->liquidId(arguments.at(0));

    float quantity = 1.0f;
    if (arguments.size() > 1) {
      if (auto maybeQuantity = maybeLexicalCast<float>(arguments.at(1)))
        quantity = *maybeQuantity;
      else
        return strf("Could not parse quantity value '{}'", arguments.at(1));
    }

    bool done = m_universe->executeForClient(connectionId, [&](WorldServer* world, PlayerPtr const& player) {
        world->modifyTile(Vec2I(player->aimPosition().floor()), PlaceLiquid{liquid, quantity}, true);
      });
    return done ? "" : "Invalid client state";

  } catch (StarException const& exception) {
    Logger::warn(
        "Could not spawn liquid '{}', exception caught: {}", argumentString, outputException(exception, false));
    return "Could not spawn liquid.";
  }
}

String CommandProcessor::kick(ConnectionId connectionId, String const& argumentString) {
  if (auto errorMsg = adminCheck(connectionId, "kick a user"))
    return *errorMsg;

  auto arguments = m_parser.tokenizeToStringList(argumentString);

  if (arguments.empty())
    return "No player specified";

  auto toKick = playerCidFromCommand(arguments[0], m_universe);
  if (!toKick)
    return strf("No user with specifier {} found.", arguments[0]);

  // Like IRC, if only the nick is passed then the nick is used as the reason
  if (arguments.size() == 1)
    arguments.append(m_universe->clientNick(*toKick));

  m_universe->disconnectClient(*toKick, arguments[1]);

  return strf("Successfully kicked user with specifier {}. ConnectionId: {}. Reason given: {}",
      arguments[0],
      toKick,
      arguments[1]);
}

String CommandProcessor::ban(ConnectionId connectionId, String const& argumentString) {
  if (auto errorMsg = adminCheck(connectionId, "ban a user"))
    return *errorMsg;

  auto arguments = m_parser.tokenizeToStringList(argumentString);

  if (arguments.empty())
    return "No player specified";

  auto toKick = playerCidFromCommand(arguments[0], m_universe);
  if (!toKick)
    return strf("No user with specifier {} found.", arguments[0]);

  String reason = arguments[0];
  if (arguments.size() < 2)
    reason = m_universe->clientNick(*toKick);
  else
    reason = arguments[1];

  pair<bool, bool> type = {true, true};

  if (arguments.size() >= 3) {
    if (arguments[2] == "ip") {
      type = {true, false};
    } else if (arguments[2] == "uuid") {
      type = {false, true};
    } else if (arguments[2] == "both") {
      type = {true, true};
    } else {
      return strf("Invalid argument {} passed as ban type to /ban.  Options are ip, uuid, or both.", arguments[2]);
    }
  }

  Maybe<int> banTime;
  if (arguments.size() == 4) {
    try {
      banTime = lexicalCast<int>(arguments[3]);
    } catch (BadLexicalCast const&) {
      return strf("Invalid argument {} passed as ban time to /ban.", arguments[3]);
    }
  }

  m_universe->banUser(*toKick, reason, type, banTime);

  return strf("Successfully kicked user with specifier {}. ConnectionId: {}. Reason given: {}",
      arguments[0], toKick, reason);
}

String CommandProcessor::unbanIp(ConnectionId connectionId, String const& argumentString) {
  if (auto errorMsg = adminCheck(connectionId, "unban a user"))
    return *errorMsg;

  auto arguments = m_parser.tokenizeToStringList(argumentString);

  if (arguments.empty())
    return "No IP specified";

  bool success = m_universe->unbanIp(arguments[0]);

  if (success)
    return strf("Successfully removed IP {} from ban list", arguments[0]);
  else
    return strf("'{}' is not a valid IP or was not found in the bans list", arguments[0]);
}

String CommandProcessor::unbanUuid(ConnectionId connectionId, String const& argumentString) {
  if (auto errorMsg = adminCheck(connectionId, "unban a user"))
    return *errorMsg;

  auto arguments = m_parser.tokenizeToStringList(argumentString);

  if (arguments.empty())
    return "No UUID specified";

  bool success = m_universe->unbanUuid(arguments[0]);

  if (success)
    return strf("Successfully removed UUID {} from ban list", arguments[0]);
  else
    return strf("'{}' is not a valid UUID or was not found in the bans list", arguments[0]);
}

String CommandProcessor::list(ConnectionId connectionId, String const&) {
  if (auto errorMsg = adminCheck(connectionId, "list clients"))
    return *errorMsg;

  StringList res;

  auto assets = Root::singleton().assets();
  for (auto cid : m_universe->clientIds())
    res.append(strf("${} : {} : $${}", cid, m_universe->clientNick(cid), m_universe->uuidForClient(cid)->hex()));

  return res.join("\n");
}

String CommandProcessor::clientCoordinate(ConnectionId connectionId, String const& argumentString) {
  ConnectionId targetClientId = connectionId;
  String targetLabel = "Your";
  auto arguments = m_parser.tokenizeToStringList(argumentString);
  if (!adminCheck(connectionId, "find other players")) {
    if (arguments.size() > 0) {
      auto cid = playerCidFromCommand(arguments[0], m_universe);
      if (!cid)
        return strf("No user with specifier {} found.", arguments[0]);
      targetClientId = *cid;
      targetLabel = strf("Client {}'s", arguments[0]);
    }
  }

  if (targetClientId) {
    auto worldId = m_universe->clientWorld(targetClientId);
    return strf("{} current location is {}", targetLabel, worldId);
  } else {
    return "";
  }
}

String CommandProcessor::serverReload(ConnectionId connectionId, String const&) {
  if (auto errorMsg = adminCheck(connectionId, "trigger root reload"))
    return *errorMsg;

  auto& root = Root::singleton();
  root.reload();
  root.fullyLoad();
  return "";
}

String CommandProcessor::eval(ConnectionId connectionId, String const& lua) {
  if (auto errorMsg = localCheck(connectionId, "execute server script"))
    return *errorMsg;

  if (auto errorMsg = adminCheck(connectionId, "execute server script"))
    return *errorMsg;

  return toString(m_scriptComponent.context()->eval(lua));
}

String CommandProcessor::entityEval(ConnectionId connectionId, String const& lua) {
  if (auto errorMsg = localCheck(connectionId, "execute server entity script"))
    return *errorMsg;

  if (auto errorMsg = adminCheck(connectionId, "execute server entity script"))
    return *errorMsg;

  String message;
  bool done = m_universe->executeForClient(connectionId,
      [&lua, &message](WorldServer* world, PlayerPtr const& player) {
        auto queryRect = RectF::withCenter(player->aimPosition(), Vec2F{2, 2});
        auto entities = world->query<ScriptedEntity>(queryRect);
        if (entities.empty()) {
          message = "Could not find scripted entity at cursor";
          return;
        }

        ScriptedEntityPtr targetEntity;
        for (auto const& entity : entities) {
          if (!targetEntity
              || vmagSquared(entity->position() - player->aimPosition())
                  < vmagSquared(targetEntity->position() - player->aimPosition()))
            targetEntity = entity;
        }

        if (auto res = targetEntity->evalScript(lua))
          message = toString(*res);
        else
          message = "Error evaluating script in entity context, check log";
      });

  return done ? message : "failed to do entity eval";
}

String CommandProcessor::enableSpawning(ConnectionId connectionId, String const&) {
  if (auto errorMsg = adminCheck(connectionId, "enable world spawning"))
    return *errorMsg;

  bool done = m_universe->executeForClient(
      connectionId, [](WorldServer* world, PlayerPtr const&) { world->setSpawningEnabled(true); });
  return done ? "enabled monster spawning" : "enabling monster spawning failed";
}

String CommandProcessor::disableSpawning(ConnectionId connectionId, String const&) {
  if (auto errorMsg = adminCheck(connectionId, "disable world spawning"))
    return *errorMsg;

  bool done = m_universe->executeForClient(
      connectionId, [](WorldServer* world, PlayerPtr const&) { world->setSpawningEnabled(false); });
  return done ? "disabled monster spawning" : "disabling monster spawning failed";
}

String CommandProcessor::placeDungeon(ConnectionId connectionId, String const& argumentString) {
  if (auto errorMsg = adminCheck(connectionId, "place dungeons"))
    return *errorMsg;

  auto arguments = m_parser.tokenizeToStringList(argumentString);
  String dungeonName = arguments.at(0);

  Maybe<Vec2I> targetPosition;
  if (arguments.size() > 1) {
    auto pos = arguments.at(1).split(",", 1);
    targetPosition = Vec2I(lexicalCast<int>(pos.at(0)), lexicalCast<int>(pos.at(1)));
  }

  bool done = m_universe->executeForClient(connectionId,
      [dungeonName, targetPosition](WorldServer* world, PlayerPtr const& player) {
        world->placeDungeon(dungeonName, targetPosition.value(Vec2I::floor(player->aimPosition())), true);
      });

  return done ? "" : "Unable to place dungeon " + dungeonName;
}

String CommandProcessor::setUniverseFlag(ConnectionId connectionId, String const& argumentString) {
  if (auto errorMsg = adminCheck(connectionId, "set universe flags"))
    return *errorMsg;

  auto arguments = m_parser.tokenizeToStringList(argumentString);
  String flag = arguments.at(0);
  m_universe->universeSettings()->setFlag(flag);

  return "set universe flag " + flag;
}

String CommandProcessor::resetUniverseFlags(ConnectionId connectionId, String const&) {
  if (auto errorMsg = adminCheck(connectionId, "reset universe flags"))
    return *errorMsg;

  m_universe->universeSettings()->resetFlags();
  return "universe flags reset!";
}

String CommandProcessor::addBiomeRegion(ConnectionId connectionId, String const& argumentString) {
  if (auto errorMsg = adminCheck(connectionId, "add biome regions"))
    return *errorMsg;

  auto arguments = m_parser.tokenizeToStringList(argumentString);

  String biomeName = arguments.at(0);
  int width = lexicalCast<int>(arguments.at(1));

  String subBlockSelector = "largeClumps";
  if (arguments.size() > 2)
    subBlockSelector = arguments.at(2);

  bool done = m_universe->executeForClient(connectionId,
      [biomeName, width, subBlockSelector](WorldServer* world, PlayerPtr const& player) {
        world->addBiomeRegion(Vec2I::floor(player->aimPosition()), biomeName, subBlockSelector, width);
      });

  return done ? strf("added region of biome {} with width {}", biomeName, width) : "failed to add biome region";
}

String CommandProcessor::expandBiomeRegion(ConnectionId connectionId, String const& argumentString) {
  if (auto errorMsg = adminCheck(connectionId, "expand biome regions"))
    return *errorMsg;

  auto arguments = m_parser.tokenizeToStringList(argumentString);

  int newWidth = lexicalCast<int>(arguments.at(0));

  bool done = m_universe->executeForClient(connectionId,
      [newWidth](WorldServer* world, PlayerPtr const& player) {
        world->expandBiomeRegion(Vec2I::floor(player->aimPosition()), newWidth);
      });

  return done ? strf("expanded region to width {}", newWidth) : "failed to expand biome region";
}

String CommandProcessor::updatePlanetType(ConnectionId connectionId, String const& argumentString) {
  if (auto errorMsg = adminCheck(connectionId, "update planet type"))
    return *errorMsg;

  auto arguments = m_parser.tokenizeToStringList(argumentString);

  auto coordinate = CelestialCoordinate(arguments.at(0));
  auto newType = arguments.at(1);
  auto weatherBiome = arguments.at(2);

  bool done = m_universe->updatePlanetType(coordinate, newType, weatherBiome);

  return done ? strf("set planet at {} to type {} weatherBiome {}", coordinate, newType, weatherBiome) : "failed to update planet type";
}

String CommandProcessor::setWeather(ConnectionId connectionId, String const& argumentString) {
  if (auto errorMsg = adminCheck(connectionId, "set weather"))
    return *errorMsg;

  auto arguments = m_parser.tokenizeToStringList(argumentString);

  if (arguments.empty()) {
    StringList list;
    bool done = m_universe->executeForClient(connectionId,
                                             [&list](WorldServer* world, PlayerPtr const&) { list = world->weatherList(); });
    return done ? strf("weathers: {}", list.join(", ")) : "failed to query weather";
  }

  String weatherName = arguments.at(0);
  bool force = false;
  CelestialCoordinate coordinate;

  if (arguments.size() >= 2) {
    if (arguments.at(1) == "force") {
      force = true;
      if (arguments.size() >= 3)
        coordinate = CelestialCoordinate(arguments.at(2));
    } else {
      coordinate = CelestialCoordinate(arguments.at(1));
    }
  }

  bool done;
  if (coordinate.isNull()) {
    done = m_universe->executeForClient(connectionId,
                                        [weatherName, force](WorldServer* world, PlayerPtr const&) { world->setWeather(weatherName, force); });
  } else {
    done = m_universe->setWeather(coordinate, weatherName, force);
  }

  return done ? (coordinate.isNull() ? strf("set weather to {}{}", weatherName, force ? " (forced)" : "") : strf("set weather for {} to {}{}", coordinate, weatherName, force ? " (forced)" : "")) : "failed to set weather";
}


String CommandProcessor::setEnvironmentBiome(ConnectionId connectionId, String const&) {
  if (auto errorMsg = adminCheck(connectionId, "update layer environment biome"))
    return *errorMsg;

  bool done = m_universe->executeForClient(connectionId,
      [](WorldServer* world, PlayerPtr const& player) {
        world->setLayerEnvironmentBiome(Vec2I::floor(player->aimPosition()));
      });

  return done ? "set environment biome for world layer" : "failed to set environment biome";
}

Maybe<ConnectionId> CommandProcessor::playerCidFromCommand(String const& player, UniverseServer* universe) {
  char const* const UsernamePrefix = "@";
  char const* const CidPrefix = "$";
  char const* const UUIDPrefix = "$$";

  if (player.beginsWith(UsernamePrefix)) {
    return universe->findNick(player.substr(strlen(UsernamePrefix)));
  } else if (player.beginsWith(UUIDPrefix)) {
    try {
      auto uuidString = player.substr(strlen(UUIDPrefix));
      return universe->clientForUuid(Uuid(uuidString));
    } catch (UuidException const&) {
      // pass to base case
    }
  } else if (player.beginsWith(CidPrefix)) {
    auto cidString = player.substr(strlen(CidPrefix));
    auto cid = maybeLexicalCast<ConnectionId>(cidString).value(ServerConnectionId);
    if (universe->isConnectedClient(cid))
      return cid;
  }

  return universe->findNick(player);
}

const StringMap<std::function<String(CommandProcessor*, ConnectionId, String)>> CommandProcessor::s_commandMap = []() {
  StringMap<std::function<String(CommandProcessor*, ConnectionId, String)>> map;
	
  auto add = [&map](const char* cmd, String (CommandProcessor::*func)(ConnectionId, const String&)) {
    map[cmd] = [func](CommandProcessor* self, ConnectionId cid, const String& args) {
      return (self->*func)(cid, args);
    };
  };

  // Register all commands
  add("admin", &CommandProcessor::admin);
  add("timewarp", &CommandProcessor::timewarp);
  add("timescale", &CommandProcessor::timescale);
  add("tickrate", &CommandProcessor::tickrate);
  add("serverstatus", &CommandProcessor::serverStatus);
  add("worldstats", &CommandProcessor::worldStats);
  add("servernetstats", &CommandProcessor::serverNetStats);
  add("settileprotection", &CommandProcessor::setTileProtection);
  add("setdungeonid", &CommandProcessor::setDungeonId);
  add("setspawnpoint", &CommandProcessor::setPlayerStart);
  add("spawnitem", &CommandProcessor::spawnItem);
  add("spawntreasure", &CommandProcessor::spawnTreasure);
  add("spawnmonster", &CommandProcessor::spawnMonster);
  add("spawnnpc", &CommandProcessor::spawnNpc);
  add("spawnstagehand", &CommandProcessor::spawnStagehand);
  add("clearstagehand", &CommandProcessor::clearStagehand);
  add("spawnvehicle", &CommandProcessor::spawnVehicle);
  add("spawnliquid", &CommandProcessor::spawnLiquid);
  add("pvp", &CommandProcessor::pvp);
  add("serverwhoami", &CommandProcessor::whoami);
  add("kick", &CommandProcessor::kick);
  add("ban", &CommandProcessor::ban);
  add("unbanip", &CommandProcessor::unbanIp);
  add("unbanuuid", &CommandProcessor::unbanUuid);
  add("list", &CommandProcessor::list);
  add("help", &CommandProcessor::help);
  add("warp", &CommandProcessor::warp);
  add("warprandom", &CommandProcessor::warpRandom);
  add("whereami", &CommandProcessor::clientCoordinate);
  add("whereis", &CommandProcessor::clientCoordinate);
  add("serverreload", &CommandProcessor::serverReload);
  add("eval", &CommandProcessor::eval);
  add("entityeval", &CommandProcessor::entityEval);
  add("enablespawning", &CommandProcessor::enableSpawning);
  add("disablespawning", &CommandProcessor::disableSpawning);
  add("placedungeon", &CommandProcessor::placeDungeon);
  add("setuniverseflag", &CommandProcessor::setUniverseFlag);
  add("resetuniverseflags", &CommandProcessor::resetUniverseFlags);
  add("addbiomeregion", &CommandProcessor::addBiomeRegion);
  add("expandbiomeregion", &CommandProcessor::expandBiomeRegion);
  add("updateplanettype", &CommandProcessor::updatePlanetType);
  add("setweather", &CommandProcessor::setWeather);
  add("setenvironmentbiome", &CommandProcessor::setEnvironmentBiome);

  return map;
}();

String CommandProcessor::handleCommand(ConnectionId connectionId, String const& command, String const& argumentString) {
  auto it = s_commandMap.find(command);
  if (it != s_commandMap.end()) {
    return it->second(this, connectionId, argumentString);
  }
  if (auto res = m_scriptComponent.invoke("command", command, connectionId, jsonFromStringList(m_parser.tokenizeToStringList(argumentString)))) {
    return toString(*res);
  }
  return strf("No such command {}", command);
}

Maybe<String> CommandProcessor::adminCheck(ConnectionId connectionId, String const& commandDescription) const {
  if (connectionId == ServerConnectionId)
    return {};

  auto config = Root::singleton().configuration();
  if (!config->get("allowAdminCommands").toBool())
    return {"Admin commands disabled on this server."};
  if (!config->get("allowAdminCommandsFromAnyone").toBool()) {
    if (!m_universe->isAdmin(connectionId))
      return {strf("Insufficient privileges to {}.", commandDescription)};
  }

  return {};
}

Maybe<String> CommandProcessor::localCheck(ConnectionId connectionId, String const& commandDescription) const {
  if (connectionId == ServerConnectionId)
    return {};

  if (!m_universe->isLocal(connectionId))
    return {strf("The {} command can only be used locally.", commandDescription)};

  return {};
}

LuaCallbacks CommandProcessor::makeCommandCallbacks() {
  LuaCallbacks callbacks;
  callbacks.registerCallbackWithSignature<Maybe<String>, ConnectionId, String>(
      "adminCheck", bind(&CommandProcessor::adminCheck, this, _1, _2));
  return callbacks;
}

}

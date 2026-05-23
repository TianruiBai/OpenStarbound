#include "StarUniverseServer.hpp"
#include "StarAiDatabase.hpp"
#include "StarAssets.hpp"
#include "StarBiomeDatabase.hpp"
#include "StarCelestialDatabase.hpp"
#include "StarChatProcessor.hpp"
#include "StarCommandProcessor.hpp"
#include "StarConfiguration.hpp"
#include "StarEncode.hpp"
#include "StarFile.hpp"
#include "StarJsonExtra.hpp"
#include "StarLogging.hpp"
#include "StarRoot.hpp"
#include "StarSecureRandom.hpp"
#include "StarSha256.hpp"
#include "StarSky.hpp"
#include "StarTcp.hpp"
#include "StarTeamManager.hpp"
#include "StarTime.hpp"
#include "StarUniverseServerLuaBindings.hpp"
#include "StarVersioningDatabase.hpp"
#include "StarWorldTemplate.hpp"

namespace Star {

namespace {

Json readUniverseServerConfig() {
#ifdef STAR_PLATFORM_N3DS
  // Use vanilla universe_server.config from packed assets for speciesShips
  // and other game data. Only override N3DS-specific resource limits.
  auto& root = Root::singleton();
  auto universeConfig = root.assets()->json("/universe_server.config");

  // Apply N3DS resource overrides on top of vanilla config
  universeConfig = universeConfig.set("usePendingConnectionStateMachine", true);
  universeConfig = universeConfig.set("clientWaitLimit", 10000);
  universeConfig = universeConfig.set("persistenceWorkerThreads", 0);
  universeConfig = universeConfig.set("useAsyncPersistence", false);
  universeConfig = universeConfig.set("maxQueuedPersistenceSnapshots", 16);
  universeConfig = universeConfig.set("maxPersistenceWriteRetries", 0);
  universeConfig = universeConfig.set("workerPoolThreads", 0);
  universeConfig = universeConfig.set("networkWorkerThreads", 0);
  universeConfig = universeConfig.set("queueOnlyConnectionSend", false);

  Logger::info("N3DS UniverseServer: merged vanilla config with N3DS overrides");
  return universeConfig;
#else
  auto& root = Root::singleton();
  auto universeConfig = root.assets()->json("/universe_server.config");

  auto configOverrides = root.configuration()->get("universeServerConfigOverrides", {});
  if (configOverrides.isType(Json::Type::Object)) {
    for (auto const& pair : configOverrides.iterateObject())
      universeConfig = universeConfig.set(pair.first, pair.second);
  }

  return universeConfig;
#endif
}

}

UniverseServer::UniverseServer(String const& storageDir)
    : Thread("UniverseServer"),
      m_workerPool("UniverseServerWorkerPool"),
      m_persistenceWorkerPool("UniverseServerPersistencePool"),
      m_clients(MinClientConnectionId, MaxClientConnectionId) {
  m_startTime = Time::monotonicTime();
  String const LockFile = "universe.lock";

  m_storageDirectory = storageDir;
  if (!File::isDirectory(m_storageDirectory)) {
    Logger::info("UniverseServer: Creating universe storage directory");
    File::makeDirectory(m_storageDirectory);
  }

  auto& root = Root::singleton();
  auto assets = root.assets();
  auto configuration = root.configuration();

  if (auto assetsDigestOverride = configuration->get("serverOverrideAssetsDigest").optString()) {
    m_assetsDigest = hexDecode(*assetsDigestOverride);
    Logger::info("UniverseServer: Overriding assets digest as '{}'", *assetsDigestOverride);
  } else {
    m_assetsDigest = assets->digest();
  }

  startLuaScripts();

  m_chatProcessor = make_shared<ChatProcessor>();
#ifdef STAR_PLATFORM_N3DS
  Logger::info("N3DS UniverseServer: skipped command processor");
#else
  m_commandProcessor = make_shared<CommandProcessor>(this, m_luaRoot);
  m_chatProcessor->setCommandHandler(bind(&CommandProcessor::userCommand, m_commandProcessor.get(), _1, _2, _3));
#endif

  Logger::info("UniverseServer: Acquiring universe lock file");

  m_storageDirectoryLock = LockFile::acquireLock(File::relativeTo(m_storageDirectory, LockFile));
  if (!m_storageDirectoryLock)
    throw UniverseServerException("Could not acquire lock for the universe directory");

  if (configuration->get("clearUniverseFiles").toBool()) {
    Logger::info("UniverseServer: Clearing all universe files");
    for (auto file : File::dirList(storageDir)) {
      if (!file.second && file.first != LockFile)
        File::remove(File::relativeTo(storageDir, file.first));
    }
  }

  m_celestialDatabase = make_shared<CelestialMasterDatabase>(File::relativeTo(m_storageDirectory, "universe.chunks"));

  Logger::info("UniverseServer: Loading settings");
  loadSettings();
  loadTempWorldIndex();
#ifdef STAR_PLATFORM_N3DS
  Logger::info("N3DS UniverseServer: settings loaded");
#endif
  m_lastClockUpdateSent = 0.0;
  m_stop = false;
  m_tcpState = TcpState::No;
  m_storageTriggerDeadline = 0;
  m_clearBrokenWorldsDeadline = 0;
  m_nextPendingConnectionId = 1;
  m_pendingConnectionStateWaitLimit = 0;
  m_pendingHandshakeAccepted = 0;
  m_pendingHandshakeFinalized = 0;
  m_pendingHandshakeRejected = 0;
  m_pendingHandshakeTimedOut = 0;
  m_universeTimings.resize(static_cast<size_t>(UniverseTimingPhase::Count));
  m_useAsyncPersistence = false;
  m_persistenceMaxQueuedSnapshots = 0;
  m_persistenceMaxWriteRetries = 0;
  m_persistenceSnapshotsPending = 0;
  m_persistenceBatchesCompleted = 0;
  m_persistenceSnapshotsWritten = 0;
  m_persistenceSnapshotBuildTimeMicroseconds = 0;
  m_persistenceWriteTimeMicroseconds = 0;
  m_persistenceCelestialCommitTimeMicroseconds = 0;
  m_persistenceCelestialCommits = 0;
  m_persistenceFailures = 0;
  m_persistenceWriteRetries = 0;
  m_persistenceSynchronousFallbacks = 0;
  m_persistenceQueueFullFallbacks = 0;

  m_maxPlayers = configuration->get("maxPlayers").toUInt();

  auto universeConfig = readUniverseServerConfig();
#ifdef STAR_PLATFORM_N3DS
  Logger::info("N3DS UniverseServer: compact config ready");
#endif
  m_usePendingConnectionStateMachine = universeConfig.getBool("usePendingConnectionStateMachine", true);
  m_pendingConnectionStateWaitLimit = universeConfig.getInt("clientWaitLimit");
  auto persistenceWorkerThreads = universeConfig.optUInt("persistenceWorkerThreads").value(1);
  m_useAsyncPersistence = universeConfig.getBool("useAsyncPersistence", false) && persistenceWorkerThreads > 0;
  m_persistenceMaxQueuedSnapshots = universeConfig.optUInt("maxQueuedPersistenceSnapshots").value(128);
  m_persistenceMaxWriteRetries = universeConfig.optUInt("maxPersistenceWriteRetries").value(0);

  for (auto const& pair : universeConfig.get("speciesShips").iterateObject())
    m_speciesShips[pair.first] = jsonToStringList(pair.second);
#ifdef STAR_PLATFORM_N3DS
  Logger::info("N3DS UniverseServer: species ships ready");
#endif

  m_teamManager = make_shared<TeamManager>();
#ifdef STAR_PLATFORM_N3DS
  Logger::info("N3DS UniverseServer: team manager ready");
  Logger::info("N3DS UniverseServer: starting worker pool");
#endif
  m_workerPool.start(universeConfig.getUInt("workerPoolThreads"));
#ifdef STAR_PLATFORM_N3DS
  Logger::info("N3DS UniverseServer: worker pool started");
#endif
  if (m_useAsyncPersistence)
    m_persistenceWorkerPool.start(persistenceWorkerThreads);

  size_t networkWorkerThreads = universeConfig.optUInt("networkWorkerThreads").value(0);
  bool queueOnlyConnectionSend = universeConfig.getBool("queueOnlyConnectionSend", false);
#ifdef STAR_PLATFORM_N3DS
  Logger::info("N3DS UniverseServer: creating connection server");
#endif
  m_connectionServer = make_shared<UniverseConnectionServer>(
    bind(&UniverseServer::packetsReceived, this, _1, _2, _3),
    networkWorkerThreads,
    queueOnlyConnectionSend);
#ifdef STAR_PLATFORM_N3DS
  Logger::info("N3DS UniverseServer: connection server ready");
#endif

  m_pause = make_shared<atomic<bool>>(false);
#ifdef STAR_PLATFORM_N3DS
  Logger::info("N3DS UniverseServer: constructor complete");
#endif

  m_secureWarps = Root::singleton().configuration()->getPath("security.secureWarps").optBool().value(true);
}

UniverseServer::~UniverseServer() {
  stop();
  stopLua();
  join();
  finishPendingPersistenceWrites();
  m_persistenceWorkerPool.stop();
  m_workerPool.stop();

  RecursiveMutexLocker locker(m_mainLock);
  WriteLocker clientsLocker(m_clientsLock);

  m_connectionServer->removeAllConnections();
  m_deadConnections.clear();

  // Make sure that all world threads and net sockets (and associated threads)
  // are shutdown before other member destruction.
  m_clients.clear();
  m_worlds.clear();
}

void UniverseServer::setListeningTcp(bool listenTcp) {
  if (!listenTcp || m_tcpState != TcpState::Fuck)
    m_tcpState = listenTcp ? TcpState::Yes : TcpState::No;
}

void UniverseServer::addClient(UniverseConnection remoteConnection) {
  if (m_usePendingConnectionStateMachine) {
    enqueuePendingConnection(std::move(remoteConnection), {});
  } else {
    RecursiveMutexLocker acceptThreadsLocker(m_connectionAcceptThreadsMutex);
    // Binding requires us to make the given lambda copy constructible, so the
    // make_shared is requried here.
    m_connectionAcceptThreads.append(Thread::invoke("UniverseServer::acceptConnection", [this, conn = make_shared<UniverseConnection>(std::move(remoteConnection))]() {
      acceptConnection(std::move(*conn), {});
    }));
  }
}

UniverseConnection UniverseServer::addLocalClient() {
  auto pair = LocalPacketSocket::openPair();
  addClient(UniverseConnection(std::move(pair.first)));
  return UniverseConnection(std::move(pair.second));
}

UniverseConnection UniverseServer::addRemoteLocalClient(HostAddress const& remoteAddress) {
  auto pair = LocalPacketSocket::openPair();
  if (m_usePendingConnectionStateMachine) {
    enqueuePendingConnection(UniverseConnection(std::move(pair.first)), remoteAddress);
  } else {
    RecursiveMutexLocker acceptThreadsLocker(m_connectionAcceptThreadsMutex);
    m_connectionAcceptThreads.append(Thread::invoke("UniverseServer::acceptConnection", [this, conn = make_shared<UniverseConnection>(std::move(pair.first)), remoteAddress]() {
      acceptConnection(std::move(*conn), remoteAddress);
    }));
  }
  return UniverseConnection(std::move(pair.second));
}

void UniverseServer::stop() {
  m_stop = true;
}

void UniverseServer::setPause(bool pause) {
  ReadLocker clientsLocker(m_clientsLock);
  // Pausing is disabled for multiplayer
  if (m_clients.size() > 1)
    pause = false;

  if (pause == *m_pause)
    return;

  *m_pause = pause;

  if (pause)
    m_universeClock->stop();
  else
    m_universeClock->start();

  RecursiveMutexLocker locker(m_mainLock);
  for (auto const& worldId : m_worlds.keys()) {
    if (auto world = getWorld(worldId)) {
      locker.unlock();
      world->setWorldPause(pause);
      locker.lock();
    }
  }
  locker.unlock();

  for (auto& p : m_clients)
    m_connectionServer->sendPackets(p.first, {make_shared<PausePacket>(*m_pause, GlobalTimescale)});
}

void UniverseServer::setTimescale(float timescale) {
  ReadLocker clientsLocker(m_clientsLock);
  GlobalTimescale = timescale;
  for (auto& p : m_clients)
    m_connectionServer->sendPackets(p.first, {make_shared<PausePacket>(*m_pause, GlobalTimescale)});
}

void UniverseServer::setTickRate(float tickRate) {
  ServerGlobalTimestep = 1.0f / tickRate;
}

List<WorldId> UniverseServer::activeWorlds() const {
  RecursiveMutexLocker locker(m_mainLock);
  return m_worlds.keys();
}

bool UniverseServer::isWorldActive(WorldId const& worldId) const {
  RecursiveMutexLocker locker(m_mainLock);
  return m_worlds.contains(worldId);
}

List<ConnectionId> UniverseServer::clientIds() const {
  ReadLocker clientsLocker(m_clientsLock);
  return m_clients.keys();
}

List<pair<ConnectionId, int64_t>> UniverseServer::clientIdsAndCreationTime() const {
  List<pair<ConnectionId, int64_t>> result;
  ReadLocker clientsLocker(m_clientsLock);
  result.reserve(m_clients.size());
  for (auto& pair : m_clients)
    result.emplaceAppend(pair.first, pair.second->creationTime());
  return result;
}

size_t UniverseServer::numberOfClients() const {
  ReadLocker clientsLocker(m_clientsLock);
  return m_clients.size();
}

uint32_t UniverseServer::maxClients() const {
  return m_maxPlayers;
}

UniverseServer::ServerStatus UniverseServer::serverStatus() const {
  ServerStatus status{};
  status.uptime = Time::monotonicTime() - m_startTime;
  status.listeningTcp = m_tcpState == TcpState::Yes;
  status.tcpListenFailed = m_tcpState == TcpState::Fuck;
  status.paused = *m_pause;
  status.timescale = GlobalTimescale;
  status.tickRate = 1.0f / ServerGlobalTimestep;

  {
    ReadLocker clientsLocker(m_clientsLock);
    status.clients = m_clients.size();
    status.maxClients = m_maxPlayers;
  }

  {
    RecursiveMutexLocker acceptThreadsLocker(m_connectionAcceptThreadsMutex);
    status.pendingConnectionAccepts = m_connectionAcceptThreads.size();
  }

  {
    RecursiveMutexLocker locker(m_mainLock);
    List<ServerTimingRecord> worldThreadTimingRecords;
    List<ServerTimingRecord> worldUpdateTimingRecords;
    status.pendingHandshakes = m_pendingConnections.size();
    status.pendingHandshakeAccepted = m_pendingHandshakeAccepted;
    status.pendingHandshakeFinalized = m_pendingHandshakeFinalized;
    status.pendingHandshakeRejected = m_pendingHandshakeRejected;
    status.pendingHandshakeTimedOut = m_pendingHandshakeTimedOut;
    for (auto const& pending : m_pendingConnections) {
      switch (pending->state) {
        case PendingConnectionState::AwaitProtocolRequest:
          status.pendingHandshakeAwaitProtocolRequest++;
          break;
        case PendingConnectionState::SendProtocolResponse:
          status.pendingHandshakeSendProtocolResponse++;
          break;
        case PendingConnectionState::AwaitClientConnect:
          status.pendingHandshakeAwaitClientConnect++;
          break;
        case PendingConnectionState::AwaitHandshakeResponse:
          status.pendingHandshakeAwaitHandshakeResponse++;
          break;
        case PendingConnectionState::FinalizeClient:
          status.pendingHandshakeFinalizeClient++;
          break;
        case PendingConnectionState::RejectAndFlush:
          status.pendingHandshakeRejectAndFlush++;
          break;
        case PendingConnectionState::Dead:
          break;
      }
    }
    status.activeWorlds = m_worlds.size();
    status.systemWorlds = m_systemWorlds.size();
    status.deadConnections = m_deadConnections.size();
    status.pendingPlayerWarps = m_pendingPlayerWarps.size();
    status.queuedFlights = m_queuedFlights.size();
    status.pendingFlights = m_pendingFlights.size();
    status.pendingArrivals = m_pendingArrivals.size();
    status.pendingDisconnections = m_pendingDisconnections.size();
    status.pendingCelestialRequestClients = m_pendingCelestialRequests.size();
    for (auto const& pair : m_pendingCelestialRequests)
      status.pendingCelestialRequests += pair.second.size();
    status.pendingChatClients = m_pendingChat.size();
    for (auto const& pair : m_pendingChat)
      status.pendingChatMessages += pair.second.size();
    status.pendingWorldMessageWorlds = m_pendingWorldMessages.size();
    for (auto const& pair : m_pendingWorldMessages)
      status.pendingWorldMessages += pair.second.size();
    for (auto const& pair : m_worlds) {
      auto const& maybeWorldPromise = pair.second;
      if (!maybeWorldPromise || !maybeWorldPromise->poll())
        continue;

      try {
        if (auto world = maybeWorldPromise->get()) {
          auto commandStats = world->commandStats();
          status.worldCommandQueueDepth += commandStats.pending;
          status.worldCommandOldestPendingAgeMicroseconds = max<int64_t>(status.worldCommandOldestPendingAgeMicroseconds, commandStats.oldestPendingAgeMicroseconds);
          status.worldCommandsProcessed += commandStats.processed;
          status.worldCommandsDirect += commandStats.direct;
          status.worldCommandsFailed += commandStats.failed;
          status.worldCommandWaitMicroseconds += commandStats.waitMicroseconds;

          auto packetPreparationStats = world->packetPreparationStats();
          status.worldPacketPrepTicks += packetPreparationStats.ticks;
          status.worldPacketPrepMonitoringRegionBuilds += packetPreparationStats.monitoringRegionBuilds;
          status.worldPacketPrepMonitoringRegionRects += packetPreparationStats.monitoringRegionRects;
          status.worldPacketPrepMonitoringRegionSplitRects += packetPreparationStats.monitoringRegionSplitRects;
          status.worldPacketPrepMonitoringRegionReuses += packetPreparationStats.monitoringRegionReuses;
          status.worldPacketPrepSectorCacheHits += packetPreparationStats.sectorPacketCacheHits;
          status.worldPacketPrepSectorCacheMisses += packetPreparationStats.sectorPacketCacheMisses;
          status.worldPacketPrepEntityStoreCacheHits += packetPreparationStats.entityStoreCacheHits;
          status.worldPacketPrepEntityStoreCacheMisses += packetPreparationStats.entityStoreCacheMisses;
          status.worldPacketPrepNetStateCacheHits += packetPreparationStats.entityNetStateCacheHits;
          status.worldPacketPrepNetStateCacheMisses += packetPreparationStats.entityNetStateCacheMisses;
          status.worldPacketPrepEntityUpdateSetPackets += packetPreparationStats.entityUpdateSetPackets;
          status.worldPacketPrepEntityUpdateSetDeltas += packetPreparationStats.entityUpdateSetDeltas;
          status.worldPacketPrepEmptyEntityUpdateSetPackets += packetPreparationStats.emptyEntityUpdateSetPackets;
          status.worldPacketPrepEmptyEntityUpdateSetSkips += packetPreparationStats.emptyEntityUpdateSetSkips;
          status.worldPacketPrepSectorClientFanoutLookups += packetPreparationStats.sectorClientFanoutLookups;
          status.worldPacketPrepSectorClientFanoutRecipients += packetPreparationStats.sectorClientFanoutRecipients;
          status.worldPacketPrepSectorClientFanoutMisses += packetPreparationStats.sectorClientFanoutMisses;
          for (auto const& serializationStats : packetPreparationStats.entitySerializationStats)
            status.worldPacketPrepEntitySerializationStats[serializationStats.first].add(serializationStats.second);

          status.worldStorageTimingStats.add(world->storageTimingStats());

          auto phase6Stats = world->phase6WorldParallelismStats();
          if (phase6Stats.storageGenerationPlanningEnabled)
            status.phase6StorageGenerationPlanningEnabledWorlds += 1;
          status.phase6StorageGenerationPlanningTicks += phase6Stats.storageGenerationPlanningTicks;
          status.phase6StorageGenerationPlanningSerialTicks += phase6Stats.storageGenerationPlanningSerialTicks;
          status.phase6StorageGenerationPlanningParallelTicks += phase6Stats.storageGenerationPlanningParallelTicks;
          status.phase6StorageGenerationPlanningSectors += phase6Stats.storageGenerationPlanningSectors;
          status.phase6StorageGenerationPlanningSerialMicroseconds += phase6Stats.storageGenerationPlanningSerialMicroseconds;
          status.phase6StorageGenerationPlanningParallelMicroseconds += phase6Stats.storageGenerationPlanningParallelMicroseconds;
          status.phase6StorageGenerationPlanningMergeMicroseconds += phase6Stats.storageGenerationPlanningMergeMicroseconds;
          status.phase6StorageGenerationPlanningFallbacks += phase6Stats.storageGenerationPlanningFallbacks;
          if (phase6Stats.storageGenerationPlanningDifferentialCheckEnabled)
            status.phase6StorageGenerationPlanningDifferentialCheckEnabledWorlds += 1;
          status.phase6StorageGenerationPlanningDifferentialChecks += phase6Stats.storageGenerationPlanningDifferentialChecks;
          status.phase6StorageGenerationPlanningDifferentialMicroseconds += phase6Stats.storageGenerationPlanningDifferentialMicroseconds;
          status.phase6StorageGenerationPlanningDivergences += phase6Stats.storageGenerationPlanningDivergences;
          if (phase6Stats.packetPreparationSectorPrefillEnabled)
            status.phase6PacketPreparationSectorPrefillEnabledWorlds += 1;
          status.phase6PacketPreparationSectorPrefillTicks += phase6Stats.packetPreparationSectorPrefillTicks;
          status.phase6PacketPreparationSectorPrefillSerialTicks += phase6Stats.packetPreparationSectorPrefillSerialTicks;
          status.phase6PacketPreparationSectorPrefillParallelTicks += phase6Stats.packetPreparationSectorPrefillParallelTicks;
          status.phase6PacketPreparationSectorPrefillSectors += phase6Stats.packetPreparationSectorPrefillSectors;
          status.phase6PacketPreparationSectorPrefillSerialMicroseconds += phase6Stats.packetPreparationSectorPrefillSerialMicroseconds;
          status.phase6PacketPreparationSectorPrefillParallelMicroseconds += phase6Stats.packetPreparationSectorPrefillParallelMicroseconds;
          status.phase6PacketPreparationSectorPrefillMergeMicroseconds += phase6Stats.packetPreparationSectorPrefillMergeMicroseconds;
          status.phase6PacketPreparationSectorPrefillFallbacks += phase6Stats.packetPreparationSectorPrefillFallbacks;
          if (phase6Stats.packetPreparationSectorPrefillDifferentialCheckEnabled)
            status.phase6PacketPreparationSectorPrefillDifferentialCheckEnabledWorlds += 1;
          status.phase6PacketPreparationSectorPrefillDifferentialChecks += phase6Stats.packetPreparationSectorPrefillDifferentialChecks;
          status.phase6PacketPreparationSectorPrefillDifferentialMicroseconds += phase6Stats.packetPreparationSectorPrefillDifferentialMicroseconds;
          status.phase6PacketPreparationSectorPrefillDivergences += phase6Stats.packetPreparationSectorPrefillDivergences;
          if (phase6Stats.storageDirtySectorFilteringEnabled)
            status.phase6StorageDirtySectorFilteringEnabledWorlds += 1;
          if (phase6Stats.subsystemBaselineMetricsEnabled)
            status.phase6SubsystemBaselineMetricsEnabledWorlds += 1;
          status.phase6LiquidBaselineTicks += phase6Stats.liquidBaselineTicks;
          status.phase6LiquidActiveCells += phase6Stats.liquidActiveCells;
          status.phase6LiquidMonitoringRegions += phase6Stats.liquidMonitoringRegions;
          status.phase6LiquidNoProcessingLimitRegionCacheBuilds += phase6Stats.liquidNoProcessingLimitRegionCacheBuilds;
          status.phase6LiquidNoProcessingLimitRegionCacheRebuildSkips += phase6Stats.liquidNoProcessingLimitRegionCacheRebuildSkips;
          status.phase6LiquidNoProcessingLimitRegionCacheRegions += phase6Stats.liquidNoProcessingLimitRegionCacheRegions;
          status.phase6LiquidNoProcessingLimitRegionCacheBuckets += phase6Stats.liquidNoProcessingLimitRegionCacheBuckets;
          status.phase6LiquidNoProcessingLimitRegionCacheLookups += phase6Stats.liquidNoProcessingLimitRegionCacheLookups;
          status.phase6LiquidNoProcessingLimitRegionCacheCandidates += phase6Stats.liquidNoProcessingLimitRegionCacheCandidates;
          status.phase6LiquidNoProcessingLimitRegionCacheHits += phase6Stats.liquidNoProcessingLimitRegionCacheHits;
          if (phase6Stats.mutationFixedSeedSignaturesEnabled)
            status.phase6MutationFixedSeedSignaturesEnabledWorlds += 1;
          status.phase6LiquidSignatureTicks += phase6Stats.liquidSignatureTicks;
          status.phase6LiquidSignatureActiveCells += phase6Stats.liquidSignatureActiveCells;
          status.phase6LiquidSignatureActiveCellHash ^= phase6Stats.liquidSignatureActiveCellHash;
          status.phase6LiquidSignatureRegionHash ^= phase6Stats.liquidSignatureRegionHash;
          status.phase6FallingBlocksBaselineTicks += phase6Stats.fallingBlocksBaselineTicks;
          status.phase6FallingBlocksPendingPositions += phase6Stats.fallingBlocksPendingPositions;
          status.phase6FallingBlocksNextPendingPositions += phase6Stats.fallingBlocksNextPendingPositions;
          status.phase6FallingBlocksProcessedPositions += phase6Stats.fallingBlocksProcessedPositions;
          status.phase6FallingBlocksMovedBlocks += phase6Stats.fallingBlocksMovedBlocks;
          status.phase6FallingBlocksSignatureTicks += phase6Stats.fallingBlocksSignatureTicks;
          status.phase6FallingBlocksPendingPositionSignature ^= phase6Stats.fallingBlocksPendingPositionSignature;
          status.phase6FallingBlocksProcessedPositionSignature ^= phase6Stats.fallingBlocksProcessedPositionSignature;
          status.phase6FallingBlocksMovedBlockSignature ^= phase6Stats.fallingBlocksMovedBlockSignature;
          status.phase6FallingBlocksNextPendingPositionSignature ^= phase6Stats.fallingBlocksNextPendingPositionSignature;
          status.phase6WiringBaselineTicks += phase6Stats.wiringBaselineTicks;
          status.phase6WiringInitialEntities += phase6Stats.wiringInitialEntities;
          status.phase6WiringLoadedEntities += phase6Stats.wiringLoadedEntities;
          status.phase6WiringNetworkLoads += phase6Stats.wiringNetworkLoads;
          status.phase6WiringEvaluatedEntities += phase6Stats.wiringEvaluatedEntities;
          status.phase6WiringNetworkSignatureChecks += phase6Stats.wiringNetworkSignatureChecks;
          status.phase6WiringCleanNetworkSignatures += phase6Stats.wiringCleanNetworkSignatures;
          status.phase6WiringDirtyNetworkSignatures += phase6Stats.wiringDirtyNetworkSignatures;
          status.phase6WiringTopologyDirtyNetworkSignatures += phase6Stats.wiringTopologyDirtyNetworkSignatures;
          status.phase6WiringOutputDirtyNetworkSignatures += phase6Stats.wiringOutputDirtyNetworkSignatures;
          status.phase6WiringCleanNetworkEntities += phase6Stats.wiringCleanNetworkEntities;
          status.phase6WiringDirtyNetworkEntities += phase6Stats.wiringDirtyNetworkEntities;
          status.phase6WiringSignatureTicks += phase6Stats.wiringSignatureTicks;
          status.phase6WiringTopologySignatureHash ^= phase6Stats.wiringTopologySignatureHash;
          status.phase6WiringOutputSignatureHash ^= phase6Stats.wiringOutputSignatureHash;
          status.phase6EntityBaselineTicks += phase6Stats.entityBaselineTicks;
          status.phase6EntityUpdatedEntities += phase6Stats.entityUpdatedEntities;
          status.phase6EntityTileEntities += phase6Stats.entityTileEntities;
          status.phase6EntityDestroyedEntities += phase6Stats.entityDestroyedEntities;
          status.phase6EntityIterationCopies += phase6Stats.entityIterationCopies;
          status.phase6EntitySortedEntities += phase6Stats.entitySortedEntities;
          status.phase6EntityCopyMicroseconds += phase6Stats.entityCopyMicroseconds;
          status.phase6EntitySortMicroseconds += phase6Stats.entitySortMicroseconds;
          status.phase6EntityUpdateMicroseconds += phase6Stats.entityUpdateMicroseconds;
          status.phase6EntityMetadataRefreshMicroseconds += phase6Stats.entityMetadataRefreshMicroseconds;
          for (auto const& attributionStats : phase6Stats.entityUpdateAttributionStats)
            status.phase6EntityUpdateAttributionStats[attributionStats.first].add(attributionStats.second);
          status.phase6LuaBaselineTicks += phase6Stats.luaBaselineTicks;
          status.phase6LuaScriptContexts += phase6Stats.luaScriptContexts;
          status.phase6LuaScriptUpdates += phase6Stats.luaScriptUpdates;
          status.phase6LuaReadyScriptUpdates += phase6Stats.luaReadyScriptUpdates;
          status.phase6LuaScriptUpdateMicroseconds += phase6Stats.luaScriptUpdateMicroseconds;
          status.phase6LuaMaxScriptUpdateMicroseconds = max(status.phase6LuaMaxScriptUpdateMicroseconds, phase6Stats.luaMaxScriptUpdateMicroseconds);
          for (auto const& contextStats : phase6Stats.luaScriptContextStats)
            status.phase6LuaScriptContextStats[contextStats.first].add(contextStats.second);
          status.phase6MutationParallelismRequestedSubsystems |= phase6Stats.mutationParallelismRequestedSubsystems;
          status.phase6MutationParallelismBlockedByFixedSeedGateSubsystems |= phase6Stats.mutationParallelismBlockedByFixedSeedGateSubsystems;
          status.phase6MutationParallelismBlockedByDependencyGateSubsystems |= phase6Stats.mutationParallelismBlockedByDependencyGateSubsystems;
          status.phase6MutationParallelismBlockedByModVisibilityGateSubsystems |= phase6Stats.mutationParallelismBlockedByModVisibilityGateSubsystems;
          status.phase6MutationParallelismBlockedByImplementationGateSubsystems |= phase6Stats.mutationParallelismBlockedByImplementationGateSubsystems;
          status.phase6MutationParallelismWorkerSubsystems |= phase6Stats.mutationParallelismWorkerSubsystems;
          status.phase6MutationWorkerTicks += phase6Stats.mutationWorkerTicks;
          status.phase6MutationWorkerJobs += phase6Stats.mutationWorkerJobs;
          status.phase6MutationWorkerMicroseconds += phase6Stats.mutationWorkerMicroseconds;
          status.phase6MutationWorkerMergeMicroseconds += phase6Stats.mutationWorkerMergeMicroseconds;
          status.phase6MutationWorkerDifferentialChecks += phase6Stats.mutationWorkerDifferentialChecks;
          status.phase6MutationWorkerDivergences += phase6Stats.mutationWorkerDivergences;
          status.phase6MutationWorkerFallbacks += phase6Stats.mutationWorkerFallbacks;

          for (auto const& timingRecord : world->threadTimingRecords())
            mergeServerTimingRecord(worldThreadTimingRecords, timingRecord);
          for (auto const& timingRecord : world->worldTimingRecords())
            mergeServerTimingRecord(worldUpdateTimingRecords, timingRecord);
        }
      } catch (std::exception const&) {
      }
    }
    status.worldThreadTimings = serverTimingStatusList(worldThreadTimingRecords);
    status.worldUpdateTimings = serverTimingStatusList(worldUpdateTimingRecords);
    status.persistenceAsyncEnabled = m_useAsyncPersistence;
    status.persistenceBatchesPending = m_pendingPersistenceWrites.size();
    status.persistenceSnapshotsPending = m_persistenceSnapshotsPending;
    auto now = Time::monotonicMilliseconds();
    for (auto const& pendingWrite : m_pendingPersistenceWrites) {
      auto age = now - pendingWrite.queuedTime;
      if (age > status.persistenceOldestPendingAgeMilliseconds)
        status.persistenceOldestPendingAgeMilliseconds = age;
    }
    status.persistenceBatchesCompleted = m_persistenceBatchesCompleted;
    status.persistenceSnapshotsWritten = m_persistenceSnapshotsWritten;
    status.persistenceSnapshotBuildTimeMicroseconds = m_persistenceSnapshotBuildTimeMicroseconds;
    status.persistenceWriteTimeMicroseconds = m_persistenceWriteTimeMicroseconds;
    status.persistenceCelestialCommitTimeMicroseconds = m_persistenceCelestialCommitTimeMicroseconds;
    status.persistenceCelestialCommits = m_persistenceCelestialCommits;
    status.persistenceFailures = m_persistenceFailures;
    status.persistenceWriteRetries = m_persistenceWriteRetries;
    status.persistenceSynchronousFallbacks = m_persistenceSynchronousFallbacks;
    status.persistenceQueueFullFallbacks = m_persistenceQueueFullFallbacks;
  }

  {
    MutexLocker timingLocker(m_universeTimingsMutex);
    for (size_t i = 0; i < m_universeTimings.size(); ++i) {
      auto phase = static_cast<UniverseTimingPhase>(i);
      status.universeTimings.append(timingStatus(universeTimingPhaseName(phase), m_universeTimings[i]));
    }
  }

  auto workerStats = connectionWorkerStats();
  status.networkQueueOnlySends = m_connectionServer->queueOnlySends();
  status.networkWorkers = workerStats.size();
  for (auto const& worker : workerStats) {
    status.networkOwnedConnections += worker.ownedConnections;
    status.networkPacketsProcessed += worker.packetsProcessed;
    status.networkQueuedSendBatches += worker.queuedSendBatches;
    status.networkQueuedSendPackets += worker.queuedSendPackets;
    status.networkEagerSendBatches += worker.eagerSendBatches;
    status.networkEagerSendPackets += worker.eagerSendPackets;
    status.networkEagerWriteTimeMicroseconds += worker.eagerWriteTimeMicroseconds;
    status.networkWorkerSendBatches += worker.workerSendBatches;
    status.networkWorkerSendPackets += worker.workerSendPackets;
    status.networkWorkerWriteTimeMicroseconds += worker.workerWriteTimeMicroseconds;
    status.networkWakeups += worker.wakeups;
    status.networkIdleTimedWaits += worker.idleTimedWaits;
  }

  return status;
}

UniverseServer::WorldStatsSummary UniverseServer::worldStats() const {
  WorldStatsSummary summary;
  RecursiveMutexLocker locker(m_mainLock);

  for (auto const& pair : m_worlds) {
    WorldStats stats;
    stats.worldId = pair.first;

    auto const& maybeWorldPromise = pair.second;
    if (!maybeWorldPromise) {
      stats.loading = true;
      summary.worlds.append(std::move(stats));
      continue;
    }

    try {
      if (!maybeWorldPromise->poll()) {
        stats.loading = true;
      } else if (auto world = maybeWorldPromise->get()) {
        stats.loaded = true;
        stats.clients = world->clients().size();
        stats.commandStats = world->commandStats();
        stats.packetPreparationStats = world->packetPreparationStats();
        stats.storageTimingStats = world->storageTimingStats();
        stats.phase6WorldParallelismStats = world->phase6WorldParallelismStats();
        stats.threadTimings = world->threadTimingStatus();
        stats.worldTimings = world->worldTimingStatus();
      } else {
        stats.errored = true;
      }
    } catch (std::exception const&) {
      stats.errored = true;
    }

    summary.worlds.append(std::move(stats));
  }

  for (auto const& pair : m_systemWorlds) {
    SystemWorldStats stats;
    stats.location = pair.first;
    if (auto const& systemWorld = pair.second) {
      stats.clients = systemWorld->clients().size();
      stats.activeInstanceWorlds = systemWorld->activeInstanceWorlds().size();
      stats.commandStats = systemWorld->commandStats();
      stats.packetStats = systemWorld->packetStats();
    }
    summary.systemWorlds.append(std::move(stats));
  }

  return summary;
}

List<UniverseConnectionServer::NetworkWorkerStats> UniverseServer::connectionWorkerStats() const {
  return m_connectionServer->workerStats();
}

char const* UniverseServer::universeTimingPhaseName(UniverseTimingPhase phase) {
  switch (phase) {
    case UniverseTimingPhase::Loop:
      return "loop";
    case UniverseTimingPhase::UpdateLua:
      return "lua";
    case UniverseTimingPhase::UniverseFlags:
      return "flags";
    case UniverseTimingPhase::TimedBans:
      return "bans";
    case UniverseTimingPhase::SendPendingChat:
      return "sendChat";
    case UniverseTimingPhase::Teams:
      return "teams";
    case UniverseTimingPhase::Ships:
      return "ships";
    case UniverseTimingPhase::ClockUpdates:
      return "clocks";
    case UniverseTimingPhase::KickErroredPlayers:
      return "kicks";
    case UniverseTimingPhase::ReapConnections:
      return "connections";
    case UniverseTimingPhase::PendingHandshakes:
      return "handshakes";
    case UniverseTimingPhase::PlanetTypeChanges:
      return "planetTypes";
    case UniverseTimingPhase::Warps:
      return "warps";
    case UniverseTimingPhase::ShipFlights:
      return "flights";
    case UniverseTimingPhase::ShipArrivals:
      return "arrivals";
    case UniverseTimingPhase::Chat:
      return "chat";
    case UniverseTimingPhase::ClientContextUpdates:
      return "clientContexts";
    case UniverseTimingPhase::CelestialRequests:
      return "celestial";
    case UniverseTimingPhase::BrokenWorlds:
      return "brokenWorlds";
    case UniverseTimingPhase::WorldMessages:
      return "worldMessages";
    case UniverseTimingPhase::InactiveWorlds:
      return "inactiveWorlds";
    case UniverseTimingPhase::PersistenceCompletions:
      return "persistence";
    case UniverseTimingPhase::TriggeredStorage:
      return "storage";
    case UniverseTimingPhase::Count:
      return "count";
  }

  return "unknown";
}

UniverseServer::ServerStatus::TimingStatus UniverseServer::timingStatus(char const* name, ServerTimingAccumulator const& timing) {
  return serverTimingStatus(name, timing);
}

void UniverseServer::recordUniverseTiming(UniverseTimingPhase phase, int64_t durationMicroseconds) {
  auto index = static_cast<size_t>(phase);
  if (durationMicroseconds < 0 || index >= m_universeTimings.size())
    return;

  MutexLocker timingLocker(m_universeTimingsMutex);
  recordServerTiming(m_universeTimings[index], durationMicroseconds);
}

bool UniverseServer::isConnectedClient(ConnectionId clientId) const {
  ReadLocker clientsLocker(m_clientsLock);
  return m_clients.contains(clientId);
}

String UniverseServer::clientDescriptor(ConnectionId clientId) const {
  ReadLocker clientsLocker(m_clientsLock);
  if (auto clientContext = m_clients.value(clientId))
    return clientContext->descriptiveName();
  else
    return "disconnected_client";
}

String UniverseServer::clientNick(ConnectionId clientId) const {
  return m_chatProcessor->connectionNick(clientId);
}

Maybe<ConnectionId> UniverseServer::findNick(String const& nick) const {
  return m_chatProcessor->findNick(nick);
}

Maybe<Uuid> UniverseServer::uuidForClient(ConnectionId clientId) const {
  ReadLocker clientsLocker(m_clientsLock);
  if (auto clientContext = m_clients.value(clientId))
    return clientContext->playerUuid();
  return {};
}

Maybe<ConnectionId> UniverseServer::clientForUuid(Uuid const& uuid) const {
  ReadLocker clientsLocker(m_clientsLock);
  return getClientForUuid(uuid);
}

void UniverseServer::adminBroadcast(String const& text) {
  m_chatProcessor->adminBroadcast(text);
}

void UniverseServer::adminWhisper(ConnectionId clientId, String const& text) {
  m_chatProcessor->adminWhisper(clientId, text);
}

String UniverseServer::adminCommand(String text) {
#ifdef STAR_PLATFORM_N3DS
  return "Commands are unavailable on N3DS";
#else
  String command = text.extract();
  return m_commandProcessor->adminCommand(command, text);
#endif
}

bool UniverseServer::isAdmin(ConnectionId clientId) const {
  ReadLocker clientsLocker(m_clientsLock);
  if (auto clientContext = m_clients.value(clientId))
    return clientContext->isAdmin();
  return false;
}

bool UniverseServer::canBecomeAdmin(ConnectionId clientId) const {
  ReadLocker clientsLocker(m_clientsLock);
  if (auto clientContext = m_clients.value(clientId))
    return clientContext->canBecomeAdmin();
  return false;
}

void UniverseServer::setAdmin(ConnectionId clientId, bool admin) {
  ReadLocker clientsLocker(m_clientsLock);
  if (auto clientContext = m_clients.value(clientId))
    clientContext->setAdmin(admin);
}

bool UniverseServer::isLocal(ConnectionId clientId) const {
  ReadLocker clientsLocker(m_clientsLock);
  if (auto clientContext = m_clients.value(clientId))
    return !clientContext->remoteAddress();
  return false;
}

bool UniverseServer::isPvp(ConnectionId clientId) const {
  ReadLocker clientsLocker(m_clientsLock);
  if (auto clientContext = m_clients.value(clientId))
    return clientContext->team().type == TeamType::PVP;
  return false;
}

void UniverseServer::setPvp(ConnectionId clientId, bool pvp) {
  RecursiveMutexLocker locker(m_mainLock);
  ReadLocker clientsLocker(m_clientsLock);
  if (auto clientContext = m_clients.value(clientId)) {
    if (pvp) {
      TeamNumber pvpTeam = m_teamManager->getPvpTeam(clientContext->playerUuid());
      if (pvpTeam == 0)
        pvpTeam = soloPvpTeam(clientId);
      clientContext->setTeam(EntityDamageTeam(TeamType::PVP, pvpTeam));
    } else
      clientContext->setTeam(EntityDamageTeam(TeamType::Friendly));
  }
}

RpcThreadPromise<Json> UniverseServer::sendWorldMessage(WorldId const& worldId, String const& message, JsonArray const& args) {
  auto pair = RpcThreadPromise<Json>::createPair();
  RecursiveMutexLocker locker(m_mainLock);
  m_pendingWorldMessages[worldId].push_back({message, args, pair.second});
  return pair.first;
}

void UniverseServer::clientWarpPlayer(ConnectionId clientId, WarpAction action, bool deploy) {
  RecursiveMutexLocker locker(m_mainLock);
  m_pendingPlayerWarps[clientId] = pair<WarpAction, bool>(std::move(action), std::move(deploy));
}

void UniverseServer::clientFlyShip(ConnectionId clientId, Vec3I const& system, SystemLocation const& location, Json const& settings) {
  RecursiveMutexLocker locker(m_mainLock);
  ReadLocker clientsLocker(m_clientsLock);

  if (m_pendingFlights.contains(clientId) || m_queuedFlights.contains(clientId))
    return;

  auto clientContext = m_clients.get(clientId);
  if (!clientContext)
    return;

  if (system == Vec3I()) {
    m_pendingFlights.set(clientId, make_tuple(Vec3I(), SystemLocation(), settings));// find starter world
    return;
  }

  auto clientSystem = clientContext->systemWorld();
  bool sameSystem = clientSystem && clientSystem->location() == system;
  bool sameLocation = clientSystem && clientSystem->clientShipLocation(clientId) == location;
  if (m_pendingArrivals.contains(clientId) && sameSystem && location && !sameLocation) {
    // for continuing flight within a system, set the new destination immediately
    clientSystem->setClientDestination(clientId, location);
    return;
  }

  // don't switch systems while already flying
  if (!m_pendingArrivals.contains(clientId) || sameSystem)
    m_pendingFlights.set(clientId, make_tuple(system, location, settings));
}

WorldId UniverseServer::clientWorld(ConnectionId clientId) const {
  RecursiveMutexLocker locker(m_mainLock);
  ReadLocker clientsLocker(m_clientsLock);
  if (auto clientContext = m_clients.value(clientId))
    return clientContext->playerWorldId();
  return WorldId();
}

CelestialCoordinate UniverseServer::clientShipCoordinate(ConnectionId clientId) const {
  ReadLocker clientsLocker(m_clientsLock);
  if (auto clientContext = m_clients.value(clientId))
    return clientContext->shipCoordinate();
  return CelestialCoordinate();
}

ClockPtr UniverseServer::universeClock() const {
  return m_universeClock;
}

UniverseSettingsPtr UniverseServer::universeSettings() const {
  return m_universeSettings;
}

CelestialDatabase& UniverseServer::celestialDatabase() {
  return *m_celestialDatabase;
}

bool UniverseServer::executeForClient(ConnectionId clientId, function<void(WorldServer*, PlayerPtr)> action) {
  RecursiveMutexLocker locker(m_mainLock);
  ReadLocker clientsLocker(m_clientsLock);
  bool success = false;
  if (auto clientContext = m_clients.value(clientId)) {
    if (auto currentWorld = clientContext->playerWorld()) {
      locker.unlock();
      success = currentWorld->executeForClient(clientId, [this, action](WorldServer* worldServer, PlayerPtr player) {
          RecursiveMutexLocker actionLocker(m_mainLock);
          action(worldServer, player);
        });
    }
  }
  return success;
}

void UniverseServer::disconnectClient(ConnectionId clientId, String const& reason) {
  RecursiveMutexLocker locker(m_mainLock);
  m_pendingDisconnections.add(clientId, reason);
}

void UniverseServer::banUser(ConnectionId clientId, String const& reason, pair<bool, bool> banType, Maybe<int> timeout) {
  RecursiveMutexLocker locker(m_mainLock);

  if (timeout)
    doTempBan(clientId, reason, banType, *timeout);
  else
    doPermBan(clientId, reason, banType);

  m_pendingDisconnections.add(clientId, reason);
}

bool UniverseServer::unbanUuid(String const& uuidString) {
  RecursiveMutexLocker locker(m_mainLock);

  bool entryFound = false;

  auto config = Root::singleton().configuration();
  auto bannedUuids = config->get("bannedUuids").toArray();

  eraseWhere(bannedUuids, [&](Json const& entry) {
    if (entry.getString("uuid") == uuidString) {
      entryFound = true;
      return true;
    }
    return false;
  });
  config->set("bannedUuids", bannedUuids);

  eraseWhere(m_tempBans, [&](TimeoutBan const& b) {
    if (b.uuid && b.uuid->hex() == uuidString) {
      entryFound = true;
      return true;
    }
    return false;
  });

  return entryFound;
}

bool UniverseServer::unbanIp(String const& addressString) {
  RecursiveMutexLocker locker(m_mainLock);

  auto addressLookup = HostAddress::lookup(addressString);
  if (addressLookup.isLeft()) {
    return false;
  } else {
    HostAddress address = std::move(addressLookup.right());
    String cleanAddressString = toString(address);

    bool entryFound = false;

    auto config = Root::singleton().configuration();
    auto bannedIPs = config->get("bannedIPs").toArray();
    eraseWhere(bannedIPs, [&](Json const& entry) {
      if (entry.getString("ip") == cleanAddressString) {
        entryFound = true;
        return true;
      }
      return false;
    });
    config->set("bannedIPs", bannedIPs);

    eraseWhere(m_tempBans, [&](TimeoutBan const& b) {
      if (b.ip && b.ip.value() == address) {
        entryFound = true;
        return true;
      }
      return false;
    });

    return entryFound;
  }
}

bool UniverseServer::updatePlanetType(CelestialCoordinate const& coordinate, String const& newType, String const& weatherBiome) {
  RecursiveMutexLocker locker(m_mainLock);

  if (!coordinate.isNull() && m_celestialDatabase->coordinateValid(coordinate)) {
    if (auto celestialParameters = m_celestialDatabase->parameters(coordinate)) {
      if (auto terrestrialParameters = as<TerrestrialWorldParameters>(celestialParameters->visitableParameters())) {
        auto newTerrestrialParameters = make_shared<TerrestrialWorldParameters>(*terrestrialParameters);
        newTerrestrialParameters->typeName = newType;

        auto biomeDatabase = Root::singleton().biomeDatabase();
        auto newWeatherPool = biomeDatabase->biomeWeathers(weatherBiome, celestialParameters->seed(), terrestrialParameters->threatLevel);
        newTerrestrialParameters->weatherPool = newWeatherPool;

        newTerrestrialParameters->terraformed = true;

        celestialParameters->setVisitableParameters(newTerrestrialParameters);

        m_celestialDatabase->updateParameters(coordinate, *celestialParameters);

        ReadLocker clientsLocker(m_clientsLock);

        for (auto clientId : m_clients.keys())
          m_connectionServer->sendPackets(clientId, {make_shared<PlanetTypeUpdatePacket>(coordinate)});

        return true;
      }
    }
  }

  return false;
}

bool UniverseServer::setWeather(CelestialCoordinate const& coordinate, String const& weatherName, bool force) {
  RecursiveMutexLocker locker(m_mainLock);

  if (!coordinate.isNull() && m_celestialDatabase->coordinateValid(coordinate)) {
    if (auto world = createWorld(CelestialWorldId(coordinate))) {
      locker.unlock();
      world->setWeather(weatherName, force);
      return true;
    }
  }

  return false;
}

StringList UniverseServer::weatherList(CelestialCoordinate const& coordinate) {
  RecursiveMutexLocker locker(m_mainLock);

  StringList result;
  if (!coordinate.isNull() && m_celestialDatabase->coordinateValid(coordinate)) {
    if (auto world = createWorld(CelestialWorldId(coordinate))) {
      locker.unlock();
      result = world->weatherList();
    }
  }

  return result;
}

bool UniverseServer::sendPacket(ConnectionId clientId, PacketPtr packet) {
  RecursiveMutexLocker locker(m_mainLock);
  ReadLocker clientsLocker(m_clientsLock);
  if (m_clients.contains(clientId)) {
    clientsLocker.unlock();
    m_connectionServer->sendPackets(clientId, {packet});
    return true;
  }
  return false;
}

void UniverseServer::run() {
  Logger::info("UniverseServer: Starting UniverseServer with UUID: {}", m_universeSettings->uuid().hex());

#ifdef STAR_PLATFORM_N3DS
  int mainWakeupInterval = 10;
#else
  int mainWakeupInterval = Root::singleton().assets()->json("/universe_server.config:mainWakeupInterval").toInt();
#endif

  TcpServerPtr tcpServer;

  while (!m_stop) {
    if (m_tcpState == TcpState::Yes && !tcpServer) {
      auto& root = Root::singleton();
      auto configuration = root.configuration();
      auto assets = root.assets();
      HostAddressWithPort bindAddress(configuration->get("gameServerBind").toString(), configuration->get("gameServerPort").toUInt());
    #ifdef STAR_PLATFORM_N3DS
      unsigned maxPendingConnections = 1;
    #else
      unsigned maxPendingConnections = assets->json("/universe_server.config:maxPendingConnections").toInt();
    #endif

      Logger::info("UniverseServer: listening for incoming TCP connections on {}", bindAddress);

      try {
        tcpServer = make_shared<TcpServer>(bindAddress);
        tcpServer->setAcceptCallback([this, maxPendingConnections](TcpSocketPtr socket) {
          size_t pendingAccepts = 0;
          if (m_usePendingConnectionStateMachine) {
            RecursiveMutexLocker locker(m_mainLock);
            pendingAccepts = m_pendingConnections.size();
          } else {
            RecursiveMutexLocker acceptThreadsLocker(m_connectionAcceptThreadsMutex);
            pendingAccepts = m_connectionAcceptThreads.size();
          }

          if (pendingAccepts < maxPendingConnections) {
            Logger::info("UniverseServer: Connection received from: {}", socket->remoteAddress());
            if (m_usePendingConnectionStateMachine) {
              auto remoteAddress = socket->remoteAddress().address();
              enqueuePendingConnection(UniverseConnection(TcpPacketSocket::open(socket)), remoteAddress);
            } else {
              RecursiveMutexLocker acceptThreadsLocker(m_connectionAcceptThreadsMutex);
              m_connectionAcceptThreads.append(Thread::invoke("UniverseServer::acceptConnection", [this, socket]() {
                acceptConnection(UniverseConnection(TcpPacketSocket::open(socket)), socket->remoteAddress().address());
              }));
            }
          } else {
            Logger::warn("UniverseServer: maximum pending connections, dropping connection from: {}", socket->remoteAddress().address());
          }
        });
      } catch (StarException const& e) {
        Logger::error("UniverseServer: Error setting up TCP, cannot accept connections: {}", e.what());
        m_tcpState = TcpState::Fuck;
        tcpServer.reset();
      }
    } else if (m_tcpState == TcpState::No && tcpServer) {
      Logger::info("UniverseServer: Not listening for incoming TCP connections");
      tcpServer.reset();
    }

    LogMap::set("universe_time", m_universeClock->time());

    auto loopStart = Time::monotonicMicroseconds();
    auto timePhase = [this](UniverseTimingPhase phase, auto&& action) {
      auto phaseStart = Time::monotonicMicroseconds();
      action();
      recordUniverseTiming(phase, Time::monotonicMicroseconds() - phaseStart);
    };

    try {
      timePhase(UniverseTimingPhase::UpdateLua, [&]() { updateLua(); });
      timePhase(UniverseTimingPhase::UniverseFlags, [&]() { processUniverseFlags(); });
      timePhase(UniverseTimingPhase::TimedBans, [&]() { removeTimedBan(); });
      timePhase(UniverseTimingPhase::SendPendingChat, [&]() { sendPendingChat(); });
      timePhase(UniverseTimingPhase::Teams, [&]() { updateTeams(); });
      timePhase(UniverseTimingPhase::Ships, [&]() { updateShips(); });
      timePhase(UniverseTimingPhase::ClockUpdates, [&]() { sendClockUpdates(); });
      timePhase(UniverseTimingPhase::KickErroredPlayers, [&]() { kickErroredPlayers(); });
      timePhase(UniverseTimingPhase::ReapConnections, [&]() {
        reapConnections();
#ifdef STAR_PLATFORM_N3DS
        if (m_connectionServer)
          m_connectionServer->update();
#endif
      });
      timePhase(UniverseTimingPhase::PendingHandshakes, [&]() { processPendingConnections(); });
      timePhase(UniverseTimingPhase::PlanetTypeChanges, [&]() { processPlanetTypeChanges(); });
      timePhase(UniverseTimingPhase::Warps, [&]() { warpPlayers(); });
      timePhase(UniverseTimingPhase::ShipFlights, [&]() { flyShips(); });
      timePhase(UniverseTimingPhase::ShipArrivals, [&]() { arriveShips(); });
      timePhase(UniverseTimingPhase::Chat, [&]() { processChat(); });
      timePhase(UniverseTimingPhase::ClientContextUpdates, [&]() { sendClientContextUpdates(); });
      timePhase(UniverseTimingPhase::CelestialRequests, [&]() { respondToCelestialRequests(); });
      timePhase(UniverseTimingPhase::BrokenWorlds, [&]() { clearBrokenWorlds(); });
      timePhase(UniverseTimingPhase::WorldMessages, [&]() { handleWorldMessages(); });
      timePhase(UniverseTimingPhase::InactiveWorlds, [&]() { shutdownInactiveWorlds(); });
      timePhase(UniverseTimingPhase::PersistenceCompletions, [&]() { processPendingPersistenceWrites(); });
      timePhase(UniverseTimingPhase::TriggeredStorage, [&]() { doTriggeredStorage(); });
    } catch (std::exception const& e) {
      Logger::error("UniverseServer: exception caught: {}", outputException(e, true));
    }
    recordUniverseTiming(UniverseTimingPhase::Loop, Time::monotonicMicroseconds() - loopStart);

    Thread::sleep(mainWakeupInterval);
  }

  Logger::info("UniverseServer: Stopping UniverseServer");

  try {
    m_workerPool.stop();

    if (tcpServer) {
      Logger::info("UniverseServer: Stopping TCP Server");
      tcpServer.reset();
    }

    ReadLocker clientsLocker(m_clientsLock);
    auto clients = m_clients.keys();
    clientsLocker.unlock();
    for (auto clientId : clients)
      doDisconnection(clientId, "ServerShutdown");

    finishPendingPersistenceWrites();

    RecursiveMutexLocker locker(m_mainLock);
    auto leftoverWorlds = take(m_worlds);
    saveSettings();
    saveTempWorldIndex();
    locker.unlock();
  } catch (std::exception const& e) {
    Logger::error("UniverseServer: exception caught cleaning up: {}", outputException(e, true));
  }
}

#ifdef STAR_PLATFORM_N3DS
void UniverseServer::n3dsUpdate() {
  try {
    updateLua();
    processUniverseFlags();
    removeTimedBan();
    sendPendingChat();
    updateTeams();
    updateShips();
    sendClockUpdates();
    kickErroredPlayers();
    reapConnections();
    if (m_connectionServer)
      m_connectionServer->update();
    processPendingConnections();
    processPlanetTypeChanges();
    warpPlayers();
#ifdef STAR_PLATFORM_N3DS
    List<WorldServerThreadPtr> worldThreads;
    {
      RecursiveMutexLocker locker(m_mainLock);
      for (auto const& worldId : m_worlds.keys()) {
        if (auto worldThread = getWorld(worldId))
          worldThreads.append(worldThread);
      }
    }
    for (auto const& worldThread : worldThreads)
      worldThread->n3dsUpdate();
#endif
    flyShips();
    arriveShips();
    processChat();
    sendClientContextUpdates();
    respondToCelestialRequests();
    clearBrokenWorlds();
    handleWorldMessages();
    shutdownInactiveWorlds();
    processPendingPersistenceWrites();
    doTriggeredStorage();
  } catch (std::exception const& e) {
    Logger::error("N3DS UniverseServer: exception caught: {}", outputException(e, true));
  }
}
#endif

void UniverseServer::processUniverseFlags() {
  RecursiveMutexLocker locker(m_mainLock);
  ReadLocker clientsLocker(m_clientsLock);

  if (auto actions = m_universeSettings->pullPendingFlagActions()) {
    for (auto action : *actions) {
      if (action.is<PlaceDungeonFlagAction>()) {
        auto placeDungeonAction = action.get<PlaceDungeonFlagAction>();
        if (instanceWorldStoredOrActive(placeDungeonAction.targetInstance)) {
          auto worldId = InstanceWorldId(placeDungeonAction.targetInstance);
          m_pendingFlagActions.append(pair<WorldId, UniverseFlagAction>{worldId, placeDungeonAction});
        }
      }
    }
  }

  eraseWhere(m_pendingFlagActions, [&](pair<WorldId, UniverseFlagAction> const& p) {
    if (p.first.is<InstanceWorldId>() && instanceWorldStoredOrActive(p.first.get<InstanceWorldId>())) {
      // world is stored or active; perform flag actions once it loads
      if (auto maybeTargetWorld = triggerWorldCreation(p.first)) {
        if (auto targetWorld = maybeTargetWorld.value()) {
          if (p.second.is<PlaceDungeonFlagAction>()) {
            auto placeDungeonAction = p.second.get<PlaceDungeonFlagAction>();
            locker.unlock();
            targetWorld->placeDungeon(placeDungeonAction.dungeonId, placeDungeonAction.targetPosition, 0);
            locker.lock();
          }
          return true;
        }
      }
      return false;
    } else {
      // world hasn't yet been created; flag actions will be handled by normal creation
      return true;
    }
  });
}

void UniverseServer::sendPendingChat() {
  RecursiveMutexLocker locker(m_mainLock);
  ReadLocker clientsLocker(m_clientsLock);
  for (auto const& p : m_clients) {
    auto messages = m_chatProcessor->pullPendingMessages(p.first);
    if (!messages.empty()) {
      List<PacketPtr> chatPackets;
      chatPackets.reserve(messages.size());
      for (auto const& message : messages)
        chatPackets.append(make_shared<ChatReceivePacket>(message));
      m_connectionServer->sendPackets(p.first, std::move(chatPackets));
    }
  }
}

void UniverseServer::updateTeams() {
  RecursiveMutexLocker locker(m_mainLock);
  ReadLocker clientsLocker(m_clientsLock);

  StringMap<List<Uuid>> connectedPlayers;
  auto teams = m_teamManager->getPvpTeams();
  for (auto const& p : m_clients) {
    connectedPlayers[p.second->playerName()].append(p.second->playerUuid());

    if (p.second->team().type == TeamType::PVP)
      p.second->setTeam(EntityDamageTeam(TeamType::PVP, teams.value(p.second->playerUuid(), soloPvpTeam(p.second->clientId()))));
    else
      p.second->setTeam(EntityDamageTeam(TeamType::Friendly));

    auto channels = m_chatProcessor->clientChannels(p.first);
    auto team = m_teamManager->getTeam(p.second->playerUuid());
    for (auto const& channel : channels) {
      if (channel != printWorldId(p.second->playerWorldId()) && (!team || channel != team.value().hex()))
        m_chatProcessor->leaveChannel(p.first, channel);
    }
    if (team && !channels.contains(team.value().hex()))
      m_chatProcessor->joinChannel(p.first, team.value().hex());
  }

  m_teamManager->setConnectedPlayers(connectedPlayers);
}

void UniverseServer::updateShips() {
  RecursiveMutexLocker locker(m_mainLock);
  ReadLocker clientsLocker(m_clientsLock);

  for (auto const& p : m_clients) {
    auto newShipUpgrades = p.second->shipUpgrades();
    if (auto shipWorld = getWorld(ClientShipWorldId(p.second->playerUuid()))) {
      locker.unlock();
      auto result = shipWorld->applyShipUpgrades(p.second->shipSpecies(), newShipUpgrades, m_speciesShips, p.second->shipChunks());
      locker.lock();
      p.second->setShipSpecies(result.species);
      newShipUpgrades = result.shipUpgrades;
      if (result.shipChunkUpdate) {
        p.second->setShipUpgrades(newShipUpgrades);
        p.second->applyShipChunksUpdate(std::move(*result.shipChunkUpdate));
      }
    }

    if (auto systemWorld = p.second->systemWorld()) {
      float speed = newShipUpgrades.shipSpeed;
      systemWorld->executeClientShipAction(p.first, [speed](SystemClientShip* ship) {
        if (ship)
          ship->setSpeed(speed);
      });
    }
  }
}

void UniverseServer::sendClockUpdates() {
  RecursiveMutexLocker locker(m_mainLock);
  ReadLocker clientsLocker(m_clientsLock);

  int64_t currentTime = Time::monotonicMilliseconds();
#ifdef STAR_PLATFORM_N3DS
  int clockUpdatePacketInterval = 10000;
#else
  int clockUpdatePacketInterval = Root::singleton().assets()->json("/universe_server.config:clockUpdatePacketInterval").toInt();
#endif
  if (currentTime > m_lastClockUpdateSent + clockUpdatePacketInterval) {
    auto timePacket = make_shared<UniverseTimeUpdatePacket>(m_universeClock->time());
    for (auto clientId : m_clients.keys())
      m_connectionServer->sendPackets(clientId, {timePacket});
    m_lastClockUpdateSent = currentTime;
  }
}

void UniverseServer::sendClientContextUpdate(ServerClientContextPtr clientContext) {
  auto clientContextData = clientContext->writeUpdate();
  if (!clientContextData.empty())
    m_connectionServer->sendPackets(clientContext->clientId(), {make_shared<ClientContextUpdatePacket>(clientContextData)});
}

void UniverseServer::sendClientContextUpdates() {
  RecursiveMutexLocker locker(m_mainLock);
  ReadLocker clientsLocker(m_clientsLock);

  HashMap<ConnectionId, ByteArray> contextUpdates;
  for (auto const& p : m_clients) {
    auto clientContextData = p.second->writeUpdate();
    if (!clientContextData.empty())
      contextUpdates[p.first] = std::move(clientContextData);
  }

  for (auto& update : contextUpdates)
    m_connectionServer->sendPackets(update.first, {make_shared<ClientContextUpdatePacket>(std::move(update.second))});
}

void UniverseServer::kickErroredPlayers() {
  RecursiveMutexLocker locker(m_mainLock);
  for (auto const& worldId : m_worlds.keys()) {
    if (auto world = getWorld(worldId)) {
      locker.unlock();
      auto erroredClients = world->erroredClients();
      locker.lock();
      for (auto clientId : erroredClients)
        m_pendingDisconnections[clientId] = "Incoming client packet has caused exception";
    }
  }
}

void UniverseServer::reapConnections() {
  int64_t startTime = Time::monotonicMilliseconds();
#ifdef STAR_PLATFORM_N3DS
  int64_t timeout = 10000;
#else
  int64_t timeout = Root::singleton().assets()->json("/universe_server.config:connectionTimeout").toInt();
#endif
  {
    RecursiveMutexLocker acceptThreadsLocker(m_connectionAcceptThreadsMutex);
    eraseWhere(m_connectionAcceptThreads, [&](ThreadFunction<void>& function) {
      if (!function.isRunning()) {
        try {
          function.finish();
        } catch (std::exception const& e) {
          Logger::error("UniverseServer: Exception caught accepting new connection: {}", outputException(e, true));
        }
      }
      return function.isFinished();
    });
  }

  RecursiveMutexLocker locker(m_mainLock);
  auto pendingConnections = take(m_pendingDisconnections);
  locker.unlock();
  for (auto p : pendingConnections)
    doDisconnection(p.first, p.second);

  ReadLocker clientsLocker(m_clientsLock);
  auto clients = m_clients.keys();
  for (auto clientId : clients) {
    auto clientContext = m_clients.value(clientId);
    if (!m_connectionServer->connectionIsOpen(clientId)) {
      Logger::info("UniverseServer: Client {} connection lost", clientContext->descriptiveName());
      clientsLocker.unlock();
      doDisconnection(clientId, String("Disconnected due to connection lost"));
      clientsLocker.lock();
    } else {
      if (clientContext->remoteAddress() && startTime - m_connectionServer->lastActivityTime(clientId) > timeout) {
        Logger::info("UniverseServer: Kicking client {} due to inactivity", clientContext->descriptiveName());
        clientsLocker.unlock();
        doDisconnection(clientId, String("Disconnected due to inactivity"));
        clientsLocker.lock();
      }
    }
  }

  locker.lock();
  // Once connections are waiting to close, send any pending data and wait up
  // to the connection timeout for the client to do the closing to ensure the
  // client has all the data.
  size_t previousDeadConnections = m_deadConnections.size();
  m_deadConnections.filter([startTime, timeout](auto& pair) {
    if (pair.first.send())
      pair.second = startTime;
    return pair.first.isOpen() && startTime - pair.second < timeout;
  });
  if (previousDeadConnections > m_deadConnections.size())
    Logger::info("UniverseServer: Reaped {} dead connections", previousDeadConnections);
}

void UniverseServer::processPlanetTypeChanges() {
  RecursiveMutexLocker locker(m_mainLock);

  for (auto const& worldId : m_worlds.keys()) {
    if (auto celestialWorldId = worldId.ptr<CelestialWorldId>()) {
      if (auto world = getWorld(worldId)) {
        locker.unlock();
        auto newPlanetType = world->pullNewPlanetType();
        locker.lock();
        if (newPlanetType)
          updatePlanetType(*celestialWorldId, newPlanetType->first, newPlanetType->second);
      }
    }
  }
}

void UniverseServer::warpPlayers() {
  RecursiveMutexLocker locker(m_mainLock);
  ReadLocker clientsLocker(m_clientsLock);

  for (auto const& clientId : m_pendingPlayerWarps.keys()) {
    auto& warp = m_pendingPlayerWarps.get(clientId);
    WarpAction warpAction = warp.first;
    bool deploy = warp.second;

    auto clientContext = m_clients.value(clientId);
    if (!clientContext)
      continue;

    if (auto toPlayerUuid = warpAction.ptr<WarpToPlayer>()) {
      bool authorized = clientContext->isAdmin() || !m_secureWarps;
      if (!authorized) {
        auto requesterTeam = m_teamManager->getTeam(clientContext->playerUuid());
        auto targetTeam = m_teamManager->getTeam(*toPlayerUuid);
        if (requesterTeam && targetTeam && *requesterTeam == *targetTeam)
          authorized = true;
      }
      if (!authorized) {
        Logger::warn("UniverseServer: Denied WarpToPlayer from client {} ({}) to UUID {}",
                     clientId, clientContext->descriptiveName(), toPlayerUuid->hex());
        m_connectionServer->sendPackets(clientId, {make_shared<PlayerWarpResultPacket>(false, warpAction, true)});
        m_pendingPlayerWarps.remove(clientId);
        continue;
      }
    }

    WarpToWorld warpToWorld = resolveWarpAction(warpAction, clientId, deploy);

    if (auto maybeToWorld = triggerWorldCreation(warpToWorld.world)) {
      Logger::info("UniverseServer: Warping player {} to {}", clientId, printWarpAction(warpToWorld));
      if (auto toWorld = maybeToWorld.value()) {
        locker.unlock();
        if (toWorld->spawnTargetValid(warpToWorld.target)) {
          if (auto currentWorld = clientContext->playerWorld()) {
            if (auto playerRevivePosition = currentWorld->playerRevivePosition(clientId))
              clientContext->setPlayerReturnWarp(WarpToWorld{currentWorld->worldId(), SpawnTargetPosition(*playerRevivePosition)});
            clientContext->clearPlayerWorld();
            m_connectionServer->sendPackets(clientId, currentWorld->removeClient(clientId));
            m_chatProcessor->leaveChannel(clientId, printWorldId(currentWorld->worldId()));
          }
          clientContext->setOrbitWarpAction({});

          // having stale world ids in the client context is bad,
          // make sure it's at least null until the next client context update
          sendClientContextUpdate(clientContext);

          // Checking the spawn target validity then adding the client is not
          // perfect, it can still become invalid in between, if we fail at
          // adding the client we need to warp them back.
          bool clientAdded = toWorld && toWorld->addClient(clientId, warpToWorld.target, !clientContext->remoteAddress(), clientContext->canBecomeAdmin(), clientContext->netRules());
#ifdef STAR_PLATFORM_N3DS
          Logger::info("N3DS UniverseServer: addClient to world {}", clientAdded ? "succeeded" : "failed");
#endif

          locker.lock();
          if (clientAdded) {
            clientContext->setPlayerWorld(toWorld);
            m_chatProcessor->joinChannel(clientId, printWorldId(warpToWorld.world));

            if (warpToWorld.world.is<ClientShipWorldId>()) {
              if (auto clientId = getClientForUuid(warpToWorld.world.get<ClientShipWorldId>())) {
                if (auto systemWorld = m_clients.get(*clientId)->systemWorld())
                  clientContext->setOrbitWarpAction(systemWorld->clientWarpAction(*clientId));
              }
            }
          } else if (auto returnWarp = clientContext->playerReturnWarp()) {
            Logger::info("UniverseServer: Warping player {} failed, returning to '{}'", clientId, printWarpAction(returnWarp));
            m_pendingPlayerWarps[clientId] = {returnWarp, false};
          } else {
            Logger::info("UniverseServer: Warping player {} failed, returning to ship", clientId);
            m_pendingPlayerWarps[clientId] = {WarpAlias::OwnShip, false};
          }
          m_connectionServer->sendPackets(clientId, {make_shared<PlayerWarpResultPacket>(true, warpAction, false)});
          m_pendingPlayerWarps.remove(clientId);
        } else {
          Logger::info("UniverseServer: Warping player {} failed, invalid spawn target '{}'", clientId, printSpawnTarget(warpToWorld.target));
          locker.lock();
          m_connectionServer->sendPackets(clientId, {make_shared<PlayerWarpResultPacket>(false, warpAction, true)});
          m_pendingPlayerWarps.remove(clientId);
        }
      } else {
        Logger::info("UniverseServer: Warping player {} failed, invalid world '{}' or world failed to load", clientId, printWorldId(warpToWorld.world));
        m_connectionServer->sendPackets(clientId, {make_shared<PlayerWarpResultPacket>(false, warpAction, false)});
        m_pendingPlayerWarps.remove(clientId);
      }
    } else {
      // If the world is not created yet, just set a new warp again to wait for
      // it to create.
#ifdef STAR_PLATFORM_N3DS
      Logger::info("N3DS UniverseServer: waiting for world creation before warp");
#endif
      m_pendingPlayerWarps[clientId] = {warpAction, deploy};
    }
  }
}

void UniverseServer::flyShips() {
  RecursiveMutexLocker locker(m_mainLock);
  ReadLocker clientsLocker(m_clientsLock);

#ifdef STAR_PLATFORM_N3DS
  double queuedFlightWaitTime = 0.0;
#else
  double queuedFlightWaitTime = Root::singleton().assets()->json("/universe_server.config:queuedFlightWaitTime").toDouble();
#endif
  for (auto clientId : m_queuedFlights.keys()) {
    if (!m_pendingFlights.contains(clientId) && !m_pendingArrivals.contains(clientId)) {
      auto& flight = m_queuedFlights.get(clientId);
      if (flight.second.isNothing())
        flight.second = m_universeClock->time() + queuedFlightWaitTime;
      else if (m_universeClock->time() > *flight.second)
        m_pendingFlights.set(clientId, flight.first);

      if (m_pendingFlights.contains(clientId))
        m_queuedFlights.remove(clientId);
    }
  }

  eraseWhere(m_pendingFlights, [this, &locker](pair<ConnectionId const, tuple<Vec3I, SystemLocation, Json>> const& p) {
    ConnectionId clientId = p.first;
    Vec3I system = get<0>(p.second);
    SystemLocation location = get<1>(p.second);
    Json settings = get<2>(p.second);

    auto clientContext = m_clients.value(clientId);
    if (!clientContext)
      return true;

    auto clientSystem = clientContext->systemWorld();
    if (!clientSystem)
      system = Vec3I();

    if (system != Vec3I() && clientContext->shipCoordinate().location() == system && clientContext->shipLocation() == location)
      return true;

    // if the ship is flying to another system do nothing
    // if the ship is flying within the target system, just update the ship destination
    if (m_pendingArrivals.contains(clientId)) {
      return true;
    }

    auto maybeClientShip = triggerWorldCreation(ClientShipWorldId(clientContext->playerUuid()));
    if (!maybeClientShip)
      return false;// ship is not loaded yet
    auto clientShip = *maybeClientShip;
    if (!clientShip)
      return true;// ship is broken

    CelestialCoordinate destination = location.maybe<CelestialCoordinate>().value(CelestialCoordinate(system));
    bool interstellar = clientSystem ? clientContext->shipCoordinate().location() != system : true;
    if (!interstellar) {
      // don't fly to null locations in the same system
      if (!location)
        return true;

      clientSystem->setClientDestination(clientId, location);
    } else if (system != Vec3I()) {
      // changing systems
      clientSystem->removeClient(clientId);
      clientContext->setSystemWorld({});

      if (location)
        m_queuedFlights.set(clientId, {make_tuple(system, location, settings), {}});

      destination = CelestialCoordinate(system);
    }

    if (destination.isNull())
      Logger::info("Flying ship for player {} to new starter world", clientId);
    else
      Logger::info("Flying ship for player {} to {}", clientId, destination);

    bool startInWarp = system == Vec3I();
    locker.unlock();
    clientShip->startFlyingSky(interstellar, startInWarp, settings);

    auto clients = clientShip->clients();
    locker.lock();

    clientContext->setShipCoordinate(CelestialCoordinate(system));
    clientContext->setOrbitWarpAction({});
    for (auto clientId : clients) {
      if (auto& clientContext = m_clients.get(clientId))
        clientContext->setOrbitWarpAction({});
    }

    m_pendingArrivals.set(clientId, destination);

    return true;
  });
}

void UniverseServer::arriveShips() {
  RecursiveMutexLocker locker(m_mainLock);
  ReadLocker clientsLocker(m_clientsLock);

  eraseWhere(m_pendingArrivals, [this, &locker](pair<ConnectionId const, CelestialCoordinate>& p) {
    auto& clientId = p.first;
    auto& coordinate = p.second;

    if (!coordinate)
      coordinate = nextStarterWorld().value();

    if (!coordinate)
      return false;

    auto clientContext = m_clients.value(clientId);
    if (!clientContext)
      return true;

    auto clientSystem = clientContext->systemWorld();
    if (!clientSystem) {
      clientSystem = createSystemWorld(coordinate.location());
      if (coordinate.isSystem())
        clientSystem->addClient(clientId, clientContext->playerUuid(), clientContext->shipUpgrades().shipSpeed, {});
      else
        clientSystem->addClient(clientId, clientContext->playerUuid(), clientContext->shipUpgrades().shipSpeed, coordinate);

      clientContext->setSystemWorld(clientSystem);
    }

    auto location = clientSystem->clientShipLocation(clientId);
    if (!location)
      return false;

    if (!coordinate.isSystem() && !triggerWorldCreation(CelestialWorldId(coordinate)))
      return false;

    Logger::info("UniverseServer: Arriving ship for player {} at {}", clientId, coordinate);

    // world is loaded, ship has arrived
    clientContext->setShipCoordinate(coordinate);
    clientContext->setShipLocation(location);

    if (auto clientShip = createWorld(ClientShipWorldId(clientContext->playerUuid()))) {
      auto skyParameters = clientSystem->clientSkyParameters(clientId);
      locker.unlock();
      clientShip->stopFlyingSkyAt(skyParameters);
      auto clients = clientShip->clients();
      locker.lock();

      for (auto shipClientId : clients) {
        if (auto& clientContext = m_clients.get(shipClientId))
          clientContext->setOrbitWarpAction(clientSystem->clientWarpAction(clientId));
      }
    }
    return true;
  });
}

void UniverseServer::respondToCelestialRequests() {
  RecursiveMutexLocker locker(m_mainLock);
  ReadLocker clientsLocker(m_clientsLock);

  for (auto& p : m_pendingCelestialRequests) {
    List<CelestialResponse> responses;
    eraseWhere(p.second, [&responses](WorkerPoolPromise<CelestialResponse> const& request) {
      if (request.poll()) {
        responses.append(request.get());
        return true;
      }
      return false;
    });
    if (m_clients.contains(p.first))
      m_connectionServer->sendPackets(p.first, {make_shared<CelestialResponsePacket>(std::move(responses))});
  }
  eraseWhere(m_pendingCelestialRequests, [](auto const& p) {
    return p.second.empty();
  });
}

void UniverseServer::processChat() {
  RecursiveMutexLocker locker(m_mainLock);
  ReadLocker clientsLocker(m_clientsLock);

  for (auto const& p : take(m_pendingChat)) {
    if (auto clientContext = m_clients.get(p.first)) {
      for (auto const& chat : p.second) {
        auto& message = get<0>(chat);
        auto sendMode = get<1>(chat);
        auto& data = get<2>(chat);
        if (clientContext->remoteAddress())
          Logger::info("Chat: <{}> {}", clientContext->playerName(), message);

        auto team = m_teamManager->getTeam(clientContext->playerUuid());
        locker.unlock();
        if (sendMode == ChatSendMode::Broadcast)
          m_chatProcessor->broadcast(p.first, message, std::move(data));
        else if (sendMode == ChatSendMode::Party && team.isValid())
          m_chatProcessor->message(p.first, MessageContext::Mode::Party, team.value().hex(), message, std::move(data));
        else
          m_chatProcessor->message(p.first, MessageContext::Mode::Local, printWorldId(clientContext->playerWorldId()), message, std::move(data));
        locker.lock();
      }
    }
  }
}

void UniverseServer::clearBrokenWorlds() {
  RecursiveMutexLocker locker(m_mainLock);

  if (Time::monotonicMilliseconds() >= m_clearBrokenWorldsDeadline) {
    // Clear out all broken worlds
    eraseWhere(m_worlds, [](auto const& p) {
      if (!p.second.isValid()) {
        Logger::info("UniverseServer: Clearing broken world {}", p.first);
        return true;
      } else {
        return false;
      }
    });

  #ifdef STAR_PLATFORM_N3DS
    int clearBrokenWorldsInterval = 30000;
  #else
    int clearBrokenWorldsInterval = Root::singleton().assets()->json("/universe_server.config:clearBrokenWorldsInterval").toInt();
  #endif
    m_clearBrokenWorldsDeadline = Time::monotonicMilliseconds() + clearBrokenWorldsInterval;
  }
}

void UniverseServer::handleWorldMessages() {
  RecursiveMutexLocker locker(m_mainLock);
  ReadLocker clientsLocker(m_clientsLock);

  auto it = m_pendingWorldMessages.begin();
  while (it != m_pendingWorldMessages.end()) {
    auto& worldId = it->first;
    if (auto worldResult = triggerWorldCreation(worldId)) {
      auto& world = *worldResult;

      if (world) {
        if (world->isRunning()) {
          world->passMessages(std::move(it->second));
          it = m_pendingWorldMessages.erase(it);
        }
      } else {
        for (auto& message : it->second)
          message.promise.fail("Error creating world");
        it = m_pendingWorldMessages.erase(it);
      }
    } else
      ++it;
  }
}

void UniverseServer::shutdownInactiveWorlds() {
  RecursiveMutexLocker locker(m_mainLock);
  ReadLocker clientsLocker(m_clientsLock);

  // Shutdown idle and errored worlds.
  for (auto const& worldId : m_worlds.keys()) {
    if (auto world = getWorld(worldId)) {
      bool stoppedWorld = false;
      clientsLocker.unlock();
      locker.unlock();
      if (world->serverErrorOccurred()) {
        world->stop();
        stoppedWorld = true;
        Logger::error("UniverseServer: World {} has stopped due to an error", worldId);
        worldDiedWithError(world->worldId());
      } else if (world->noClients()) {
        bool anyPendingWarps = false;
        for (auto const& p : m_pendingPlayerWarps) {
          if (resolveWarpAction(p.second.first, p.first, p.second.second).world == world->worldId()) {
            anyPendingWarps = true;
            break;
          }
        }

        if (!anyPendingWarps && world->shouldExpire()) {
          Logger::info("UniverseServer: Stopping idle world {}", worldId);
          world->stop();
          stoppedWorld = true;
        }
      }
      locker.lock();
      clientsLocker.lock();
#ifdef STAR_PLATFORM_N3DS
      if (stoppedWorld) {
#else
      if (world->isJoined()) {
#endif
        auto kickClients = world->clients();
        if (!kickClients.empty()) {
          Logger::info("UniverseServer: World {} shutdown, kicking {} players to their own ships", worldId, world->clients().size());
          for (auto clientId : world->clients())
            clientWarpPlayer(clientId, WarpAlias::OwnShip);
        }

        if (worldId.is<ClientShipWorldId>()) {
          world->unloadAll(true);
          if (auto clientId = getClientForUuid(worldId.get<ClientShipWorldId>())) {
            auto clientContext = m_clients.get(*clientId);
            clientContext->applyShipChunksUpdate(world->readChunkUpdate(clientContext->shipChunks()));
          }
        }

        m_worlds.remove(worldId);
        // Once a world is shutdown, mark its shutdown time in m_tempWorldIndex
        if (auto instanceWorldId = worldId.maybe<InstanceWorldId>()) {
          if (m_tempWorldIndex.contains(*instanceWorldId))
            m_tempWorldIndex[*instanceWorldId].first = m_universeClock->milliseconds();
        }
      }
      clientsLocker.unlock();
    }
  }

  // Clear out all temporary worlds shut down more than tempWorldDeleteTime time ago.
  // Keep around worlds that are currently running or are active in system worlds
  Set<InstanceWorldId> systemLocationWorlds;
  for (auto p : m_systemWorlds) {
    for (auto instanceWorldId : p.second->activeInstanceWorlds()) {
      if (m_tempWorldIndex.contains(instanceWorldId))
        systemLocationWorlds.add(instanceWorldId);
    }
  }
  eraseWhere(m_tempWorldIndex, [this, systemLocationWorlds](pair<InstanceWorldId, pair<uint64_t, uint64_t>> const& p) {
    String storageFile = tempWorldFile(p.first);
    if (!m_worlds.contains(WorldId(p.first)) && !systemLocationWorlds.contains(p.first) && m_universeClock->milliseconds() > int64_t(p.second.first + p.second.second)) {
      Logger::info("UniverseServer: Expiring temporary world {}", printWorldId(p.first));
      if (File::isFile(storageFile))
        File::remove(storageFile);
      return true;
    }
    return false;
  });

  // Clear out empty system worlds
  eraseWhere(m_systemWorlds, [](pair<Vec3I, SystemWorldServerThreadPtr> w) {
    return w.second->clients().empty();
  });
}

void UniverseServer::doTriggeredStorage() {
#ifdef STAR_PLATFORM_N3DS
  return;
#endif

  RecursiveMutexLocker locker(m_mainLock);
  ReadLocker clientsLocker(m_clientsLock);

  if (Time::monotonicMilliseconds() >= m_storageTriggerDeadline) {
    Logger::debug("UniverseServer: periodic sync to disk");

    clientsLocker.unlock();
    locker.unlock();
    persistVersionedJsonStorageSnapshots(buildTriggeredStorageSnapshots());
    cleanupAndCommitCelestialDatabase();

    locker.lock();
  #ifdef STAR_PLATFORM_N3DS
    int storageTriggerInterval = 60000;
  #else
    int storageTriggerInterval = Root::singleton().assets()->json("/universe_server.config:universeStorageInterval").toInt();
  #endif
    m_storageTriggerDeadline = Time::monotonicMilliseconds() + storageTriggerInterval;
  }
}

UniverseServer::VersionedJsonStorageSnapshot UniverseServer::buildUniverseSettingsStorageSnapshot() {
  auto versioningDatabase = Root::singleton().versioningDatabase();

  RecursiveMutexLocker locker(m_mainLock);
  return {"UniverseSettings",
      File::relativeTo(m_storageDirectory, "universe.dat"),
      versioningDatabase->makeCurrentVersionedJson("UniverseSettings", m_universeSettings->toJson().set("time", m_universeClock->time()))};
}

UniverseServer::VersionedJsonStorageSnapshot UniverseServer::buildTempWorldIndexStorageSnapshot() {
  auto versioningDatabase = Root::singleton().versioningDatabase();
  JsonObject worldIndex = JsonObject();

  RecursiveMutexLocker locker(m_mainLock);
  for (auto p : m_tempWorldIndex)
    worldIndex.set(printWorldId(p.first), JsonArray{p.second.first, p.second.second});

  return {"TempWorldIndex",
      File::relativeTo(m_storageDirectory, "tempworlds.index"),
      versioningDatabase->makeCurrentVersionedJson("TempWorldIndex", worldIndex)};
}

UniverseServer::VersionedJsonStorageSnapshot UniverseServer::buildClientContextStorageSnapshot(ServerClientContextPtr const& clientContext) {
  auto versioningDatabase = Root::singleton().versioningDatabase();
  String clientContextFile = File::relativeTo(m_storageDirectory, strf("{}.clientcontext", clientContext->playerUuid().hex()));
  return {"ClientContext", clientContextFile, versioningDatabase->makeCurrentVersionedJson("ClientContext", clientContext->storeServerData())};
}

List<UniverseServer::VersionedJsonStorageSnapshot> UniverseServer::buildClientContextStorageSnapshots() {
  List<VersionedJsonStorageSnapshot> snapshots;

  ReadLocker clientsLocker(m_clientsLock);
  auto clients = m_clients.values();
  clientsLocker.unlock();

  for (auto const& clientContext : clients) {
    RecursiveMutexLocker locker(m_mainLock);
    auto shipWorld = getWorld(ClientShipWorldId(clientContext->playerUuid()));
    locker.unlock();

    if (shipWorld) {
      clientContext->applyShipChunksUpdate(shipWorld->readChunkUpdate(clientContext->shipChunks()));
    }

    snapshots.append(buildClientContextStorageSnapshot(clientContext));
  }

  return snapshots;
}

List<UniverseServer::VersionedJsonStorageSnapshot> UniverseServer::buildTriggeredStorageSnapshots() {
  auto snapshotStart = Time::monotonicMicroseconds();
  List<VersionedJsonStorageSnapshot> snapshots;

  snapshots.append(buildUniverseSettingsStorageSnapshot());
  snapshots.append(buildTempWorldIndexStorageSnapshot());
  snapshots.appendAll(buildClientContextStorageSnapshots());

  RecursiveMutexLocker locker(m_mainLock);
  m_persistenceSnapshotBuildTimeMicroseconds += Time::monotonicMicroseconds() - snapshotStart;
  return snapshots;
}

List<UniverseServer::PersistenceWriteResult> UniverseServer::writeVersionedJsonStorageSnapshotsNow(List<VersionedJsonStorageSnapshot> snapshots, unsigned maxRetries) {
  List<PersistenceWriteResult> results;
  for (auto& snapshot : snapshots) {
    auto writeStart = Time::monotonicMicroseconds();
    uint64_t retryCount = 0;
    try {
      while (true) {
        try {
          VersionedJson::writeFile(snapshot.store, snapshot.file);
          results.append({snapshot.jobType, snapshot.file, true, {}, Time::monotonicMicroseconds() - writeStart, retryCount});
          break;
        } catch (std::exception const&) {
          if (retryCount >= maxRetries)
            throw;

          retryCount++;
        }
      }
    } catch (std::exception const& e) {
      results.append({snapshot.jobType, snapshot.file, false, strf("{}", outputException(e, false)), Time::monotonicMicroseconds() - writeStart, retryCount});
    }
  }

  return results;
}

void UniverseServer::recordPersistenceWriteResults(List<PersistenceWriteResult> results) {
  uint64_t written = 0;
  uint64_t failures = 0;
  uint64_t writeTime = 0;
  uint64_t retries = 0;

  for (auto const& result : results) {
    writeTime += result.durationMicroseconds;
    retries += result.retryCount;
    if (result.success) {
      written++;
    } else {
      failures++;
      Logger::error("UniverseServer: Failed writing {} snapshot '{}' after {} retries: {}", result.jobType, result.file, result.retryCount, result.error);
    }
  }

  RecursiveMutexLocker locker(m_mainLock);
  m_persistenceSnapshotsWritten += written;
  m_persistenceFailures += failures;
  m_persistenceWriteTimeMicroseconds += writeTime;
  m_persistenceWriteRetries += retries;
}

void UniverseServer::writeVersionedJsonStorageSnapshots(List<VersionedJsonStorageSnapshot> snapshots) {
  recordPersistenceWriteResults(writeVersionedJsonStorageSnapshotsNow(std::move(snapshots), m_persistenceMaxWriteRetries));
}

void UniverseServer::persistVersionedJsonStorageSnapshots(List<VersionedJsonStorageSnapshot> snapshots) {
  if (snapshots.empty())
    return;

  if (!m_useAsyncPersistence) {
    writeVersionedJsonStorageSnapshots(std::move(snapshots));
    return;
  }

  processPendingPersistenceWrites();

  auto snapshotCount = snapshots.size();
  bool queueFull = false;
  {
    RecursiveMutexLocker locker(m_mainLock);
    if (m_persistenceMaxQueuedSnapshots != 0 && m_persistenceSnapshotsPending + snapshotCount > m_persistenceMaxQueuedSnapshots) {
      m_persistenceQueueFullFallbacks++;
      m_persistenceSynchronousFallbacks++;
      queueFull = true;
    } else {
      m_persistenceSnapshotsPending += snapshotCount;
    }
  }

  if (queueFull) {
    writeVersionedJsonStorageSnapshots(std::move(snapshots));
    return;
  }

  auto maxRetries = m_persistenceMaxWriteRetries;
  auto promise = m_persistenceWorkerPool.addProducer<List<PersistenceWriteResult>>([snapshots = std::move(snapshots), maxRetries]() mutable {
      return UniverseServer::writeVersionedJsonStorageSnapshotsNow(std::move(snapshots), maxRetries);
    });

  RecursiveMutexLocker locker(m_mainLock);
  m_pendingPersistenceWrites.append({std::move(promise), snapshotCount, Time::monotonicMilliseconds()});
}

void UniverseServer::processPendingPersistenceWrites() {
  size_t index = 0;
  while (true) {
    RecursiveMutexLocker locker(m_mainLock);
    while (index < m_pendingPersistenceWrites.size() && !m_pendingPersistenceWrites[index].promise.done())
      index++;

    if (index == m_pendingPersistenceWrites.size())
      return;

    auto pendingWrite = m_pendingPersistenceWrites.takeAt(index);
    m_persistenceSnapshotsPending -= min(m_persistenceSnapshotsPending, pendingWrite.snapshotCount);
    m_persistenceBatchesCompleted++;
    locker.unlock();

    recordPersistenceWriteResults(pendingWrite.promise.get());
  }
}

void UniverseServer::finishPendingPersistenceWrites() {
  while (true) {
    RecursiveMutexLocker locker(m_mainLock);
    if (m_pendingPersistenceWrites.empty())
      return;

    auto pendingWrite = m_pendingPersistenceWrites.takeAt(0);
    m_persistenceSnapshotsPending -= min(m_persistenceSnapshotsPending, pendingWrite.snapshotCount);
    m_persistenceBatchesCompleted++;
    locker.unlock();

    recordPersistenceWriteResults(pendingWrite.promise.get());
  }
}

void UniverseServer::cleanupAndCommitCelestialDatabase() {
  auto commitStart = Time::monotonicMicroseconds();
  m_celestialDatabase->cleanupAndCommit();

  RecursiveMutexLocker locker(m_mainLock);
  m_persistenceCelestialCommitTimeMicroseconds += Time::monotonicMicroseconds() - commitStart;
  m_persistenceCelestialCommits++;
}

void UniverseServer::saveSettings() {
  List<VersionedJsonStorageSnapshot> snapshots;
  snapshots.append(buildUniverseSettingsStorageSnapshot());
  writeVersionedJsonStorageSnapshots(std::move(snapshots));
}

void UniverseServer::loadSettings() {
  RecursiveMutexLocker locker(m_mainLock);

  auto loadDefaultSettings = [this]() {
    m_universeClock = make_shared<Clock>();
    m_universeSettings = make_shared<UniverseSettings>();
  };

  auto versioningDatabase = Root::singleton().versioningDatabase();
  auto storageFile = File::relativeTo(m_storageDirectory, "universe.dat");
  if (File::isFile(storageFile)) {
    try {
      auto settings = versioningDatabase->loadVersionedJson(VersionedJson::readFile(storageFile), "UniverseSettings");
      m_universeSettings = make_shared<UniverseSettings>(settings);
      m_universeClock = make_shared<Clock>();
      m_universeClock->setTime(settings.getDouble("time"));
    } catch (std::exception const& e) {
      Logger::error("UniverseServer: Could not load universe settings file, loading defaults {}", outputException(e, false));
      File::rename(storageFile, strf("{}.{}.fail", storageFile, Time::millisecondsSinceEpoch()));
      loadDefaultSettings();
    }
  } else {
    loadDefaultSettings();
  }

  m_universeClock->start();
}

Maybe<CelestialCoordinate> UniverseServer::nextStarterWorld() {
  RecursiveMutexLocker locker(m_mainLock);

#ifdef STAR_PLATFORM_N3DS
  return {};
#else
  auto assets = Root::singleton().assets();
  String defaultWorldCoordinate = assets->json("/universe_server.config:defaultWorldCoordinate").toString();
  if (!defaultWorldCoordinate.empty())
    return CelestialCoordinate(defaultWorldCoordinate);

  if (m_nextRandomizedStarterWorld && m_nextRandomizedStarterWorld->done()) {
    CelestialCoordinate nextWorld = m_nextRandomizedStarterWorld->get();
    m_nextRandomizedStarterWorld.reset();
    return nextWorld;
  }

  if (!m_nextRandomizedStarterWorld) {
    m_nextRandomizedStarterWorld = m_workerPool.addProducer<CelestialCoordinate>([assets, celestialDatabase = m_celestialDatabase]() {
      Logger::info("Searching for new randomized starter world");
      auto filterWorld = [celestialDatabase](CelestialCoordinate const& coordinate, Json const& filter) {
        auto parameters = celestialDatabase->parameters(coordinate);
        auto visitableParameters = parameters->visitableParameters();
        if (!visitableParameters)
          return false;

        if (auto biome = filter.optString("terrestrialBiome")) {
          auto terrestrialParameters = as<TerrestrialWorldParameters>(visitableParameters);
          if (!terrestrialParameters || *biome != terrestrialParameters->primaryBiome)
            return false;
        }

        if (auto size = filter.optString("terrestrialSize")) {
          auto terrestrialParameters = as<TerrestrialWorldParameters>(visitableParameters);
          if (!terrestrialParameters || *size != terrestrialParameters->sizeName)
            return false;
        }

        if (auto dungeon = filter.optString("floatingDungeon")) {
          auto dungeonParameters = as<FloatingDungeonWorldParameters>(visitableParameters);
          if (!dungeonParameters || *dungeon != dungeonParameters->primaryDungeon)
            return false;
        }

        return true;
      };

      auto findParameters = assets->json("/universe_server.config:findStarterWorldParameters");
      auto randomWorld = celestialDatabase->findRandomWorld(findParameters.getUInt("tries"), findParameters.getUInt("range"), [&](CelestialCoordinate const& coordinate) {
        if (!filterWorld(coordinate, findParameters.get("starterWorld")))
          return false;

        List<CelestialCoordinate> allChildren;
        for (auto const& planet : celestialDatabase->children(coordinate.system())) {
          allChildren.append(planet);
          for (auto const& satellite : celestialDatabase->children(planet))
            allChildren.append(satellite);
        }

        for (auto const& requiredSystemWorld : findParameters.getArray("requiredSystemWorlds", {})) {
          bool worldFound = false;
          for (auto const& world : allChildren) {
            if (filterWorld(world, requiredSystemWorld)) {
              worldFound = true;
              break;
            }
          }

          if (!worldFound)
            return false;
        }

        return true;
      });

      if (randomWorld)
        Logger::info("UniverseServer: Found randomized starter world at {}", *randomWorld);
      else
        Logger::error("UniverseServer: Could not find randomized starter world!");

      return randomWorld.value();
    });
  }

  return {};
#endif
}

void UniverseServer::loadTempWorldIndex() {
#ifdef STAR_PLATFORM_N3DS
  m_tempWorldIndex.clear();
  Logger::info("N3DS UniverseServer: skipped temp world index scan");
  return;
#endif

  auto versioningDatabase = Root::singleton().versioningDatabase();
  auto storageFile = File::relativeTo(m_storageDirectory, "tempworlds.index");
  if (File::isFile(storageFile)) {
    try {
      m_tempWorldIndex.clear();
      auto settings = versioningDatabase->loadVersionedJson(VersionedJson::readFile(storageFile), "TempWorldIndex");
      for (auto p : settings.iterateObject()) {
        WorldId worldId = parseWorldId(p.first);
        pair<uint64_t, uint64_t> deleteTime = {p.second.get(0).toUInt(), p.second.get(1).toUInt()};
        m_tempWorldIndex.insert(worldId.get<InstanceWorldId>(), deleteTime);
      }
    } catch (std::exception const& e) {
      Logger::error("UniverseServer: Could not load temp world index file", outputException(e, false));
      File::rename(storageFile, strf("{}.{}.fail", storageFile, Time::millisecondsSinceEpoch()));
    }
  }

  // delete temporary instance worlds not found in the index on load
  auto tempWorldFiles = m_tempWorldIndex.keys().transformed([this](InstanceWorldId const& worldId) { return tempWorldFile(worldId); });
  for (auto p : File::dirList(m_storageDirectory)) {
    if (p.second == false && p.first.endsWith(".tempworld")) {
      String storageFile = File::relativeTo(m_storageDirectory, p.first);
      if (!tempWorldFiles.contains(storageFile)) {
        Logger::info("UniverseServer: Removing unindexed temporary world {}", p.first);
        File::remove(storageFile);
      }
    }
  }
}

void UniverseServer::saveTempWorldIndex() {
  List<VersionedJsonStorageSnapshot> snapshots;
  snapshots.append(buildTempWorldIndexStorageSnapshot());
  writeVersionedJsonStorageSnapshots(std::move(snapshots));
}

String UniverseServer::tempWorldFile(InstanceWorldId const& worldId) const {
  String identifier = worldId.instance;
  if (worldId.uuid)
    identifier = strf("{}-{}", identifier, worldId.uuid->hex());
  if (worldId.level)
    identifier = strf("{}-{}", identifier, worldId.level.value());
  return File::relativeTo(m_storageDirectory, strf("{}.tempworld", identifier));
}

Maybe<String> UniverseServer::isBannedUser(Maybe<HostAddress> hostAddress, Uuid playerUuid) const {
  RecursiveMutexLocker locker(m_mainLock);
  auto config = Root::singleton().configuration();

  if (hostAddress) {
    for (auto const& ban : m_tempBans) {
      if (ban.ip) {
        if (*ban.ip == *hostAddress) {
          return ban.reason;
        }
      }
    }

    for (auto banEntry : config->get("bannedIPs").iterateArray()) {
      if (HostAddress(banEntry.getString("ip")) == *hostAddress) {
        return banEntry.getString("reason");
      }
    }
  }

  for (auto const& ban : m_tempBans) {
    if (ban.uuid) {
      if (*ban.uuid == playerUuid)
        return ban.reason;
    }
  }

  for (auto banEntry : config->get("bannedUuids").iterateArray()) {
    if (Uuid(banEntry.getString("uuid")) == playerUuid)
      return banEntry.getString("reason");
  }

  return {};
}

void UniverseServer::doTempBan(ConnectionId clientId, String const& reason, pair<bool, bool> banType, int timeout) {
  RecursiveMutexLocker locker(m_mainLock);
  ReadLocker clientsLocker(m_clientsLock);

  if (auto clientContext = m_clients.value(clientId)) {
    if (!clientContext->remoteAddress())
      return;

    auto banExpiry = Time::monotonicMilliseconds() + timeout * 1000;// current time is in millis, conversion factor
    Maybe<HostAddress> ip;
    if (banType.first)
      ip = clientContext->remoteAddress();

    Maybe<Uuid> uuid;

    if (banType.second)
      uuid = clientContext->playerUuid();

    m_tempBans.append({banExpiry, reason, ip, uuid});
  }
}

void UniverseServer::doPermBan(ConnectionId clientId, String const& reason, pair<bool, bool> banType) {
  RecursiveMutexLocker locker(m_mainLock);
  ReadLocker clientsLocker(m_clientsLock);

  if (auto clientContext = m_clients.value(clientId)) {
    if (!clientContext->remoteAddress())
      return;

    auto config = Root::singleton().configuration();
    if (banType.first) {
      auto bannedIPs = config->get("bannedIPs").toArray();

      bannedIPs.append(JsonObject{
        {"ip", toString(*clientContext->remoteAddress())},
        {"reason", reason},
      });

      config->set("bannedIPs", bannedIPs);
    }

    if (banType.second) {
      auto bannedUuids = config->get("bannedUuids").toArray();

      bannedUuids.append(JsonObject{
        {"uuid", clientContext->playerUuid().hex()},
        {"reason", reason},
      });

      config->set("bannedUuids", bannedUuids);
    }
  }
}

void UniverseServer::removeTimedBan() {
  RecursiveMutexLocker locker(m_mainLock);
  auto currentTime = Time::monotonicMilliseconds();
  eraseWhere(m_tempBans, [currentTime](TimeoutBan const& b) {
    return b.banExpiry <= currentTime;
  });
}

void UniverseServer::addCelestialRequests(ConnectionId clientId, List<CelestialRequest> requests) {
  RecursiveMutexLocker locker(m_mainLock);
  for (auto request : requests) {
    m_pendingCelestialRequests[clientId].append(m_workerPool.addProducer<CelestialResponse>([this, request]() {
      return m_celestialDatabase->respondToRequest(request);
    }));
  }
}

void UniverseServer::worldUpdated(WorldServerThread* server) {
  for (auto clientId : server->clients()) {
    auto packets = server->pullOutgoingPackets(clientId);
    m_connectionServer->sendPackets(clientId, std::move(packets));
  }
}

void UniverseServer::systemWorldUpdated(SystemWorldServerThread* systemWorldServer) {
  for (auto clientId : systemWorldServer->clients()) {
    auto packets = systemWorldServer->pullOutgoingPackets(clientId);
    m_connectionServer->sendPackets(clientId, std::move(packets));
  }
}

void UniverseServer::packetsReceived(UniverseConnectionServer*, ConnectionId clientId, List<PacketPtr> packets) {
  ReadLocker clientsLocker(m_clientsLock);
  if (auto clientContext = m_clients.value(clientId)) {
    clientsLocker.unlock();

    for (auto& packet : packets) {
      auto packetType = packet->type();

      if (auto warpAction = as<PlayerWarpPacket>(packet)) {
        auto const& action = warpAction->action;
        bool blocked = m_secureWarps;

        if (action.is<WarpAlias>() || action.is<WarpToPlayer>()) {
          blocked = false;
        } else if (auto warpToWorld = action.ptr<WarpToWorld>()) {
          if (warpToWorld->world.empty() || 
              warpToWorld->world.is<ClientShipWorldId>() || 
              warpToWorld->world.is<CelestialWorldId>() ||
              warpToWorld->world.is<InstanceWorldId>()) {
            blocked = false;
          }
        }

        if (blocked) {
          Logger::warn("UniverseServer: Blocked invalid warp action from client {}", clientId);
          m_connectionServer->sendPackets(clientId, {make_shared<PlayerWarpResultPacket>(false, action, true)});
          continue;
        }

        clientWarpPlayer(clientId, warpAction->action, warpAction->deploy);

      } else if (auto flyShip = as<FlyShipPacket>(packet)) {
        clientFlyShip(clientId, flyShip->system, flyShip->location, flyShip->settings);

      } else if (auto chatSend = as<ChatSendPacket>(packet)) {
        RecursiveMutexLocker locker(m_mainLock);
        m_pendingChat[clientId].append(make_tuple(std::move(chatSend->text), chatSend->sendMode, std::move(chatSend->data)));

      } else if (auto clientContextUpdatePacket = as<ClientContextUpdatePacket>(packet)) {
        clientContext->readUpdate(std::move(clientContextUpdatePacket->updateData));

      } else if (auto clientDisconnectPacket = as<ClientDisconnectRequestPacket>(packet)) {
        disconnectClient(clientId, String());

      } else if (auto celestialRequest = as<CelestialRequestPacket>(packet)) {
        addCelestialRequests(clientId, std::move(celestialRequest->requests));

      } else if (auto entityMessage = as<EntityMessagePacket>(packet)) {
        entityMessage->fromConnection = clientId;

        if (m_secureWarps && entityMessage->message == "warp") {
          bool blocked = false;

          if (entityMessage->args.size() < 1 || !entityMessage->args.get(0).canConvert(Json::Type::String)) {
            Logger::warn("UniverseServer: Blocked warp entity message with invalid args from client {}", clientId);
            blocked = true;
          } else {
            try {
              parseWarpAction(entityMessage->args.get(0).toString());
            } catch (StarException const&) {
              Logger::warn("UniverseServer: Blocked warp entity message with unparseable warp action from client {}", clientId);
              blocked = true;
            }
          }

          if (!blocked) {
            if (auto targetEntityId = entityMessage->entityId.ptr<EntityId>()) {
              auto targetConnection = connectionForEntity(*targetEntityId);
              if (targetConnection != clientId) {
                clientsLocker.lock();
                bool isAdmin = clientContext->isAdmin();
                clientsLocker.unlock();
                if (!isAdmin) {
                  Logger::warn("UniverseServer: Blocked warp entity message from non-admin client {} targeting entity owned by connection {}", clientId, targetConnection);
                  blocked = true;
                }
              }
            }
          }

          if (blocked) {
            m_connectionServer->sendPackets(clientId, {make_shared<EntityMessageResponsePacket>(makeLeft(String("Warp entity message blocked by server")), entityMessage->uuid)});
            continue;
          }
        }

        if (auto currentWorld = clientContext->playerWorld())
          currentWorld->pushIncomingPackets(clientId, {std::move(packet)});

      } else if (is<SystemObjectSpawnPacket>(packet)) {
        if (auto currentSystem = clientContext->systemWorld())
          currentSystem->pushIncomingPacket(clientId, std::move(packet));
      } else {
        if (auto currentWorld = clientContext->playerWorld())
          currentWorld->pushIncomingPackets(clientId, {std::move(packet)});
      }
    }
  }
}

void UniverseServer::enqueuePendingConnection(UniverseConnection connection, Maybe<HostAddress> remoteAddress) {
  RecursiveMutexLocker locker(m_mainLock);
  auto pendingConnection = make_shared<PendingConnection>(m_nextPendingConnectionId++, std::move(connection), std::move(remoteAddress));
  setPendingConnectionState(*pendingConnection, PendingConnectionState::AwaitProtocolRequest);
  m_pendingConnections.append(pendingConnection);
  m_pendingHandshakeAccepted++;
}

void UniverseServer::processPendingConnections() {
  RecursiveMutexLocker locker(m_mainLock);
  eraseWhere(m_pendingConnections, [this](shared_ptr<PendingConnection> const& pendingConnection) {
    advancePendingConnection(*pendingConnection);
    return pendingConnection->state == PendingConnectionState::Dead;
  });
}

String UniverseServer::pendingConnectionStateName(PendingConnectionState state) const {
  switch (state) {
    case PendingConnectionState::AwaitProtocolRequest:
      return "awaitProtocol";
    case PendingConnectionState::SendProtocolResponse:
      return "sendProtocol";
    case PendingConnectionState::AwaitClientConnect:
      return "awaitClient";
    case PendingConnectionState::AwaitHandshakeResponse:
      return "awaitPassword";
    case PendingConnectionState::FinalizeClient:
      return "finalize";
    case PendingConnectionState::RejectAndFlush:
      return "rejectFlush";
    case PendingConnectionState::Dead:
      return "dead";
  }
  return "unknown";
}

void UniverseServer::setPendingConnectionState(PendingConnection& pendingConnection, PendingConnectionState state) {
  pendingConnection.state = state;
  pendingConnection.stateDeadline = Time::monotonicMilliseconds() + m_pendingConnectionStateWaitLimit;
}

void UniverseServer::failPendingConnection(PendingConnection& pendingConnection, String message, bool timedOut) {
  if (timedOut)
    m_pendingHandshakeTimedOut++;
  m_pendingHandshakeRejected++;

  String playerName = pendingConnection.clientConnect ? pendingConnection.clientConnect->playerName : "<unknown>";
  Logger::warn("UniverseServer: Login attempt failed with account '{}' as player '{}' from address {}, error: {}",
      pendingConnection.accountString.empty() ? String("<unknown>") : pendingConnection.accountString,
      playerName,
      pendingConnection.remoteAddressString.empty() ? String("unknown") : pendingConnection.remoteAddressString,
      message);
  pendingConnection.failureReason = message;
  pendingConnection.connection.pushSingle(make_shared<ConnectFailurePacket>(std::move(message)));
  setPendingConnectionState(pendingConnection, PendingConnectionState::RejectAndFlush);
}

void UniverseServer::advancePendingConnection(PendingConnection& pendingConnection) {
  auto& root = Root::singleton();
  auto assets = root.assets();
  auto configuration = root.configuration();
  auto connectionSettings = configuration->get("connectionSettings");

  int64_t now = Time::monotonicMilliseconds();
  bool stateTimedOut = now >= pendingConnection.stateDeadline || !pendingConnection.connection.isOpen();

  switch (pendingConnection.state) {
    case PendingConnectionState::AwaitProtocolRequest: {
      pendingConnection.connection.receive();
      auto packet = pendingConnection.connection.pullSingle();
      if (!packet) {
        if (stateTimedOut) {
          Logger::warn("UniverseServer: client connection aborted, expected ProtocolRequestPacket");
          m_pendingHandshakeTimedOut++;
          pendingConnection.state = PendingConnectionState::Dead;
        }
        return;
      }

      auto protocolRequest = as<ProtocolRequestPacket>(packet);
      if (!protocolRequest) {
        Logger::warn("UniverseServer: client connection aborted, expected ProtocolRequestPacket");
        m_pendingHandshakeRejected++;
        pendingConnection.state = PendingConnectionState::Dead;
        return;
      }

      pendingConnection.legacyClient = protocolRequest->compressionMode() != PacketCompressionMode::Enabled;
      if (pendingConnection.legacyClient)
        pendingConnection.connection.packetSocket().setNetRules(LegacyVersion);

      auto protocolResponse = make_shared<ProtocolResponsePacket>();
      protocolResponse->setCompressionMode(PacketCompressionMode::Enabled);
      if (protocolRequest->requestProtocolVersion != StarProtocolVersion) {
        Logger::warn("UniverseServer: client connection aborted, unsupported protocol version {}, supported version {}",
            protocolRequest->requestProtocolVersion, StarProtocolVersion);
        protocolResponse->allowed = false;
        pendingConnection.protocolAllowed = false;
        pendingConnection.connection.pushSingle(protocolResponse);
        m_pendingHandshakeRejected++;
        setPendingConnectionState(pendingConnection, PendingConnectionState::RejectAndFlush);
        return;
      }

      pendingConnection.protocolAllowed = true;
      protocolResponse->allowed = true;
      if (!pendingConnection.legacyClient) {
        auto compressionName = connectionSettings.getString("compression", "None");
        auto compressionMode = NetCompressionModeNames.maybeLeft(compressionName).value(NetCompressionMode::None);
        pendingConnection.useCompressionStream = compressionMode == NetCompressionMode::Zstd;
        protocolResponse->info = JsonObject{
          {"compression", NetCompressionModeNames.getRight(compressionMode)},
          {"openProtocolVersion", OpenProtocolVersion}};
      }
      pendingConnection.connection.pushSingle(protocolResponse);
      setPendingConnectionState(pendingConnection, PendingConnectionState::SendProtocolResponse);
      return;
    }

    case PendingConnectionState::SendProtocolResponse: {
      pendingConnection.connection.send();
      if (pendingConnection.connection.packetSocket().sentPacketsPending()) {
        if (stateTimedOut)
          setPendingConnectionState(pendingConnection, PendingConnectionState::RejectAndFlush);
        return;
      }

      if (auto compressedSocket = as<CompressedPacketSocket>(&pendingConnection.connection.packetSocket()))
        compressedSocket->setCompressionStreamEnabled(pendingConnection.useCompressionStream);

      pendingConnection.remoteAddressString = pendingConnection.remoteAddress ? toString(*pendingConnection.remoteAddress) : "local";
      Logger::info("UniverseServer: Awaiting connection info from {} ({} client)",
          pendingConnection.remoteAddressString, pendingConnection.legacyClient ? "vanilla" : "custom");
      setPendingConnectionState(pendingConnection, PendingConnectionState::AwaitClientConnect);
      return;
    }

    case PendingConnectionState::AwaitClientConnect: {
      pendingConnection.connection.receive();
      auto packet = pendingConnection.connection.pullSingle();
      if (!packet) {
        if (stateTimedOut)
          failPendingConnection(pendingConnection, "connect timeout", true);
        return;
      }

      pendingConnection.clientConnect = as<ClientConnectPacket>(packet);
      if (!pendingConnection.clientConnect) {
        failPendingConnection(pendingConnection, "Expected ClientConnectPacket.");
        return;
      }

      pendingConnection.accountString = !pendingConnection.clientConnect->account.empty()
          ? strf("'{}'", pendingConnection.clientConnect->account)
          : "<anonymous>";
      setPendingConnectionState(pendingConnection, PendingConnectionState::FinalizeClient);

    #ifdef STAR_PLATFORM_N3DS
      String serverAssetsMismatchMessage = "Server assets mismatch";
      String clientAssetsMismatchMessage = "Client assets mismatch";
    #else
      String serverAssetsMismatchMessage = assets->json("/universe_server.config:serverAssetsMismatchMessage").toString();
      String clientAssetsMismatchMessage = assets->json("/universe_server.config:clientAssetsMismatchMessage").toString();
    #endif

      if (connectionSettings.getBool("requireLatestVersion", false)
          && (pendingConnection.legacyClient || pendingConnection.clientConnect->info.getUInt("openProtocolVersion", 0) < OpenProtocolVersion)) {
        failPendingConnection(pendingConnection, strf("OpenStarbound v{} or later is required.\nSource ID: {}...", OpenStarVersionString, String(StarSourceIdentifierString, 8)));
        return;
      }

      if (!pendingConnection.remoteAddress) {
        pendingConnection.administrator = true;
        Logger::info("UniverseServer: Logged in player '{}' locally", pendingConnection.clientConnect->playerName);
      } else {
        if (pendingConnection.clientConnect->assetsDigest != m_assetsDigest) {
          if (!configuration->get("allowAssetsMismatch").toBool()) {
            failPendingConnection(pendingConnection, serverAssetsMismatchMessage);
            return;
          } else if (!pendingConnection.clientConnect->allowAssetsMismatch) {
            failPendingConnection(pendingConnection, clientAssetsMismatchMessage);
            return;
          }
        }

        if (!m_speciesShips.contains(pendingConnection.clientConnect->shipSpecies)) {
          failPendingConnection(pendingConnection, "Unknown ship species");
          return;
        }

        if (!pendingConnection.clientConnect->account.empty()) {
#ifdef STAR_PLATFORM_N3DS
          pendingConnection.passwordSalt = secureRandomBytes(16);
#else
          pendingConnection.passwordSalt = secureRandomBytes(assets->json("/universe_server.config:passwordSaltLength").toUInt());
#endif
          Logger::info("UniverseServer: Sending Handshake Challenge");
          pendingConnection.connection.pushSingle(make_shared<HandshakeChallengePacket>(pendingConnection.passwordSalt));
          pendingConnection.challengeQueued = true;
          setPendingConnectionState(pendingConnection, PendingConnectionState::AwaitHandshakeResponse);
          return;
        } else {
          if (!configuration->get("allowAnonymousConnections").toBool()) {
            failPendingConnection(pendingConnection, "Anonymous connections disallowed");
            return;
          }
          pendingConnection.administrator = configuration->get("anonymousConnectionsAreAdmin").toBool();
        }

        if (auto reason = isBannedUser(pendingConnection.remoteAddress, pendingConnection.clientConnect->playerUuid)) {
          failPendingConnection(pendingConnection, "You are banned: " + *reason);
          return;
        }
      }

      finalizePendingConnection(pendingConnection);
      return;
    }

    case PendingConnectionState::AwaitHandshakeResponse: {
      pendingConnection.connection.send();
      if (pendingConnection.connection.packetSocket().sentPacketsPending()) {
        if (stateTimedOut)
          failPendingConnection(pendingConnection, "Expected HandshakeResponsePacket.", true);
        return;
      }

      pendingConnection.connection.receive();
      auto packet = pendingConnection.connection.pullSingle();
      if (!packet) {
        if (stateTimedOut)
          failPendingConnection(pendingConnection, "Expected HandshakeResponsePacket.", true);
        return;
      }

      auto handshakeResponsePacket = as<HandshakeResponsePacket>(packet);
      if (!handshakeResponsePacket) {
        failPendingConnection(pendingConnection, "Expected HandshakeResponsePacket.");
        return;
      }

      bool success = false;
      if (Json account = configuration->get("serverUsers").get(pendingConnection.clientConnect->account, {})) {
        pendingConnection.administrator = account.getBool("admin", false);
        ByteArray passAccountSalt = (account.getString("password") + pendingConnection.clientConnect->account).utf8Bytes();
        passAccountSalt.append(pendingConnection.passwordSalt);
        ByteArray passHash = sha256(passAccountSalt);
        if (passHash == handshakeResponsePacket->passHash)
          success = true;
      }

      if (!success) {
        failPendingConnection(pendingConnection, strf("No such account '{}' or incorrect password", pendingConnection.clientConnect->account));
        return;
      }

      if (auto reason = isBannedUser(pendingConnection.remoteAddress, pendingConnection.clientConnect->playerUuid)) {
        failPendingConnection(pendingConnection, "You are banned: " + *reason);
        return;
      }

      finalizePendingConnection(pendingConnection);
      return;
    }

    case PendingConnectionState::FinalizeClient:
      finalizePendingConnection(pendingConnection);
      return;

    case PendingConnectionState::RejectAndFlush:
      pendingConnection.connection.send();
      m_deadConnections.append({std::move(pendingConnection.connection), Time::monotonicMilliseconds()});
      pendingConnection.state = PendingConnectionState::Dead;
      return;

    case PendingConnectionState::Dead:
      return;
  }
}

bool UniverseServer::finalizePendingConnection(PendingConnection& pendingConnection) {
  auto& root = Root::singleton();
  auto assets = root.assets();
  auto versioningDatabase = root.versioningDatabase();
  auto clientConnect = pendingConnection.clientConnect;
  if (!clientConnect) {
    pendingConnection.state = PendingConnectionState::Dead;
    return false;
  }

  String connectionLog = strf("UniverseServer: Logged in account '{}' as player '{}' from address {}",
      pendingConnection.accountString, clientConnect->playerName, pendingConnection.remoteAddressString);

  NetCompatibilityRules netRules(pendingConnection.legacyClient ? LegacyVersion : 1);
  netRules.setIsAdmin(pendingConnection.administrator);
  if (Json& info = clientConnect->info) {
    if (auto openProtocolVersion = info.optUInt("openProtocolVersion"))
      netRules.setVersion(*openProtocolVersion);
    if (Json brand = info.get("brand", "custom"))
      connectionLog += strf(" ({} client)", brand.toString());
    if (info.getBool("legacy", false))
      netRules.setVersion(LegacyVersion);
  }
  pendingConnection.connection.packetSocket().setNetRules(netRules);
  Logger::log(LogLevel::Info, connectionLog.utf8Ptr());

  WriteLocker clientsLocker(m_clientsLock);
  if (auto clashId = getClientForUuid(clientConnect->playerUuid)) {
    if (pendingConnection.administrator) {
      clientsLocker.unlock();
      doDisconnection(*clashId, "Duplicate UUID joined and is Administrator so has priority.");
      clientsLocker.lock();
    } else {
      failPendingConnection(pendingConnection, "Duplicate player UUID");
      return false;
    }
  }

  if (m_clients.size() + 1 > m_maxPlayers && !pendingConnection.administrator) {
    failPendingConnection(pendingConnection, "Max player connections");
    return false;
  }

  ConnectionId clientId = m_clients.nextId();
  auto clientContext = make_shared<ServerClientContext>(clientId, pendingConnection.remoteAddress, netRules, clientConnect->playerUuid,
      clientConnect->playerName, clientConnect->shipSpecies, pendingConnection.administrator, clientConnect->shipChunks);
#ifdef STAR_PLATFORM_N3DS
  Logger::info("N3DS UniverseServer: compact client context ready");
#else
  clientContext->registerRpcHandlers(m_teamManager->authenticatedRpcHandlers(clientContext->playerUuid()));
#endif

#ifndef STAR_PLATFORM_N3DS
  String clientContextFile = File::relativeTo(m_storageDirectory, strf("{}.clientcontext", clientConnect->playerUuid.hex()));
  if (File::isFile(clientContextFile)) {
    try {
      auto contextStore = versioningDatabase->loadVersionedJson(VersionedJson::readFile(clientContextFile), "ClientContext");
      clientContext->loadServerData(contextStore);
    } catch (std::exception const& e) {
      Logger::error("UniverseServer: Could not load client context file for <User: {}>, ignoring! {}",
          clientConnect->playerName, outputException(e, false));
      File::rename(clientContextFile, strf("{}.{}.fail", clientContextFile, Time::millisecondsSinceEpoch()));
    }
  }
#else
  Logger::info("N3DS UniverseServer: skipped persisted client context load");
#endif

  if (!pendingConnection.administrator)
    clientContext->setAdmin(false);

  clientContext->setShipUpgrades(clientConnect->shipUpgrades);

  m_connectionServer->addConnection(clientId, std::move(pendingConnection.connection));
#ifdef STAR_PLATFORM_N3DS
  Logger::info("N3DS UniverseServer: connection registered");
  m_connectionServer->sendPackets(clientId, {make_shared<ConnectSuccessPacket>(clientId, m_universeSettings->uuid(), m_celestialDatabase->baseInformation())});
  Logger::info("N3DS UniverseServer: sent compact connect success");
#else
  m_connectionServer->sendPackets(clientId, {make_shared<ConnectSuccessPacket>(clientId, m_universeSettings->uuid(), m_celestialDatabase->baseInformation()), make_shared<UniverseTimeUpdatePacket>(m_universeClock->time()), make_shared<PausePacket>(*m_pause, GlobalTimescale)});
#endif

  m_clients.add(clientId, clientContext);
#ifndef STAR_PLATFORM_N3DS
  m_chatProcessor->connectClient(clientId, clientConnect->playerName);
#else
  Logger::info("N3DS UniverseServer: skipped chat client registration");
#endif
  clientsLocker.unlock();

#ifndef STAR_PLATFORM_N3DS
  setPvp(clientId, false);

  Vec3I location = clientContext->shipCoordinate().location();
  if (location != Vec3I()) {
    try {
      auto clientSystem = createSystemWorld(location);
      clientSystem->addClient(clientId, clientContext->playerUuid(), clientContext->shipUpgrades().shipSpeed, clientContext->shipLocation());
      addCelestialRequests(clientId, {makeLeft(location.vec2()), makeRight(location)});
      clientContext->setSystemWorld(clientSystem);
    } catch (StarException const& e) {
      Logger::error("Failed to place client ship at {}, resetting coordinate: {}", clientContext->shipCoordinate(), outputException(e, true));
      clientContext->setShipCoordinate({});
    }
  }
#else
  Logger::info("N3DS UniverseServer: skipped initial PVP/team and system placement");
#endif

#ifdef STAR_PLATFORM_N3DS
  Logger::info("N3DS UniverseServer: spawning player at ship");
  clientWarpPlayer(clientId, WarpAlias::OwnShip);
  Logger::info("N3DS UniverseServer: queued initial ship warp");
#else
  Json introInstance = assets->json("/universe_server.config:introInstance");
  String speciesIntroInstance = introInstance.getString(clientConnect->shipSpecies, introInstance.getString("default", ""));
  if (!speciesIntroInstance.empty() && !clientConnect->introComplete) {
    Logger::info("UniverseServer: Spawning player in intro instance {}", speciesIntroInstance);
    WarpAction introWarp = WarpToWorld{InstanceWorldId(speciesIntroInstance, clientContext->playerUuid()), {}};
    clientWarpPlayer(clientId, introWarp);
  } else if (auto reviveWarp = clientContext->playerReviveWarp()) {
    bool useReviveWarp = true;
    if (reviveWarp.world.is<InstanceWorldId>()) {
      String instance = reviveWarp.world.get<InstanceWorldId>().instance;
      auto worldConfig = Root::singleton().assets()->json("/instance_worlds.config").opt(instance);
      if (!worldConfig || !worldConfig->getBool("persistent", false))
        useReviveWarp = false;
    }

    if (reviveWarp.world.is<ClientShipWorldId>() && reviveWarp.world.get<ClientShipWorldId>() != clientConnect->playerUuid)
      useReviveWarp = false;

    if (useReviveWarp) {
      Logger::info("UniverseServer: Reviving player at {}", reviveWarp.world);
      clientWarpPlayer(clientId, reviveWarp);
    } else {
      Logger::info("UniverseServer: Player revive position is expired, spawning back at own ship");
      clientWarpPlayer(clientId, WarpAlias::OwnShip);
    }
  } else {
    Maybe<String> defaultReviveWarp = assets->json("/universe_server.config").optString("defaultReviveWarp");
    if (defaultReviveWarp) {
      Logger::info("UniverseServer: Spawning player at default warp");
      clientWarpPlayer(clientId, parseWarpAction(*defaultReviveWarp));
    } else {
      Logger::info("UniverseServer: Spawning player at ship");
      clientWarpPlayer(clientId, WarpAlias::OwnShip);
    }
  }
#endif

#ifdef STAR_PLATFORM_N3DS
  Logger::info("N3DS UniverseServer: skipped initial ship flight queue");
#else
  clientFlyShip(clientId, clientContext->shipCoordinate().location(), clientContext->shipLocation());
#endif
  Logger::info("UniverseServer: Client {} connected", clientContext->descriptiveName());

  ReadLocker clientsReadLocker(m_clientsLock);
  auto players = static_cast<uint16_t>(m_clients.size());
  auto clients = m_clients.keys();
  clientsReadLocker.unlock();

#ifndef STAR_PLATFORM_N3DS
  for (auto clientId : clients)
    m_connectionServer->sendPackets(clientId, {make_shared<ServerInfoPacket>(players, static_cast<uint16_t>(m_maxPlayers))});
#endif

  for (auto& p : m_scriptContexts)
    p.second->invoke("acceptConnection", clientId);

  m_pendingHandshakeFinalized++;
  pendingConnection.state = PendingConnectionState::Dead;
  return true;
}

void UniverseServer::acceptConnection(UniverseConnection connection, Maybe<HostAddress> remoteAddress) {
  auto& root = Root::singleton();
  auto assets = root.assets();
  auto configuration = root.configuration();
  auto versioningDatabase = root.versioningDatabase();

#ifdef STAR_PLATFORM_N3DS
  int clientWaitLimit = 10000;
  String serverAssetsMismatchMessage = "Server assets mismatch";
  String clientAssetsMismatchMessage = "Client assets mismatch";
#else
  int clientWaitLimit = assets->json("/universe_server.config:clientWaitLimit").toInt();
  String serverAssetsMismatchMessage = assets->json("/universe_server.config:serverAssetsMismatchMessage").toString();
  String clientAssetsMismatchMessage = assets->json("/universe_server.config:clientAssetsMismatchMessage").toString();
#endif
  auto connectionSettings = configuration->get("connectionSettings");

  RecursiveMutexLocker mainLocker(m_mainLock, false);

  connection.receiveAny(clientWaitLimit);
  auto protocolRequest = as<ProtocolRequestPacket>(connection.pullSingle());
  if (!protocolRequest) {
    Logger::warn("UniverseServer: client connection aborted, expected ProtocolRequestPacket");
    return;
  }

  bool legacyClient = protocolRequest->compressionMode() != PacketCompressionMode::Enabled;
  if (legacyClient)
    connection.packetSocket().setNetRules(LegacyVersion);

  auto protocolResponse = make_shared<ProtocolResponsePacket>();
  protocolResponse->setCompressionMode(PacketCompressionMode::Enabled);// Signal that we're OpenStarbound
  if (protocolRequest->requestProtocolVersion != StarProtocolVersion) {
    Logger::warn("UniverseServer: client connection aborted, unsupported protocol version {}, supported version {}",
                 protocolRequest->requestProtocolVersion, StarProtocolVersion);
    protocolResponse->allowed = false;
    connection.pushSingle(protocolResponse);
    connection.sendAll(clientWaitLimit);
    mainLocker.lock();
    m_deadConnections.append({std::move(connection), Time::monotonicMilliseconds()});
    return;
  }

  bool useCompressionStream = false;
  protocolResponse->allowed = true;
  if (!legacyClient) {
    auto compressionName = connectionSettings.getString("compression", "None");
    auto compressionMode = NetCompressionModeNames.maybeLeft(compressionName).value(NetCompressionMode::None);
    useCompressionStream = compressionMode == NetCompressionMode::Zstd;
    protocolResponse->info = JsonObject{
      {"compression", NetCompressionModeNames.getRight(compressionMode)},
      {"openProtocolVersion", OpenProtocolVersion}};
  }
  connection.pushSingle(protocolResponse);
  connection.sendAll(clientWaitLimit);

  if (auto compressedSocket = as<CompressedPacketSocket>(&connection.packetSocket()))
    compressedSocket->setCompressionStreamEnabled(useCompressionStream);

  String remoteAddressString = remoteAddress ? toString(*remoteAddress) : "local";
  Logger::info("UniverseServer: Awaiting connection info from {} ({} client)", remoteAddressString, legacyClient ? "vanilla" : "custom");

  connection.receiveAny(clientWaitLimit);
  auto clientConnect = as<ClientConnectPacket>(connection.pullSingle());
  if (!clientConnect) {
    Logger::warn("UniverseServer: client connection aborted");
    connection.pushSingle(make_shared<ConnectFailurePacket>("connect timeout"));
    mainLocker.lock();
    m_deadConnections.append({std::move(connection), Time::monotonicMilliseconds()});
    return;
  }

  bool administrator = false;
  String accountString = !clientConnect->account.empty() ? strf("'{}'", clientConnect->account) : "<anonymous>";

  auto connectionFail = [&](String message) {
    Logger::warn("UniverseServer: Login attempt failed with account '{}' as player '{}' from address {}, error: {}",
                 accountString, clientConnect->playerName, remoteAddressString, message);
    connection.pushSingle(make_shared<ConnectFailurePacket>(std::move(message)));
    mainLocker.lock();
    m_deadConnections.append({std::move(connection), Time::monotonicMilliseconds()});
  };

  if (connectionSettings.getBool("requireLatestVersion", false)
      && (legacyClient || clientConnect->info.getUInt("openProtocolVersion", 0) < OpenProtocolVersion)) {
    connectionFail(strf("OpenStarbound v{} or later is required.\nSource ID: {}...", OpenStarVersionString, String(StarSourceIdentifierString, 8)));
    return;
  }

  if (!remoteAddress) {
    administrator = true;
    Logger::info("UniverseServer: Logged in player '{}' locally", clientConnect->playerName);
  } else {
    if (clientConnect->assetsDigest != m_assetsDigest) {
      if (!configuration->get("allowAssetsMismatch").toBool()) {
        connectionFail(serverAssetsMismatchMessage);
        return;
      } else if (!clientConnect->allowAssetsMismatch) {
        connectionFail(clientAssetsMismatchMessage);
        return;
      }
    }

    if (!m_speciesShips.contains(clientConnect->shipSpecies)) {
      connectionFail("Unknown ship species");
      return;
    }

    if (!clientConnect->account.empty()) {
    #ifdef STAR_PLATFORM_N3DS
      auto passwordSalt = secureRandomBytes(16);
    #else
      auto passwordSalt = secureRandomBytes(assets->json("/universe_server.config:passwordSaltLength").toUInt());
    #endif
      Logger::info("UniverseServer: Sending Handshake Challenge");
      connection.pushSingle(make_shared<HandshakeChallengePacket>(passwordSalt));
      connection.sendAll(clientWaitLimit);
      connection.receiveAny(clientWaitLimit);
      shared_ptr<HandshakeResponsePacket> handshakeResponsePacket = as<HandshakeResponsePacket>(connection.pullSingle());
      if (!handshakeResponsePacket) {
        connectionFail("Expected HandshakeResponsePacket.");
        return;
      }

      bool success = false;
      if (Json account = configuration->get("serverUsers").get(clientConnect->account, {})) {
        administrator = account.getBool("admin", false);
        ByteArray passAccountSalt = (account.getString("password") + clientConnect->account).utf8Bytes();
        passAccountSalt.append(passwordSalt);
        ByteArray passHash = sha256(passAccountSalt);
        if (passHash == handshakeResponsePacket->passHash)
          success = true;
      }
      // Give the same message for missing account vs wrong password to
      // prevent account detection, overkill given the overall level of
      // security but hey, why not.
      if (!success) {
        connectionFail(strf("No such account '{}' or incorrect password", clientConnect->account));
        return;
      }
    } else {
      if (!configuration->get("allowAnonymousConnections").toBool()) {
        connectionFail("Anonymous connections disallowed");
        return;
      }
      administrator = configuration->get("anonymousConnectionsAreAdmin").toBool();
    }

    if (auto reason = isBannedUser(remoteAddress, clientConnect->playerUuid)) {
      connectionFail("You are banned: " + *reason);
      return;
    }
  }

  String connectionLog = strf("UniverseServer: Logged in account '{}' as player '{}' from address {}",
                              accountString, clientConnect->playerName, remoteAddressString);

  NetCompatibilityRules netRules(legacyClient ? LegacyVersion : 1);
  netRules.setIsAdmin(administrator);
  if (Json& info = clientConnect->info) {
    if (auto openProtocolVersion = info.optUInt("openProtocolVersion"))
      netRules.setVersion(*openProtocolVersion);
    if (Json brand = info.get("brand", "custom"))
      connectionLog += strf(" ({} client)", brand.toString());
    if (info.getBool("legacy", false))
      netRules.setVersion(LegacyVersion);
  }
  connection.packetSocket().setNetRules(netRules);
  Logger::log(LogLevel::Info, connectionLog.utf8Ptr());

  WriteLocker clientsLocker(m_clientsLock);
  if (auto clashId = getClientForUuid(clientConnect->playerUuid)) {
    if (administrator) {
      clientsLocker.unlock();
      doDisconnection(*clashId, "Duplicate UUID joined and is Administrator so has priority.");
      clientsLocker.lock();
    } else {
      connectionFail("Duplicate player UUID");
      return;
    }
  }

  if (m_clients.size() + 1 > m_maxPlayers && !administrator) {
    connectionFail("Max player connections");
    return;
  }

  ConnectionId clientId = m_clients.nextId();
  auto clientContext = make_shared<ServerClientContext>(clientId, remoteAddress, netRules, clientConnect->playerUuid,
                                                        clientConnect->playerName, clientConnect->shipSpecies, administrator, clientConnect->shipChunks);
#ifdef STAR_PLATFORM_N3DS
  Logger::info("N3DS UniverseServer: compact client context ready");
#else
  clientContext->registerRpcHandlers(m_teamManager->authenticatedRpcHandlers(clientContext->playerUuid()));
#endif

#ifndef STAR_PLATFORM_N3DS
  String clientContextFile = File::relativeTo(m_storageDirectory, strf("{}.clientcontext", clientConnect->playerUuid.hex()));
  if (File::isFile(clientContextFile)) {
    try {
      auto contextStore = versioningDatabase->loadVersionedJson(VersionedJson::readFile(clientContextFile), "ClientContext");
      clientContext->loadServerData(contextStore);
    } catch (std::exception const& e) {
      Logger::error("UniverseServer: Could not load client context file for <User: {}>, ignoring! {}",
                    clientConnect->playerName, outputException(e, false));
      File::rename(clientContextFile, strf("{}.{}.fail", clientContextFile, Time::millisecondsSinceEpoch()));
    }
  }
#else
  Logger::info("N3DS UniverseServer: skipped persisted client context load");
#endif

  // Need to do this after loadServerData because it sets the admin flag
  if (!administrator)
    clientContext->setAdmin(false);

  clientContext->setShipUpgrades(clientConnect->shipUpgrades);

  m_connectionServer->addConnection(clientId, std::move(connection));
#ifdef STAR_PLATFORM_N3DS
  Logger::info("N3DS UniverseServer: connection registered");
  m_connectionServer->sendPackets(clientId, {make_shared<ConnectSuccessPacket>(clientId, m_universeSettings->uuid(), m_celestialDatabase->baseInformation())});
  Logger::info("N3DS UniverseServer: sent compact connect success");
#else
  m_connectionServer->sendPackets(clientId, {make_shared<ConnectSuccessPacket>(clientId, m_universeSettings->uuid(), m_celestialDatabase->baseInformation()), make_shared<UniverseTimeUpdatePacket>(m_universeClock->time()), make_shared<PausePacket>(*m_pause, GlobalTimescale)});
#endif

  m_clients.add(clientId, clientContext);
#ifndef STAR_PLATFORM_N3DS
  m_chatProcessor->connectClient(clientId, clientConnect->playerName);
#else
  Logger::info("N3DS UniverseServer: skipped chat client registration");
#endif
  clientsLocker.unlock();

#ifndef STAR_PLATFORM_N3DS
  setPvp(clientId, false);

  Vec3I location = clientContext->shipCoordinate().location();
  if (location != Vec3I()) {
    try {
      auto clientSystem = createSystemWorld(location);
      clientSystem->addClient(clientId, clientContext->playerUuid(), clientContext->shipUpgrades().shipSpeed, clientContext->shipLocation());
      addCelestialRequests(clientId, {makeLeft(location.vec2()), makeRight(location)});
      clientContext->setSystemWorld(clientSystem);
    }
    catch (StarException const& e) {
      Logger::error("Failed to place client ship at {}, resetting coordinate: {}", clientContext->shipCoordinate(), outputException(e, true));
      clientContext->setShipCoordinate({});
    }
  }
#else
  Logger::info("N3DS UniverseServer: skipped initial PVP/team and system placement");
#endif

#ifdef STAR_PLATFORM_N3DS
  Logger::info("N3DS UniverseServer: spawning player at ship");
  clientWarpPlayer(clientId, WarpAlias::OwnShip);
  Logger::info("N3DS UniverseServer: queued initial ship warp");
#else
  Json introInstance = assets->json("/universe_server.config:introInstance");
  String speciesIntroInstance = introInstance.getString(clientConnect->shipSpecies, introInstance.getString("default", ""));
  if (!speciesIntroInstance.empty() && !clientConnect->introComplete) {
    Logger::info("UniverseServer: Spawning player in intro instance {}", speciesIntroInstance);
    WarpAction introWarp = WarpToWorld{InstanceWorldId(speciesIntroInstance, clientContext->playerUuid()), {}};
    clientWarpPlayer(clientId, introWarp);
  } else if (auto reviveWarp = clientContext->playerReviveWarp()) {
    // Do not revive players at non-persistent instance worlds or on ship worlds that
    // are not their own ship.
    bool useReviveWarp = true;
    if (reviveWarp.world.is<InstanceWorldId>()) {
      String instance = reviveWarp.world.get<InstanceWorldId>().instance;
      auto worldConfig = Root::singleton().assets()->json("/instance_worlds.config").opt(instance);
      if (!worldConfig || !worldConfig->getBool("persistent", false))
        useReviveWarp = false;
    }

    if (reviveWarp.world.is<ClientShipWorldId>() && reviveWarp.world.get<ClientShipWorldId>() != clientConnect->playerUuid)
      useReviveWarp = false;

    if (useReviveWarp) {
      Logger::info("UniverseServer: Reviving player at {}", reviveWarp.world);
      clientWarpPlayer(clientId, reviveWarp);
    } else {
      Logger::info("UniverseServer: Player revive position is expired, spawning back at own ship");
      clientWarpPlayer(clientId, WarpAlias::OwnShip);
    }
  } else {
    Maybe<String> defaultReviveWarp = assets->json("/universe_server.config").optString("defaultReviveWarp");
    if (defaultReviveWarp) {
      Logger::info("UniverseServer: Spawning player at default warp");
      clientWarpPlayer(clientId, parseWarpAction(*defaultReviveWarp));
    } else {
      Logger::info("UniverseServer: Spawning player at ship");
      clientWarpPlayer(clientId, WarpAlias::OwnShip);
    }
  }
#endif

#ifdef STAR_PLATFORM_N3DS
  Logger::info("N3DS UniverseServer: skipped initial ship flight queue");
#else
  clientFlyShip(clientId, clientContext->shipCoordinate().location(), clientContext->shipLocation());
#endif
  Logger::info("UniverseServer: Client {} connected", clientContext->descriptiveName());

  ReadLocker clientsReadLocker(m_clientsLock);
  auto players = static_cast<uint16_t>(m_clients.size());
  auto clients = m_clients.keys();
  clientsReadLocker.unlock();

#ifndef STAR_PLATFORM_N3DS
  for (auto clientId : clients) {
    m_connectionServer->sendPackets(clientId, {make_shared<ServerInfoPacket>(players, static_cast<uint16_t>(m_maxPlayers))});
  }
#endif

  for (auto& p : m_scriptContexts)
    p.second->invoke("acceptConnection", clientId);
}

WarpToWorld UniverseServer::resolveWarpAction(WarpAction warpAction, ConnectionId clientId, bool deploy) const {
  auto clientContext = m_clients.value(clientId);
  if (!clientContext)
    return {};

  WorldId toWorldId;
  SpawnTarget spawnTarget;
  for (auto& p : m_scriptContexts) {
    auto out = p.second->invoke<Json>("overrideWarp", warpActionToJson(warpAction), clientId, deploy);
    if (out && *out) {
      auto& jout = *out;
      if (auto world = jout.optString("worldId")) {
        toWorldId = parseWorldId(*world);
      } else {
        toWorldId = clientContext->playerWorldId();
      }
      if (jout.opt("spawnTarget")) {
        spawnTarget = spawnTargetFromJson(jout.get("spawnTarget"));
      }
      return WarpToWorld(toWorldId, spawnTarget);
    }
  }

  if (auto toWorld = warpAction.ptr<WarpToWorld>()) {
    if (!toWorld->world)
      toWorldId = clientContext->playerWorldId();
    else
      toWorldId = toWorld->world;
    spawnTarget = toWorld->target;
  } else if (auto toPlayerUuid = warpAction.ptr<WarpToPlayer>()) {
    if (auto toClientId = getClientForUuid(*toPlayerUuid)) {
      if (auto toClientWorld = m_clients.get(*toClientId)->playerWorld()) {
        if (auto toClientPosition = toClientWorld->playerRevivePosition(*toClientId)) {
          toWorldId = toClientWorld->worldId();
          if (deploy)
            spawnTarget.reset();
          else
            spawnTarget = SpawnTargetPosition(*toClientPosition);
        }
      }
    }
  } else if (auto shortcut = warpAction.ptr<WarpAlias>()) {
    if (*shortcut == WarpAlias::Return) {
      if (auto returnWarp = clientContext->playerReturnWarp()) {
        toWorldId = returnWarp.world;
        spawnTarget = returnWarp.target;
      }
    } else if (*shortcut == WarpAlias::OrbitedWorld) {
      if (auto warpAction = clientContext->orbitWarpAction()) {
        if (auto warpToWorld = warpAction->first.maybe<WarpToWorld>()) {
          toWorldId = warpToWorld->world;
          spawnTarget = warpToWorld->target;
        }
      }
    } else if (*shortcut == WarpAlias::OwnShip) {
      toWorldId = ClientShipWorldId(clientContext->playerUuid());
    }
  }

  if (auto shipWorldId = toWorldId.ptr<ClientShipWorldId>()) {
    if (m_secureWarps && !canWarpToShip(clientId, *shipWorldId))
      return {};
  }

  return WarpToWorld(toWorldId, spawnTarget);
}

bool UniverseServer::canWarpToShip(ConnectionId clientId, Uuid const& targetShipUuid) const {
  auto clientContext = m_clients.value(clientId);
  if (!clientContext)
    return false;

  Uuid callerUuid = clientContext->playerUuid();

  if (targetShipUuid == callerUuid || clientContext->isAdmin())
    return true;

  auto callerTeam = m_teamManager->getTeam(callerUuid);
  if (!callerTeam)
    return false;

  if (auto ownerTeam = m_teamManager->getTeam(targetShipUuid)) {
    if (*ownerTeam == *callerTeam)
      return true;
  }

  for (auto const& otherClient : m_clients) {
    if (otherClient.first == clientId)
      continue;
    auto otherTeam = m_teamManager->getTeam(otherClient.second->playerUuid());
    if (!otherTeam || *otherTeam != *callerTeam)
      continue;
    WorldId otherWorldId = otherClient.second->playerWorldId();
    if (auto otherShipWorld = otherWorldId.ptr<ClientShipWorldId>()) {
      if (*otherShipWorld == targetShipUuid)
        return true;
    }
  }

  Logger::warn("UniverseServer: Rejected ship warp from {} to ship {} - not in team and no teammate present",
               callerUuid.hex(), targetShipUuid.hex());
  return false;
}

void UniverseServer::doDisconnection(ConnectionId clientId, String const& reason) {
  RecursiveMutexLocker locker(m_mainLock);
  WriteLocker clientsLocker(m_clientsLock);
  if (auto clientContext = m_clients.value(clientId)) {
    m_teamManager->playerDisconnected(clientContext->playerUuid());
    clientsLocker.unlock();
    // The client should revive at their ship if they are in an un-revivable
    // state
    WarpToWorld reviveWarp = WarpToWorld(ClientShipWorldId(clientContext->playerUuid()));
    if (auto currentWorld = clientContext->playerWorld()) {
      auto currentWorldId = currentWorld->worldId();
      locker.unlock();
      if (auto playerRevivePosition = currentWorld->playerRevivePosition(clientId))
        reviveWarp = WarpToWorld(currentWorldId, SpawnTargetPosition(*playerRevivePosition));
      auto finalPackets = currentWorld->removeClient(clientId);
      m_connectionServer->sendPackets(clientId, finalPackets);
      m_chatProcessor->leaveChannel(clientId, printWorldId(currentWorld->worldId()));
      locker.lock();
    }

    clientContext->clearPlayerWorld();
    clientContext->setPlayerReviveWarp(reviveWarp);

    if (auto systemWorld = clientContext->systemWorld())
      systemWorld->removeClient(clientId);

    clientContext->clearSystemWorld();

    if (m_chatProcessor->hasClient(clientId))
      m_chatProcessor->disconnectClient(clientId);

    if (m_connectionServer->connectionIsOpen(clientId)) {
      // Send the client the last ship update.
      if (auto shipWorld = getWorld(ClientShipWorldId(clientContext->playerUuid()))) {
        locker.unlock();
        shipWorld->unloadAll(true);
        clientContext->applyShipChunksUpdate(shipWorld->readChunkUpdate(clientContext->shipChunks()));
        shipWorld->stop();
        locker.lock();
      }

      auto clientContextSnapshot = buildClientContextStorageSnapshot(clientContext);
      sendClientContextUpdate(clientContext);

      // Then send the disconnect packet.
      m_connectionServer->sendPackets(clientId, {make_shared<ServerDisconnectPacket>(reason)});

      writeVersionedJsonStorageSnapshots({std::move(clientContextSnapshot)});
    } else {
      writeVersionedJsonStorageSnapshots({buildClientContextStorageSnapshot(clientContext)});
    }

    clientsLocker.lock();
    m_clients.remove(clientId);
    m_deadConnections.append({m_connectionServer->removeConnection(clientId), Time::monotonicMilliseconds()});
    Logger::info("UniverseServer: Client {} disconnected for reason: {}", clientContext->descriptiveName(), reason);

    auto players = static_cast<uint16_t>(m_clients.size());
    for (auto clientId : m_clients.keys()) {
      m_connectionServer->sendPackets(clientId, {make_shared<ServerInfoPacket>(players, static_cast<uint16_t>(m_maxPlayers))});
    }
    clientsLocker.unlock();

    for (auto& p : m_scriptContexts)
      p.second->invoke("doDisconnection", clientId);
  }
}

Maybe<ConnectionId> UniverseServer::getClientForUuid(Uuid const& uuid) const {
  for (auto const& p : m_clients) {
    if (p.second->playerUuid() == uuid)
      return p.second->clientId();
  }

  return {};
}

WorldServerThreadPtr UniverseServer::getWorld(WorldId const& worldId) {
  if (m_worlds.contains(worldId)) {
    auto& maybeWorldPromise = m_worlds.get(worldId);
    try {
      if (!maybeWorldPromise || !maybeWorldPromise->poll())
        return {};

      return maybeWorldPromise->get();
    } catch (std::exception const& e) {
      maybeWorldPromise.reset();
      Logger::error("UniverseServer: error during world create: {}", outputException(e, true));
      worldDiedWithError(worldId);
    }
  }

  return {};
}

WorldServerThreadPtr UniverseServer::createWorld(WorldId const& worldId) {
  if (!m_worlds.contains(worldId)) {
    if (auto promise = makeWorldPromise(worldId))
      m_worlds.add(worldId, promise.take());
    else
      return {};
  }

  auto& maybeWorldPromise = m_worlds.get(worldId);
  if (!maybeWorldPromise)
    return {};
  try {
    return maybeWorldPromise->get();
  } catch (std::exception const& e) {
    maybeWorldPromise.reset();
    Logger::error("UniverseServer: error during world create: {}", outputException(e, true));
    worldDiedWithError(worldId);
    return {};
  }
}

Maybe<WorldServerThreadPtr> UniverseServer::triggerWorldCreation(WorldId const& worldId) {
  if (!m_worlds.contains(worldId)) {
    if (auto promise = makeWorldPromise(worldId)) {
      auto worldPromise = promise.take();
      m_worlds.add(worldId, worldPromise);
#ifdef STAR_PLATFORM_N3DS
      try {
        if (worldPromise.poll()) {
          Logger::info("N3DS UniverseServer: world creation completed synchronously for {}", printWorldId(worldId));
          return worldPromise.get();
        }
      } catch (std::exception const& e) {
        auto& maybeWorldPromise = m_worlds.get(worldId);
        maybeWorldPromise.reset();
        Logger::error("UniverseServer: error during world create: {}", outputException(e, true));
        worldDiedWithError(worldId);
        return WorldServerThreadPtr();
      }
#endif
      return {};
    } else {
      return WorldServerThreadPtr();
    }
  } else {
    auto& maybeWorldPromise = m_worlds.get(worldId);
    try {
      // If the promise is reset, this means that the promise threw an
      // exception, return nullptr to signify error.
      if (!maybeWorldPromise)
        return WorldServerThreadPtr();

      if (!maybeWorldPromise->poll())
        return {};

      return maybeWorldPromise->get();
    } catch (std::exception const& e) {
      maybeWorldPromise.reset();
      Logger::error("UniverseServer: error during world create: {}", outputException(e, true));
      worldDiedWithError(worldId);
      return WorldServerThreadPtr();
    }
  }
}

Maybe<WorkerPoolPromise<WorldServerThreadPtr>> UniverseServer::makeWorldPromise(WorldId const& worldId) {
  if (auto celestialWorld = worldId.ptr<CelestialWorldId>())
    return celestialWorldPromise(*celestialWorld);
  else if (auto shipWorld = worldId.ptr<ClientShipWorldId>())
    return shipWorldPromise(*shipWorld);
  else if (auto instanceWorld = worldId.ptr<InstanceWorldId>())
    return instanceWorldPromise(*instanceWorld);
  else
    return {};
}

Maybe<WorkerPoolPromise<WorldServerThreadPtr>> UniverseServer::shipWorldPromise(
  ClientShipWorldId const& clientShipWorldId) {
  auto clientId = clientForUuid(clientShipWorldId);
  if (!clientId)
    return {};

  auto clientContext = m_clients.get(*clientId);
  auto speciesShips = m_speciesShips;
  auto celestialDatabase = m_celestialDatabase;
  auto universeClock = m_universeClock;

  return m_workerPool.addProducer<WorldServerThreadPtr>([this, clientShipWorldId, clientContext, speciesShips, celestialDatabase, universeClock]() {
    WorldServerPtr shipWorld;

    auto shipChunks = clientContext->shipChunks();
    if (!shipChunks.empty()) {
      try {
        Logger::info("UniverseServer: Loading client ship world {}", clientShipWorldId);
        shipWorld = make_shared<WorldServer>(shipChunks);
      } catch (std::exception const& e) {
        Logger::error("UniverseServer: Could not load client ship {}, resetting ship to default state! {}",
                      clientShipWorldId, outputException(e, false));
      }
    }

    if (!shipWorld) {
      Logger::info("UniverseServer: Creating new client ship world {}", clientShipWorldId);
    #ifdef STAR_PLATFORM_N3DS
      auto species = clientContext->shipSpecies();
      if (species.empty() || !speciesShips.contains(species)) {
        Logger::info("N3DS UniverseServer: falling back to human ship for missing species '{}'", species);
        species = "human";
        clientContext->setShipSpecies(species);
      }
    #else
      auto& species = clientContext->shipSpecies();
    #endif
      ShipUpgrades currentUpgrades = clientContext->shipUpgrades();
#ifdef STAR_PLATFORM_N3DS
      auto shipStructure = WorldStructure(speciesShips.get(species).first());
      auto structureRegion = shipStructure.region();
      auto structureSize = structureRegion.isNull() ? Vec2I(32, 32) : structureRegion.size();
      Vec2U worldSize(
          max<unsigned>(96, (unsigned)structureSize.x() + 32),
          max<unsigned>(64, (unsigned)structureSize.y() + 24));
      if (auto configuredWorldSize = shipStructure.configValue("worldSize")) {
        auto originalWorldSize = jsonToVec2U(configuredWorldSize);
        worldSize[0] = min<unsigned>(worldSize[0], originalWorldSize[0]);
        worldSize[1] = min<unsigned>(worldSize[1], originalWorldSize[1]);
      }
      Logger::info("N3DS UniverseServer: creating bounded original ship world {} from structure region {}", worldSize, structureRegion);
      shipWorld = make_shared<WorldServer>(worldSize, File::ephemeralFile());
        auto shipForegroundBlocks = shipStructure.foregroundBlocks().size();
        auto shipBackgroundBlocks = shipStructure.backgroundBlocks().size();
        auto shipObjects = shipStructure.objects().size();
        auto shipOverlays = shipStructure.backgroundOverlays().size() + shipStructure.foregroundOverlays().size();
        auto shipUpgradeConfig = shipStructure.configValue("shipUpgrades");
        shipWorld->setCentralStructure(std::move(shipStructure));
      Logger::info("N3DS UniverseServer: original ship structure placed blocks fg={} bg={} objects={} overlays={}",
          shipForegroundBlocks, shipBackgroundBlocks, shipObjects, shipOverlays);
      currentUpgrades.apply(Root::singleton().assets()->json("/ships/shipupgrades.config"));
        if (shipUpgradeConfig.type() != Json::Type::Null)
          currentUpgrades.apply(shipUpgradeConfig);
      if (currentUpgrades.maxFuel == 0)
        currentUpgrades.maxFuel = 1000;
      if (currentUpgrades.crewSize == 0)
        currentUpgrades.crewSize = 2;
      if (currentUpgrades.shipSpeed == 0)
        currentUpgrades.shipSpeed = 1.0f;
#else
      auto shipStructure = WorldStructure(speciesShips.get(species).first());
      Vec2U worldSize(2048, 2048);
      if (auto jWorldSize = shipStructure.configValue("worldSize"))
        worldSize = jsonToVec2U(jWorldSize);
      shipWorld = make_shared<WorldServer>(worldSize, File::ephemeralFile());
      shipStructure = shipWorld->setCentralStructure(shipStructure);
      currentUpgrades.apply(Root::singleton().assets()->json("/ships/shipupgrades.config"));
      currentUpgrades.apply(shipStructure.configValue("shipUpgrades"));
#endif
      clientContext->setShipUpgrades(currentUpgrades);

      shipWorld->setSpawningEnabled(false);
      shipWorld->setProperty("invinciblePlayers", true);
      shipWorld->setProperty("ship.level", 0);
      shipWorld->setProperty("ship.species", species);
      shipWorld->setProperty("ship.fuel", 0);
      shipWorld->setProperty("ship.maxFuel", currentUpgrades.maxFuel);
      shipWorld->setProperty("ship.crewSize", currentUpgrades.crewSize);
      shipWorld->setProperty("ship.fuelEfficiency", currentUpgrades.fuelEfficiency);
      shipWorld->setProperty("ship.epoch", Time::timeSinceEpoch());
    }

    auto shipClock = make_shared<Clock>();
    auto shipTime = shipWorld->getProperty("ship.epoch");
    if (!shipTime.canConvert(Json::Type::Float)) {
      auto now = Time::timeSinceEpoch();
      shipWorld->setProperty("ship.epoch", now);
    } else {
      shipClock->setTime(Time::timeSinceEpoch() - shipTime.toDouble());
    }

    shipWorld->setUniverseSettings(m_universeSettings);
    shipWorld->setReferenceClock(shipClock);
    shipClock->start();

    if (auto systemWorld = clientContext->systemWorld())
      shipWorld->setOrbitalSky(systemWorld->clientSkyParameters(clientContext->clientId()));
    else
      shipWorld->setOrbitalSky(celestialSkyParameters(clientContext->shipCoordinate()));

    shipWorld->initLua(this);

    auto shipWorldThread = make_shared<WorldServerThread>(shipWorld, ClientShipWorldId(clientShipWorldId));
    shipWorldThread->setPause(m_pause);
#ifdef STAR_PLATFORM_N3DS
    Logger::info("N3DS UniverseServer: skipped ship chunk snapshot");
#else
    clientContext->applyShipChunksUpdate(shipWorldThread->readChunkUpdate(clientContext->shipChunks()));
#endif
    shipWorldThread->start();
    shipWorldThread->setUpdateAction(bind(&UniverseServer::worldUpdated, this, _1));

    return shipWorldThread;
  });
}

Maybe<WorkerPoolPromise<WorldServerThreadPtr>> UniverseServer::celestialWorldPromise(CelestialWorldId const& celestialWorldId) {
  if (!celestialWorldId)
    return {};

  auto storageDirectory = m_storageDirectory;
  auto celestialDatabase = m_celestialDatabase;
  auto universeClock = m_universeClock;

  return m_workerPool.addProducer<WorldServerThreadPtr>([this, celestialWorldId, storageDirectory, celestialDatabase, universeClock]() {
    WorldServerPtr worldServer;
    String storageFile = File::relativeTo(storageDirectory, strf("{}.world", celestialWorldId.filename()));
    if (File::isFile(storageFile)) {
      try {
        Logger::info("UniverseServer: Loading celestial world {}", celestialWorldId);
        worldServer = make_shared<WorldServer>(File::open(storageFile, IOMode::ReadWrite));
      } catch (std::exception const& e) {
        Logger::error("UniverseServer: Could not load celestial world {}, removing! Cause: {}",
                      celestialWorldId, outputException(e, false));
        File::rename(storageFile, strf("{}.{}.fail", storageFile, Time::millisecondsSinceEpoch()));
      }
    }

    if (!worldServer) {
      Logger::info("UniverseServer: Creating celestial world {}", celestialWorldId);
      auto worldTemplate = make_shared<WorldTemplate>(celestialWorldId, celestialDatabase);
      worldServer = make_shared<WorldServer>(worldTemplate, File::open(storageFile, IOMode::ReadWrite | IOMode::Truncate));
    }

    worldServer->setUniverseSettings(m_universeSettings);
    worldServer->setReferenceClock(universeClock);
    worldServer->initLua(this);

    auto worldThread = make_shared<WorldServerThread>(worldServer, celestialWorldId);
    worldThread->setPause(m_pause);
    worldThread->start();
    worldThread->setUpdateAction(bind(&UniverseServer::worldUpdated, this, _1));

    return worldThread;
  });
}

Maybe<WorkerPoolPromise<WorldServerThreadPtr>> UniverseServer::instanceWorldPromise(InstanceWorldId const& instanceWorldId) {
  auto storageDirectory = m_storageDirectory;
  auto universeClock = m_universeClock;
  return m_workerPool.addProducer<WorldServerThreadPtr>([this, storageDirectory, instanceWorldId, universeClock]() {
    Json worldConfig = Root::singleton().assets()->json("/instance_worlds.config").get(instanceWorldId.instance);
    uint64_t worldSeed;
    if (worldConfig.contains("seed"))
      worldSeed = worldConfig.getUInt("seed");
    else
      worldSeed = Random::randu64();

    String worldType = worldConfig.getString("type");

    VisitableWorldParametersPtr worldParameters;
    if (worldType.equalsIgnoreCase("Terrestrial"))
      worldParameters = generateTerrestrialWorldParameters(worldConfig.getString("planetType"), worldConfig.getString("planetSize"), worldSeed);
    else if (worldType.equalsIgnoreCase("Asteroids"))
      worldParameters = generateAsteroidsWorldParameters(worldSeed);
    else if (worldType.equalsIgnoreCase("FloatingDungeon"))
      worldParameters = generateFloatingDungeonWorldParameters(worldConfig.getString("dungeonWorld"));
    else
      throw UniverseServerException(strf("Unknown world type: '{}'\n", worldType));

    if (instanceWorldId.level)
      worldParameters->threatLevel = *instanceWorldId.level;

    if (worldConfig.contains("beamUpRule"))
      worldParameters->beamUpRule = BeamUpRuleNames.getLeft(worldConfig.getString("beamUpRule"));
    worldParameters->disableDeathDrops = worldConfig.getBool("disableDeathDrops", false);

    SkyParameters skyParameters = SkyParameters(worldConfig.get("skyParameters", Json()));
    auto worldTemplate = make_shared<WorldTemplate>(worldParameters, skyParameters, worldSeed);
    Json worldProperties = worldConfig.get("worldProperties", JsonObject{});
    bool spawningEnabled = worldConfig.getBool("spawningEnabled", true);
    bool persistent = worldConfig.getBool("persistent", false);
    bool useUniverseClock = worldConfig.getBool("useUniverseClock", false);

    WorldServerPtr worldServer;

    bool worldExisted = false;

    if (persistent) {
      String identifier = instanceWorldId.instance;
      if (instanceWorldId.uuid)
        identifier = strf("{}-{}", identifier, instanceWorldId.uuid->hex());
      if (instanceWorldId.level)
        identifier = strf("{}-{}", identifier, instanceWorldId.level.value());
      String storageFile = File::relativeTo(storageDirectory, strf("unique-{}.world", identifier));
      if (File::isFile(storageFile)) {
        try {
          Logger::info("UniverseServer: Loading persistent unique instance world {}", instanceWorldId.instance);
          worldServer = make_shared<WorldServer>(File::open(storageFile, IOMode::ReadWrite));
          worldExisted = true;
        } catch (std::exception const& e) {
          Logger::error("UniverseServer: Could not load persistent unique instance world {}, removing! Cause: {}",
                        instanceWorldId.instance, outputException(e, false));
          File::rename(storageFile, strf("{}.{}.fail", storageFile, Time::millisecondsSinceEpoch()));
        }
      }

      if (!worldServer) {
        Logger::info("UniverseServer: Creating persistent unique instance world {}", instanceWorldId.instance);
        worldServer = make_shared<WorldServer>(worldTemplate, File::open(storageFile, IOMode::ReadWrite | IOMode::Truncate));
      }
    } else {
      String storageFile = tempWorldFile(instanceWorldId);
      uint64_t deleteTime = worldConfig.optInt("tempWorldDeleteTime").value(0);
      if (File::isFile(storageFile)) {
        if (m_tempWorldIndex.contains(instanceWorldId)) {
          auto file = File::open(storageFile, IOMode::ReadWrite);
          if (file->size() > 0) {
            Logger::info("UniverseServer: Loading temporary instance world {} from storage", instanceWorldId);
            try {
              worldServer = make_shared<WorldServer>(file);
              worldExisted = true;
            } catch (std::exception const& e) {
              Logger::error("UniverseServer: Could not load temporary instance world '{}', re-creating cause: {}",
                            instanceWorldId, outputException(e, false));
            }
          }
        } else {
          File::remove(storageFile);
        }
      }

      if (!worldServer) {
        Logger::info("UniverseServer: Creating temporary instance world '{}' with expiry time {}", instanceWorldId, deleteTime);

        worldServer = make_shared<WorldServer>(worldTemplate, File::open(storageFile, IOMode::ReadWrite));
        m_tempWorldIndex.set(instanceWorldId, pair<uint64_t, uint64_t>(m_universeClock->milliseconds(), deleteTime));
      }
    }

    worldServer->setUniverseSettings(m_universeSettings);
    for (auto const& p : worldProperties.iterateObject())
      worldServer->setProperty(p.first, p.second);
    worldServer->setProperty("ephemeral", !persistent);
    worldServer->setSpawningEnabled(spawningEnabled);
    if (useUniverseClock)
      worldServer->setReferenceClock(universeClock);

    if (!worldExisted) {
      for (auto flagAction : m_universeSettings->currentFlagActionsForInstanceWorld(instanceWorldId.instance)) {
        if (flagAction.is<PlaceDungeonFlagAction>()) {
          auto placeDungeonAction = flagAction.get<PlaceDungeonFlagAction>();
          worldServer->placeDungeon(placeDungeonAction.dungeonId, placeDungeonAction.targetPosition, 0);
        }
      }
    }

    worldServer->initLua(this);

    auto worldThread = make_shared<WorldServerThread>(worldServer, instanceWorldId);
    worldThread->setPause(m_pause);
    worldThread->start();
    worldThread->setUpdateAction(bind(&UniverseServer::worldUpdated, this, _1));

    return worldThread;
  });
}

SystemWorldServerThreadPtr UniverseServer::createSystemWorld(Vec3I const& location) {
  if (!m_systemWorlds.contains(location)) {
    SystemWorldServerPtr systemWorld;

    String storageFile = File::relativeTo(m_storageDirectory, strf("{}_{}_{}.system", location[0], location[1], location[2]));
    bool loadedFromStorage = false;
    if (File::isFile(storageFile)) {
      Logger::info("UniverseServer: Loading system world {} from disk storage", location);
      try {
        auto versioningDatabase = Root::singleton().versioningDatabase();
        VersionedJson versionedStore = VersionedJson::readFile(storageFile);
        Json store = versioningDatabase->loadVersionedJson(versionedStore, "System");

        systemWorld = make_shared<SystemWorldServer>(store, m_universeClock, m_celestialDatabase);
        loadedFromStorage = true;
      } catch (std::exception const& e) {
        Logger::error("UniverseServer: Failed to load system {} from disk storage, re-creating. Cause: {}", location, outputException(e, false));
        File::rename(storageFile, strf("{}.{}.fail", storageFile, Time::millisecondsSinceEpoch()));
        loadedFromStorage = false;
      }
    }

    if (!loadedFromStorage) {
      Logger::info("UniverseServer: Creating new system world at location {}", location);
      systemWorld = make_shared<SystemWorldServer>(location, m_universeClock, m_celestialDatabase);
    }

    auto systemThread = make_shared<SystemWorldServerThread>(location, systemWorld, storageFile);
    systemThread->setUpdateAction(bind(&UniverseServer::systemWorldUpdated, this, _1));
    systemThread->start();
    m_systemWorlds.set(location, systemThread);
  }

  return m_systemWorlds.get(location);
}

bool UniverseServer::instanceWorldStoredOrActive(InstanceWorldId const& worldId) const {
  String storageFile = File::relativeTo(m_storageDirectory, strf("unique-{}.world", worldId.instance));
  return m_worlds.value(worldId).isValid() || m_tempWorldIndex.contains(worldId) || File::isFile(storageFile);
}

void UniverseServer::worldDiedWithError(WorldId world) {
  if (world.is<ClientShipWorldId>()) {
    if (auto clientId = getClientForUuid(world.get<ClientShipWorldId>()))
      m_pendingDisconnections.add(*clientId, "Client ship world has errored");
  }
}

SkyParameters UniverseServer::celestialSkyParameters(CelestialCoordinate const& coordinate) const {
  if (m_celestialDatabase->coordinateValid(coordinate))
    return SkyParameters(coordinate, m_celestialDatabase);
  return SkyParameters();
}

void UniverseServer::startLuaScripts() {
#ifdef STAR_PLATFORM_N3DS
  m_luaRoot = make_shared<LuaRoot>();
  Logger::info("N3DS UniverseServer: skipped universe Lua scripts");
#else
  auto assets = Root::singleton().assets();
  auto universeConfig = assets->json("/universe_server.config");

  m_luaRoot = make_shared<LuaRoot>();
  m_luaRoot->tuneAutoGarbageCollection(universeConfig.getFloat("luaGcPause"), universeConfig.getFloat("luaGcStepMultiplier"));

  for (auto& p : universeConfig.getObject("scriptContexts")) {
    auto scriptComponent = make_shared<ScriptComponent>();
    scriptComponent->setLuaRoot(m_luaRoot);
    scriptComponent->addCallbacks("universe", LuaBindings::makeUniverseServerCallbacks(this));
    scriptComponent->setScripts(jsonToStringList(p.second.toArray()));

    m_scriptContexts.set(p.first, scriptComponent);
    scriptComponent->init();
  }
#endif
}

void UniverseServer::updateLua() {
  for (auto& p : m_scriptContexts)
    p.second->update();
}

void UniverseServer::stopLua() {
  for (auto& p : m_scriptContexts)
    p.second->uninit();

  m_scriptContexts.clear();
}

}// namespace Star

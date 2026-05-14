#pragma once

#include "StarLockFile.hpp"
#include "StarIdMap.hpp"
#include "StarWorkerPool.hpp"
#include "StarGameTypes.hpp"
#include "StarCelestialCoordinate.hpp"
#include "StarServerClientContext.hpp"
#include "StarServerTiming.hpp"
#include "StarWorldServerThread.hpp"
#include "StarSystemWorldServerThread.hpp"
#include "StarUniverseConnection.hpp"
#include "StarUniverseSettings.hpp"
#include "StarVersioningDatabase.hpp"

namespace Star {

STAR_CLASS(Clock);
STAR_CLASS(File);
STAR_CLASS(Player);
STAR_CLASS(ChatProcessor);
STAR_CLASS(CommandProcessor);
STAR_CLASS(TeamManager);
STAR_CLASS(UniverseServer);
STAR_CLASS(WorldTemplate);
STAR_CLASS(WorldServer);
STAR_CLASS(UniverseSettings);

STAR_EXCEPTION(UniverseServerException, StarException);

// Manages all running worlds, listens for new client connections and marshalls
// between all the different worlds and all the different client connections
// and routes packets between them.
class UniverseServer : public Thread {
public:
  struct ServerStatus {
    using TimingStatus = ServerTimingStatus;

    double uptime;
    bool listeningTcp;
    bool tcpListenFailed;
    bool paused;
    float timescale;
    float tickRate;
    size_t clients;
    uint32_t maxClients;
    size_t activeWorlds;
    size_t systemWorlds;
    size_t pendingConnectionAccepts;
    size_t pendingHandshakes;
    size_t pendingHandshakeAwaitProtocolRequest;
    size_t pendingHandshakeSendProtocolResponse;
    size_t pendingHandshakeAwaitClientConnect;
    size_t pendingHandshakeAwaitHandshakeResponse;
    size_t pendingHandshakeFinalizeClient;
    size_t pendingHandshakeRejectAndFlush;
    uint64_t pendingHandshakeAccepted;
    uint64_t pendingHandshakeFinalized;
    uint64_t pendingHandshakeRejected;
    uint64_t pendingHandshakeTimedOut;
    size_t deadConnections;
    size_t pendingPlayerWarps;
    size_t queuedFlights;
    size_t pendingFlights;
    size_t pendingArrivals;
    size_t pendingDisconnections;
    size_t pendingCelestialRequestClients;
    size_t pendingCelestialRequests;
    size_t pendingChatClients;
    size_t pendingChatMessages;
    size_t pendingWorldMessageWorlds;
    size_t pendingWorldMessages;
    size_t worldCommandQueueDepth;
    int64_t worldCommandOldestPendingAgeMicroseconds;
    uint64_t worldCommandsProcessed;
    uint64_t worldCommandsDirect;
    uint64_t worldCommandsFailed;
    uint64_t worldCommandWaitMicroseconds;
    uint64_t worldPacketPrepTicks;
    uint64_t worldPacketPrepMonitoringRegionBuilds;
    uint64_t worldPacketPrepMonitoringRegionRects;
    uint64_t worldPacketPrepMonitoringRegionSplitRects;
    uint64_t worldPacketPrepMonitoringRegionReuses;
    uint64_t worldPacketPrepSectorCacheHits;
    uint64_t worldPacketPrepSectorCacheMisses;
    uint64_t worldPacketPrepEntityStoreCacheHits;
    uint64_t worldPacketPrepEntityStoreCacheMisses;
    uint64_t worldPacketPrepNetStateCacheHits;
    uint64_t worldPacketPrepNetStateCacheMisses;
    uint64_t worldPacketPrepEntityUpdateSetPackets;
    uint64_t worldPacketPrepEntityUpdateSetDeltas;
    uint64_t worldPacketPrepEmptyEntityUpdateSetPackets;
    uint64_t worldPacketPrepEmptyEntityUpdateSetSkips;
    uint64_t worldPacketPrepSectorClientFanoutLookups;
    uint64_t worldPacketPrepSectorClientFanoutRecipients;
    uint64_t worldPacketPrepSectorClientFanoutMisses;
    HashMap<EntityType, WorldServer::EntitySerializationStats> worldPacketPrepEntitySerializationStats;
    WorldStorageTimingStats worldStorageTimingStats;
    bool persistenceAsyncEnabled;
    size_t phase6StorageGenerationPlanningEnabledWorlds;
    uint64_t phase6StorageGenerationPlanningTicks;
    uint64_t phase6StorageGenerationPlanningSerialTicks;
    uint64_t phase6StorageGenerationPlanningParallelTicks;
    uint64_t phase6StorageGenerationPlanningSectors;
    uint64_t phase6StorageGenerationPlanningSerialMicroseconds;
    uint64_t phase6StorageGenerationPlanningParallelMicroseconds;
    uint64_t phase6StorageGenerationPlanningMergeMicroseconds;
    uint64_t phase6StorageGenerationPlanningFallbacks;
    size_t phase6StorageGenerationPlanningDifferentialCheckEnabledWorlds;
    uint64_t phase6StorageGenerationPlanningDifferentialChecks;
    uint64_t phase6StorageGenerationPlanningDifferentialMicroseconds;
    uint64_t phase6StorageGenerationPlanningDivergences;
    size_t phase6PacketPreparationSectorPrefillEnabledWorlds;
    uint64_t phase6PacketPreparationSectorPrefillTicks;
    uint64_t phase6PacketPreparationSectorPrefillSerialTicks;
    uint64_t phase6PacketPreparationSectorPrefillParallelTicks;
    uint64_t phase6PacketPreparationSectorPrefillSectors;
    uint64_t phase6PacketPreparationSectorPrefillSerialMicroseconds;
    uint64_t phase6PacketPreparationSectorPrefillParallelMicroseconds;
    uint64_t phase6PacketPreparationSectorPrefillMergeMicroseconds;
    uint64_t phase6PacketPreparationSectorPrefillFallbacks;
    size_t phase6PacketPreparationSectorPrefillDifferentialCheckEnabledWorlds;
    uint64_t phase6PacketPreparationSectorPrefillDifferentialChecks;
    uint64_t phase6PacketPreparationSectorPrefillDifferentialMicroseconds;
    uint64_t phase6PacketPreparationSectorPrefillDivergences;
    size_t phase6SubsystemBaselineMetricsEnabledWorlds;
    uint64_t phase6LiquidBaselineTicks;
    uint64_t phase6LiquidActiveCells;
    uint64_t phase6LiquidMonitoringRegions;
    uint64_t phase6LiquidNoProcessingLimitRegionCacheBuilds;
    uint64_t phase6LiquidNoProcessingLimitRegionCacheRebuildSkips;
    uint64_t phase6LiquidNoProcessingLimitRegionCacheRegions;
    uint64_t phase6LiquidNoProcessingLimitRegionCacheBuckets;
    uint64_t phase6LiquidNoProcessingLimitRegionCacheLookups;
    uint64_t phase6LiquidNoProcessingLimitRegionCacheCandidates;
    uint64_t phase6LiquidNoProcessingLimitRegionCacheHits;
    size_t phase6MutationFixedSeedSignaturesEnabledWorlds;
    uint64_t phase6LiquidSignatureTicks;
    uint64_t phase6LiquidSignatureActiveCells;
    uint64_t phase6LiquidSignatureActiveCellHash;
    uint64_t phase6LiquidSignatureRegionHash;
    uint64_t phase6FallingBlocksBaselineTicks;
    uint64_t phase6FallingBlocksPendingPositions;
    uint64_t phase6FallingBlocksNextPendingPositions;
    uint64_t phase6FallingBlocksProcessedPositions;
    uint64_t phase6FallingBlocksMovedBlocks;
    uint64_t phase6FallingBlocksSignatureTicks;
    uint64_t phase6FallingBlocksPendingPositionSignature;
    uint64_t phase6FallingBlocksProcessedPositionSignature;
    uint64_t phase6FallingBlocksMovedBlockSignature;
    uint64_t phase6FallingBlocksNextPendingPositionSignature;
    uint64_t phase6WiringBaselineTicks;
    uint64_t phase6WiringInitialEntities;
    uint64_t phase6WiringLoadedEntities;
    uint64_t phase6WiringNetworkLoads;
    uint64_t phase6WiringEvaluatedEntities;
    uint64_t phase6WiringNetworkSignatureChecks;
    uint64_t phase6WiringCleanNetworkSignatures;
    uint64_t phase6WiringDirtyNetworkSignatures;
    uint64_t phase6WiringTopologyDirtyNetworkSignatures;
    uint64_t phase6WiringOutputDirtyNetworkSignatures;
    uint64_t phase6WiringCleanNetworkEntities;
    uint64_t phase6WiringDirtyNetworkEntities;
    uint64_t phase6WiringSignatureTicks;
    uint64_t phase6WiringTopologySignatureHash;
    uint64_t phase6WiringOutputSignatureHash;
    uint64_t phase6EntityBaselineTicks;
    uint64_t phase6EntityUpdatedEntities;
    uint64_t phase6EntityTileEntities;
    uint64_t phase6EntityDestroyedEntities;
    uint64_t phase6EntityIterationCopies;
    uint64_t phase6EntitySortedEntities;
    uint64_t phase6EntityCopyMicroseconds;
    uint64_t phase6EntitySortMicroseconds;
    uint64_t phase6EntityUpdateMicroseconds;
    uint64_t phase6EntityMetadataRefreshMicroseconds;
    uint64_t phase6LuaBaselineTicks;
    uint64_t phase6LuaScriptContexts;
    uint64_t phase6LuaScriptUpdates;
    uint64_t phase6LuaScriptUpdateMicroseconds;
    uint64_t phase6LuaMaxScriptUpdateMicroseconds;
    uint32_t phase6MutationParallelismRequestedSubsystems;
    uint32_t phase6MutationParallelismBlockedByFixedSeedGateSubsystems;
    uint32_t phase6MutationParallelismBlockedByDependencyGateSubsystems;
    uint32_t phase6MutationParallelismBlockedByModVisibilityGateSubsystems;
    uint32_t phase6MutationParallelismBlockedByImplementationGateSubsystems;
    uint32_t phase6MutationParallelismWorkerSubsystems;
    uint64_t phase6MutationWorkerTicks;
    uint64_t phase6MutationWorkerJobs;
    uint64_t phase6MutationWorkerMicroseconds;
    uint64_t phase6MutationWorkerMergeMicroseconds;
    uint64_t phase6MutationWorkerDifferentialChecks;
    uint64_t phase6MutationWorkerDivergences;
    uint64_t phase6MutationWorkerFallbacks;
    bool networkQueueOnlySends;
    size_t networkWorkers;
    size_t networkOwnedConnections;
    uint64_t networkPacketsProcessed;
    uint64_t networkQueuedSendBatches;
    uint64_t networkQueuedSendPackets;
    uint64_t networkEagerSendBatches;
    uint64_t networkEagerSendPackets;
    uint64_t networkEagerWriteTimeMicroseconds;
    uint64_t networkWorkerSendBatches;
    uint64_t networkWorkerSendPackets;
    uint64_t networkWorkerWriteTimeMicroseconds;
    uint64_t networkWakeups;
    uint64_t networkIdleTimedWaits;
    size_t persistenceBatchesPending;
    size_t persistenceSnapshotsPending;
    int64_t persistenceOldestPendingAgeMilliseconds;
    uint64_t persistenceBatchesCompleted;
    uint64_t persistenceSnapshotsWritten;
    uint64_t persistenceSnapshotBuildTimeMicroseconds;
    uint64_t persistenceWriteTimeMicroseconds;
    uint64_t persistenceCelestialCommitTimeMicroseconds;
    uint64_t persistenceCelestialCommits;
    uint64_t persistenceFailures;
    uint64_t persistenceWriteRetries;
    uint64_t persistenceSynchronousFallbacks;
    uint64_t persistenceQueueFullFallbacks;
    List<TimingStatus> universeTimings;
    List<TimingStatus> worldThreadTimings;
    List<TimingStatus> worldUpdateTimings;
  };

  struct WorldStats {
    WorldId worldId;
    bool loaded = false;
    bool loading = false;
    bool errored = false;
    size_t clients = 0;
    WorldServerThread::CommandStats commandStats{};
    WorldServer::PacketPreparationStats packetPreparationStats{};
    WorldStorageTimingStats storageTimingStats{};
    WorldServer::Phase6WorldParallelismStats phase6WorldParallelismStats{};
    List<ServerTimingStatus> threadTimings;
    List<ServerTimingStatus> worldTimings;
  };

  struct SystemWorldStats {
    Vec3I location;
    size_t clients = 0;
    size_t activeInstanceWorlds = 0;
    SystemWorldServerThread::CommandStats commandStats{};
    SystemWorldServer::PacketStats packetStats{};
  };

  struct WorldStatsSummary {
    List<WorldStats> worlds;
    List<SystemWorldStats> systemWorlds;
  };

  UniverseServer(String const& storageDir);
  ~UniverseServer();

  // If enabled, will listen on the configured server port for incoming
  // connections.
  void setListeningTcp(bool listenTcp);

  // Connects an arbitrary UniverseConnection to this server
  void addClient(UniverseConnection remoteConnection);
  // Constructs an in-process connection to a UniverseServer for a
  // UniverseClient, and returns the other side of the connection.
  UniverseConnection addLocalClient();
  // Constructs an in-process connection that is treated as remote by the
  // handshake path, and returns the other side of the connection.
  UniverseConnection addRemoteLocalClient(HostAddress const& remoteAddress);

  // Signals the UniverseServer to stop and then joins the thread.
  void stop();

  void setPause(bool pause);
  void setTimescale(float timescale);
  void setTickRate(float tickRate);

  List<WorldId> activeWorlds() const;
  bool isWorldActive(WorldId const& worldId) const;

  List<ConnectionId> clientIds() const;
  List<pair<ConnectionId, int64_t>> clientIdsAndCreationTime() const;
  size_t numberOfClients() const;
  uint32_t maxClients() const;
  ServerStatus serverStatus() const;
  WorldStatsSummary worldStats() const;
  List<UniverseConnectionServer::NetworkWorkerStats> connectionWorkerStats() const;
  bool isConnectedClient(ConnectionId clientId) const;

  String clientDescriptor(ConnectionId clientId) const;

  String clientNick(ConnectionId clientId) const;
  Maybe<ConnectionId> findNick(String const& nick) const;

  Maybe<Uuid> uuidForClient(ConnectionId clientId) const;
  Maybe<ConnectionId> clientForUuid(Uuid const& uuid) const;

  void adminBroadcast(String const& text);
  void adminWhisper(ConnectionId clientId, String const& text);
  String adminCommand(String text);

  bool isAdmin(ConnectionId clientId) const;
  bool canBecomeAdmin(ConnectionId clientId) const;
  void setAdmin(ConnectionId clientId, bool admin);

  bool isLocal(ConnectionId clientId) const;

  bool isPvp(ConnectionId clientId) const;
  void setPvp(ConnectionId clientId, bool pvp);

  RpcThreadPromise<Json> sendWorldMessage(WorldId const& worldId, String const& message, JsonArray const& args = {});

  void clientWarpPlayer(ConnectionId clientId, WarpAction action, bool deploy = false);
  void clientFlyShip(ConnectionId clientId, Vec3I const& system, SystemLocation const& location, Json const& settings = {});
  WorldId clientWorld(ConnectionId clientId) const;
  CelestialCoordinate clientShipCoordinate(ConnectionId clientId) const;

  ClockPtr universeClock() const;
  UniverseSettingsPtr universeSettings() const;

	CelestialDatabase& celestialDatabase();

  // If the client exists and is in a valid connection state, executes the
  // given function on the client world and player object in a thread safe way.
  // Returns true if function was called, false if client was not found or in
  // an invalid connection state.
  bool executeForClient(ConnectionId clientId, function<void(WorldServer*, PlayerPtr)> action);
  void disconnectClient(ConnectionId clientId, String const& reason);
  void banUser(ConnectionId clientId, String const& reason, pair<bool, bool> banType, Maybe<int> timeout);
  bool unbanIp(String const& addressString);
  bool unbanUuid(String const& uuidString);

  bool updatePlanetType(CelestialCoordinate const& coordinate, String const& newType, String const& weatherBiome);

  bool setWeather(CelestialCoordinate const& coordinate, String const& weatherName, bool force = false);

  StringList weatherList(CelestialCoordinate const& coordinate);

  bool sendPacket(ConnectionId clientId, PacketPtr packet);

protected:
  virtual void run();

private:
  struct TimeoutBan {
    int64_t banExpiry;
    String reason;
    Maybe<HostAddress> ip;
    Maybe<Uuid> uuid;
  };

  enum class UniverseTimingPhase : uint8_t {
    Loop,
    UpdateLua,
    UniverseFlags,
    TimedBans,
    SendPendingChat,
    Teams,
    Ships,
    ClockUpdates,
    KickErroredPlayers,
    ReapConnections,
    PendingHandshakes,
    PlanetTypeChanges,
    Warps,
    ShipFlights,
    ShipArrivals,
    Chat,
    ClientContextUpdates,
    CelestialRequests,
    BrokenWorlds,
    WorldMessages,
    InactiveWorlds,
    PersistenceCompletions,
    TriggeredStorage,
    Count
  };

  enum class TcpState : uint8_t { No, Yes, Fuck };

  enum class PendingConnectionState : uint8_t {
    AwaitProtocolRequest,
    SendProtocolResponse,
    AwaitClientConnect,
    AwaitHandshakeResponse,
    FinalizeClient,
    RejectAndFlush,
    Dead
  };

  struct PendingConnection {
    PendingConnection(uint64_t pendingId, UniverseConnection connection, Maybe<HostAddress> remoteAddress)
      : pendingId(pendingId), connection(std::move(connection)), remoteAddress(std::move(remoteAddress)) {}

    uint64_t pendingId;
    UniverseConnection connection;
    Maybe<HostAddress> remoteAddress;
    PendingConnectionState state = PendingConnectionState::AwaitProtocolRequest;
    int64_t stateDeadline = 0;
    bool protocolAllowed = false;
    bool legacyClient = false;
    bool useCompressionStream = false;
    bool administrator = false;
    bool challengeQueued = false;
    ByteArray passwordSalt;
    shared_ptr<ClientConnectPacket> clientConnect;
    String accountString;
    String remoteAddressString;
    String failureReason;
  };

  struct VersionedJsonStorageSnapshot {
    String jobType;
    String file;
    VersionedJson store;
  };

  struct PersistenceWriteResult {
    String jobType;
    String file;
    bool success;
    String error;
    int64_t durationMicroseconds;
    uint64_t retryCount;
  };

  struct PendingPersistenceWrite {
    PendingPersistenceWrite(WorkerPoolPromise<List<PersistenceWriteResult>> promise, size_t snapshotCount, int64_t queuedTime)
      : promise(std::move(promise)), snapshotCount(snapshotCount), queuedTime(queuedTime) {}

    WorkerPoolPromise<List<PersistenceWriteResult>> promise;
    size_t snapshotCount;
    int64_t queuedTime;
  };

  void processUniverseFlags();
  void sendPendingChat();
  void updateTeams();
  void updateShips();
  void sendClockUpdates();
  void sendClientContextUpdate(ServerClientContextPtr clientContext);
  void sendClientContextUpdates();
  void kickErroredPlayers();
  void reapConnections();
  void processPlanetTypeChanges();
  void warpPlayers();
  void flyShips();
  void arriveShips();
  void respondToCelestialRequests();
  void processChat();
  void clearBrokenWorlds();
  void handleWorldMessages();
  void shutdownInactiveWorlds();
  void doTriggeredStorage();

  void saveSettings();
  void loadSettings();

  void startLuaScripts();
  void updateLua();
  void stopLua();

  // Either returns the default configured starter world, or a new randomized
  // starter world, or if a randomized world is not yet available, starts a job
  // to find a randomized starter world and returns nothing until it is ready.
  Maybe<CelestialCoordinate> nextStarterWorld();

  void loadTempWorldIndex();
  void saveTempWorldIndex();
  String tempWorldFile(InstanceWorldId const& worldId) const;

  Maybe<String> isBannedUser(Maybe<HostAddress> hostAddress, Uuid playerUuid) const;
  void doTempBan(ConnectionId clientId, String const& reason, pair<bool, bool> banType, int timeout);
  void doPermBan(ConnectionId clientId, String const& reason, pair<bool, bool> banType);
  void removeTimedBan();

  void addCelestialRequests(ConnectionId clientId, List<CelestialRequest> requests);

  void worldUpdated(WorldServerThread* worldServer);
  void systemWorldUpdated(SystemWorldServerThread* systemWorldServer);
  void packetsReceived(UniverseConnectionServer* connectionServer, ConnectionId clientId, List<PacketPtr> packets);

  void enqueuePendingConnection(UniverseConnection connection, Maybe<HostAddress> remoteAddress);
  void processPendingConnections();
  void advancePendingConnection(PendingConnection& pendingConnection);
  void setPendingConnectionState(PendingConnection& pendingConnection, PendingConnectionState state);
  String pendingConnectionStateName(PendingConnectionState state) const;
  void failPendingConnection(PendingConnection& pendingConnection, String message, bool timedOut = false);
  bool finalizePendingConnection(PendingConnection& pendingConnection);

  void acceptConnection(UniverseConnection connection, Maybe<HostAddress> remoteAddress);

  // Main lock and clients read lock must be held when calling
  WarpToWorld resolveWarpAction(WarpAction warpAction, ConnectionId clientId, bool deploy) const;
  bool canWarpToShip(ConnectionId clientId, Uuid const& targetShipUuid) const;

  void doDisconnection(ConnectionId clientId, String const& reason);

  // Clients read lock must be held when calling
  Maybe<ConnectionId> getClientForUuid(Uuid const& uuid) const;

  // Get the world only if it is already loaded, Main lock must be held when
  // calling.
  WorldServerThreadPtr getWorld(WorldId const& worldId);

  // If the world is not created, block and load it, otherwise just return the
  // loaded world.  Main lock and Clients read lock must be held when calling.
  WorldServerThreadPtr createWorld(WorldId const& worldId);

  // Trigger off-thread world creation, returns a value when the creation is
  // finished, either successfully or with an error.  Main lock and Clients
  // read lock must be held when calling.
  Maybe<WorldServerThreadPtr> triggerWorldCreation(WorldId const& worldId);

  // Main lock and clients read lock must be held when calling world promise
  // generators
  Maybe<WorkerPoolPromise<WorldServerThreadPtr>> makeWorldPromise(WorldId const& worldId);
  Maybe<WorkerPoolPromise<WorldServerThreadPtr>> shipWorldPromise(ClientShipWorldId const& uuid);
  Maybe<WorkerPoolPromise<WorldServerThreadPtr>> celestialWorldPromise(CelestialWorldId const& coordinate);
  Maybe<WorkerPoolPromise<WorldServerThreadPtr>> instanceWorldPromise(InstanceWorldId const& instanceWorld);

  // If the system world is not created, initialize it, otherwise return the
  // already initialized one
  SystemWorldServerThreadPtr createSystemWorld(Vec3I const& location);

  bool instanceWorldStoredOrActive(InstanceWorldId const& worldId) const;

  VersionedJsonStorageSnapshot buildUniverseSettingsStorageSnapshot();
  VersionedJsonStorageSnapshot buildTempWorldIndexStorageSnapshot();
  VersionedJsonStorageSnapshot buildClientContextStorageSnapshot(ServerClientContextPtr const& clientContext);
  List<VersionedJsonStorageSnapshot> buildClientContextStorageSnapshots();
  List<VersionedJsonStorageSnapshot> buildTriggeredStorageSnapshots();
  static List<PersistenceWriteResult> writeVersionedJsonStorageSnapshotsNow(List<VersionedJsonStorageSnapshot> snapshots, unsigned maxRetries = 0);
  void recordPersistenceWriteResults(List<PersistenceWriteResult> results);
  void writeVersionedJsonStorageSnapshots(List<VersionedJsonStorageSnapshot> snapshots);
  void persistVersionedJsonStorageSnapshots(List<VersionedJsonStorageSnapshot> snapshots);
  void processPendingPersistenceWrites();
  void finishPendingPersistenceWrites();
  void cleanupAndCommitCelestialDatabase();

  static char const* universeTimingPhaseName(UniverseTimingPhase phase);
  static ServerStatus::TimingStatus timingStatus(char const* name, ServerTimingAccumulator const& timing);
  void recordUniverseTiming(UniverseTimingPhase phase, int64_t durationMicroseconds);

  // Signal that a world either failed to load, or died due to an exception,
  // kicks clients if that world is a ship world.  Main lock and clients read
  // lock must be held when calling.
  void worldDiedWithError(WorldId world);

  // Get SkyParameters if the coordinate is a valid world, and empty
  // SkyParameters otherwise.
  SkyParameters celestialSkyParameters(CelestialCoordinate const& coordinate) const;

  mutable RecursiveMutex m_mainLock;
  mutable Mutex m_universeTimingsMutex;
  List<ServerTimingAccumulator> m_universeTimings;

  double m_startTime;
  String m_storageDirectory;
  ByteArray m_assetsDigest;
  Maybe<LockFile> m_storageDirectoryLock;
  StringMap<StringList> m_speciesShips;
  CelestialMasterDatabasePtr m_celestialDatabase;
  ClockPtr m_universeClock;
  UniverseSettingsPtr m_universeSettings;
  WorkerPool m_workerPool;
  WorkerPool m_persistenceWorkerPool;

  int64_t m_storageTriggerDeadline;
  int64_t m_clearBrokenWorldsDeadline;
  int64_t m_lastClockUpdateSent;
  atomic<bool> m_stop;
  atomic<TcpState> m_tcpState;

  mutable ReadersWriterMutex m_clientsLock;
  unsigned m_maxPlayers;
  IdMap<ConnectionId, ServerClientContextPtr> m_clients;

  shared_ptr<atomic<bool>> m_pause;
  bool m_secureWarps;
  Map<WorldId, Maybe<WorkerPoolPromise<WorldServerThreadPtr>>> m_worlds;
  Map<InstanceWorldId, pair<int64_t, int64_t>> m_tempWorldIndex;
  Map<Vec3I, SystemWorldServerThreadPtr> m_systemWorlds;
  UniverseConnectionServerPtr m_connectionServer;
  bool m_usePendingConnectionStateMachine;
  int64_t m_pendingConnectionStateWaitLimit;
  uint64_t m_nextPendingConnectionId;
  uint64_t m_pendingHandshakeAccepted;
  uint64_t m_pendingHandshakeFinalized;
  uint64_t m_pendingHandshakeRejected;
  uint64_t m_pendingHandshakeTimedOut;
  LinkedList<shared_ptr<PendingConnection>> m_pendingConnections;

  bool m_useAsyncPersistence;
  size_t m_persistenceMaxQueuedSnapshots;
  unsigned m_persistenceMaxWriteRetries;
  size_t m_persistenceSnapshotsPending;
  uint64_t m_persistenceBatchesCompleted;
  uint64_t m_persistenceSnapshotsWritten;
  uint64_t m_persistenceSnapshotBuildTimeMicroseconds;
  uint64_t m_persistenceWriteTimeMicroseconds;
  uint64_t m_persistenceCelestialCommitTimeMicroseconds;
  uint64_t m_persistenceCelestialCommits;
  uint64_t m_persistenceFailures;
  uint64_t m_persistenceWriteRetries;
  uint64_t m_persistenceSynchronousFallbacks;
  uint64_t m_persistenceQueueFullFallbacks;
  List<PendingPersistenceWrite> m_pendingPersistenceWrites;

  mutable RecursiveMutex m_connectionAcceptThreadsMutex;
  List<ThreadFunction<void>> m_connectionAcceptThreads;
  LinkedList<pair<UniverseConnection, int64_t>> m_deadConnections;

  ChatProcessorPtr m_chatProcessor;
  CommandProcessorPtr m_commandProcessor;
  TeamManagerPtr m_teamManager;

  HashMap<ConnectionId, pair<WarpAction, bool>> m_pendingPlayerWarps;
  HashMap<ConnectionId, pair<tuple<Vec3I, SystemLocation, Json>, Maybe<double>>> m_queuedFlights;
  HashMap<ConnectionId, tuple<Vec3I, SystemLocation, Json>> m_pendingFlights;
  HashMap<ConnectionId, CelestialCoordinate> m_pendingArrivals;
  HashMap<ConnectionId, String> m_pendingDisconnections;
  HashMap<ConnectionId, List<WorkerPoolPromise<CelestialResponse>>> m_pendingCelestialRequests;
  List<pair<WorldId, UniverseFlagAction>> m_pendingFlagActions;
  HashMap<ConnectionId, List<tuple<String, ChatSendMode, JsonObject>>> m_pendingChat;
  Maybe<WorkerPoolPromise<CelestialCoordinate>> m_nextRandomizedStarterWorld;
  Map<WorldId, List<WorldServerThread::Message>> m_pendingWorldMessages;

  List<TimeoutBan> m_tempBans;

  LuaRootPtr m_luaRoot;

  typedef LuaUpdatableComponent<LuaBaseComponent> ScriptComponent;
  typedef shared_ptr<ScriptComponent> ScriptComponentPtr;
  StringMap<ScriptComponentPtr> m_scriptContexts;
};

}

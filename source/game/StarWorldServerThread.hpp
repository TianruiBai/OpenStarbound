#pragma once

#include "StarWorldServer.hpp"
#include "StarThread.hpp"
#include "StarRpcThreadPromise.hpp"

namespace Star {

STAR_CLASS(WorldServerThread);

// Runs a WorldServer in a separate thread and guards exceptions that occur in
// it.  All methods are designed to not throw exceptions, but will instead log
// the error and trigger the WorldServerThread error state.
class WorldServerThread : public Thread {
public:
  struct Message {
    String message;
    JsonArray args;
    RpcThreadPromiseKeeper<Json> promise;
  };

  struct CommandStats {
    size_t pending;
    int64_t oldestPendingAgeMicroseconds;
    uint64_t processed;
    uint64_t direct;
    uint64_t failed;
    uint64_t waitMicroseconds;
  };

  struct ShipUpgradeApplicationResult {
    String species;
    ShipUpgrades shipUpgrades;
    Maybe<WorldChunks> shipChunks;
  };

  typedef function<void(WorldServerThread*, WorldServer*)> WorldServerAction;

  WorldServerThread(WorldServerPtr server, WorldId worldId);
  ~WorldServerThread();

  WorldId worldId() const;

  void start();
  // Signals the WorldServerThread to stop and then joins it
  void stop();
  void setPause(shared_ptr<const atomic<bool>> pause);

  // An exception occurred from the actual WorldServer itself and the
  // WorldServerThread has stopped running.
  bool serverErrorOccurred();
  bool shouldExpire();
  CommandStats commandStats() const;
  List<ServerTimingRecord> threadTimingRecords() const;
  List<ServerTimingStatus> threadTimingStatus() const;
  List<ServerTimingRecord> worldTimingRecords() const;
  List<ServerTimingStatus> worldTimingStatus() const;
  WorldServer::PacketPreparationStats packetPreparationStats() const;
  WorldServer::Phase6WorldParallelismStats phase6WorldParallelismStats() const;

  void setWorldPause(bool pause);

  bool spawnTargetValid(SpawnTarget const& spawnTarget);

  bool addClient(ConnectionId clientId, SpawnTarget const& spawnTarget, bool isLocal, bool isAdmin = false, NetCompatibilityRules netRules = {});
  // Returns final outgoing packets
  List<PacketPtr> removeClient(ConnectionId clientId);
  bool executeForClient(ConnectionId clientId, function<void(WorldServer*, PlayerPtr)> action);

  List<ConnectionId> clients() const;
  bool hasClient(ConnectionId clientId) const;
  bool noClients() const;

  // Clients that have caused an error with incoming packets are removed from
  // the world and no further packets are handled from them.  They are still
  // added to this WorldServerThread, and must be removed and the final
  // outgoing packets should be sent to them.
  List<ConnectionId> erroredClients() const;

  void pushIncomingPackets(ConnectionId clientId, List<PacketPtr> packets);
  List<PacketPtr> pullOutgoingPackets(ConnectionId clientId);

  Maybe<Vec2F> playerRevivePosition(ConnectionId clientId) const;

  // Worlds use this to notify the universe server that their celestial type should change
  Maybe<pair<String, String>> pullNewPlanetType();

  void setWeather(String const& weatherName, bool force = false);
  StringList weatherList();
  List<ItemDescriptor> containerPutItems(EntityId entityId, List<ItemDescriptor> items);
  void setUniverseFlag(String const& flagName);
  bool placeDungeon(String const& dungeonName, Vec2I const& position, Maybe<DungeonId> dungeonId = {}, bool forcePlacement = true);
  void startFlyingSky(bool enterHyperspace, bool startInWarp, Json settings = {});
  void stopFlyingSkyAt(SkyParameters const& destination);
  ShipUpgradeApplicationResult applyShipUpgrades(String fallbackSpecies, ShipUpgrades shipUpgrades, StringMap<StringList> const& speciesShips);

  // Executes the given action on the world in a thread safe context.  This
  // does *not* catch exceptions thrown by the action or set the server error
  // flag.
  void executeAction(WorldServerAction action);

  // If a callback is set here, then this is called after every world update,
  // also in a thread safe context.
  void setUpdateAction(WorldServerAction updateAction);

  // 
  void passMessages(List<Message>&& messages);

  void unloadAll(bool force = false);

  // Syncs all active sectors to disk and reads the full content of the world
  // into memory, useful for the ship.
  WorldChunks readChunks();

protected:
  virtual void run();

private:
  struct CommandState {
    Mutex mutex;
    ConditionVariable condition;
    bool finished = false;
    bool failed = false;
    String error;
  };

  struct Command {
    String name;
    WorldServerAction action;
    shared_ptr<CommandState> state;
    int64_t queuedAt;
  };

  enum class ThreadTimingPhase : uint8_t {
    Loop,
    ProcessCommands,
    IncomingPackets,
    WorldUpdate,
    Messages,
    OutgoingPackets,
    UpdateAction,
    Sync,
    Count
  };

  void executeCommand(String const& name, WorldServerAction action);
  void processCommands();
  void failPendingCommands(String const& error);
  static char const* threadTimingPhaseName(ThreadTimingPhase phase);
  void recordThreadTiming(ThreadTimingPhase phase, int64_t durationMicroseconds);

  void update(WorldServerFidelity fidelity);
  void sync();

  mutable RecursiveMutex m_mutex;

  HashSet<ConnectionId> m_clients;

  WorldServerPtr m_worldServer;
  WorldId m_worldId;
  WorldServerAction m_updateAction;

  mutable RecursiveMutex m_queueMutex;
  Map<ConnectionId, List<PacketPtr>> m_incomingPacketQueue;
  Map<ConnectionId, List<PacketPtr>> m_outgoingPacketQueue;

  mutable RecursiveMutex m_messageMutex;
  List<Message> m_messages;

  mutable Mutex m_commandMutex;
  List<Command> m_commandQueue;
  atomic<uint64_t> m_commandsProcessed{0};
  atomic<uint64_t> m_commandsProcessedDirect{0};
  atomic<uint64_t> m_commandsFailed{0};
  atomic<uint64_t> m_commandWaitMicroseconds{0};

  mutable Mutex m_threadTimingsMutex;
  List<ServerTimingAccumulator> m_threadTimings;

  atomic<bool> m_stop;
  shared_ptr<const atomic<bool>> m_pause;
  mutable atomic<bool> m_errorOccurred;
  mutable atomic<bool> m_shouldExpire;
};

}

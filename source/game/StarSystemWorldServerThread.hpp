#pragma once

#include "StarSystemWorldServer.hpp"
#include "StarThread.hpp"
#include "StarNetPackets.hpp"
#include "StarVersioningDatabase.hpp"

namespace Star {

STAR_CLASS(SystemWorldServerThread);

typedef function<void(SystemClientShip*)> ClientShipAction;

class SystemWorldServerThread : public Thread {
public:
  struct CommandStats {
    size_t pending;
    int64_t oldestPendingAgeMicroseconds;
    uint64_t processed;
    uint64_t direct;
    uint64_t failed;
    uint64_t waitMicroseconds;
  };

  SystemWorldServerThread(Vec3I const& location, SystemWorldServerPtr systemWorld, String storageFile);
  ~SystemWorldServerThread();

  Vec3I location() const;

  List<ConnectionId> clients();
  void addClient(ConnectionId clientId, Uuid const& uuid, float shipSpeed, SystemLocation const& location);
  void removeClient(ConnectionId clientId);

  void setPause(shared_ptr<const atomic<bool>> pause);
  void run() override;
  void stop();

  void update();
  CommandStats commandStats() const;
  SystemWorldServer::PacketStats packetStats();

  void setClientDestination(ConnectionId clientId, SystemLocation const& location);
  void executeClientShipAction(ConnectionId clientId, ClientShipAction action);

  SystemLocation clientShipLocation(ConnectionId clientId);
  Maybe<pair<WarpAction, WarpMode>> clientWarpAction(ConnectionId clientId);
  SkyParameters clientSkyParameters(ConnectionId clientId);

  List<InstanceWorldId> activeInstanceWorlds() const;

  // callback to be run after update in the server thread
  void setUpdateAction(function<void(SystemWorldServerThread*)> updateAction);
  void pushIncomingPacket(ConnectionId clientId, PacketPtr packet);
  List<PacketPtr> pullOutgoingPackets(ConnectionId clientId);

  void store();

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
    function<void()> action;
    shared_ptr<CommandState> state;
    int64_t queuedAt;
  };

  struct SystemWorldStorageSnapshot {
    Vec3I location;
    String file;
    VersionedJson store;
  };

  SystemWorldStorageSnapshot buildStorageSnapshot();
  static void writeStorageSnapshot(SystemWorldStorageSnapshot snapshot);
  void executeCommand(String const& name, function<void()> action);
  void processCommands();
  void failPendingCommands(String const& error);

  Vec3I m_systemLocation;
  SystemWorldServerPtr m_systemWorld;

  atomic<bool> m_stop{false};
  float m_periodicStorage{300.0f};
  bool m_triggerStorage{ false};
  String m_storageFile;

  shared_ptr<const atomic<bool>> m_pause;
  function<void(SystemWorldServerThread*)> m_updateAction;

  ReadersWriterMutex m_mutex;
  mutable ReadersWriterMutex m_queueMutex;
  mutable Mutex m_commandMutex;
  List<Command> m_commandQueue;
  atomic<uint64_t> m_commandsProcessed{0};
  atomic<uint64_t> m_commandsProcessedDirect{0};
  atomic<uint64_t> m_commandsFailed{0};
  atomic<uint64_t> m_commandWaitMicroseconds{0};

  HashSet<ConnectionId> m_clients;
  HashMap<ConnectionId, SystemLocation> m_clientShipDestinations;
  HashMap<ConnectionId, pair<SystemLocation, SkyParameters>> m_clientShipLocations;
  HashMap<ConnectionId, pair<WarpAction, WarpMode>> m_clientWarpActions;
  List<pair<ConnectionId, ClientShipAction>> m_clientShipActions;
  List<InstanceWorldId> m_activeInstanceWorlds;
  Map<ConnectionId, List<PacketPtr>> m_outgoingPacketQueue;
  List<pair<ConnectionId, PacketPtr>> m_incomingPacketQueue;
};


}

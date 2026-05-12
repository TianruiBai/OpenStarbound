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
  Mutex m_commandMutex;
  List<Command> m_commandQueue;

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

#include "StarSystemWorldServerThread.hpp"
#include "StarRoot.hpp"
#include "StarTickRateMonitor.hpp"
#include "StarNetPackets.hpp"
#include "StarLogging.hpp"
#include "StarTime.hpp"

namespace Star {

SystemWorldServerThread::SystemWorldServerThread(Vec3I const& location, SystemWorldServerPtr systemWorld, String storageFile)
  : Thread(strf("SystemWorldServer: {}", location))
  , m_systemLocation(location)
  , m_systemWorld(std::move(systemWorld))
  , m_storageFile(storageFile)
{
}

SystemWorldServerThread::~SystemWorldServerThread() {
  m_stop = true;
  join();
}

Vec3I SystemWorldServerThread::location() const {
  return m_systemLocation;
}

List<ConnectionId> SystemWorldServerThread::clients() {
  ReadLocker locker(m_queueMutex);
  return m_clients.values();
}

void SystemWorldServerThread::addClient(ConnectionId clientId, Uuid const& uuid, float shipSpeed, SystemLocation const& location) {
  executeCommand("addClient", [this, clientId, uuid, shipSpeed, location]() {
      m_clients.add(clientId);
      m_outgoingPacketQueue.set(clientId, List<PacketPtr>());

      m_systemWorld->addClientShip(clientId, uuid, shipSpeed, location);

      m_clientShipLocations.set(clientId, {m_systemWorld->clientShipLocation(clientId), m_systemWorld->clientSkyParameters(clientId)});
      if (auto warpAction = m_systemWorld->clientWarpAction(clientId))
        m_clientWarpActions.set(clientId, *warpAction);
    });
}

void SystemWorldServerThread::removeClient(ConnectionId clientId) {
  executeCommand("removeClient", [this, clientId]() {
      m_systemWorld->removeClientShip(clientId);
      m_clients.remove(clientId);
      m_clientShipDestinations.remove(clientId);
      m_clientShipLocations.remove(clientId);
      m_clientWarpActions.remove(clientId);
      m_outgoingPacketQueue.remove(clientId);
      eraseWhere(m_incomingPacketQueue, [clientId](pair<ConnectionId, PacketPtr> const& packet) {
          return packet.first == clientId;
        });
    });
}

void SystemWorldServerThread::setPause(shared_ptr<const atomic<bool>> pause) {
  m_pause = std::move(pause);
}

void SystemWorldServerThread::run() {
  TickRateApproacher tickApproacher(1.0 / SystemWorldTimestep, 0.5);

  while (!m_stop) {
    LogMap::set(strf("system_{}_update_rate", m_systemLocation), strf("{:4.2f}Hz", tickApproacher.rate()));

    update();

    m_periodicStorage -= 1.0 / tickApproacher.rate();
    if (m_triggerStorage || m_periodicStorage <= 0.0) {
      m_triggerStorage = false;
      m_periodicStorage = 300.0; // store every 5 minutes
      store();
    }

    tickApproacher.tick();

    double spareTime = tickApproacher.spareTime();
    uint64_t millis = floor(spareTime * 1000);
    if (spareTime > 0)
      sleepPrecise(millis);
  }

  store();
  failPendingCommands("System world server thread stopped before queued command could run");
}

void SystemWorldServerThread::stop() {
  m_stop = true;
}

void SystemWorldServerThread::executeCommand(String const& name, function<void()> action) {
  if (!isRunning() || m_stop) {
    WriteLocker queueLocker(m_queueMutex);
    WriteLocker locker(m_mutex);
    action();
    ++m_commandsProcessedDirect;
    return;
  }

  auto state = make_shared<CommandState>();
  {
    MutexLocker locker(m_commandMutex);
    m_commandQueue.append(Command{name, std::move(action), state, Time::monotonicMicroseconds()});
  }

  MutexLocker stateLocker(state->mutex);
  while (!state->finished)
    state->condition.wait(state->mutex);

  if (state->failed)
    throw StarException::format("System world server queued command failed: {}", state->error);
}

void SystemWorldServerThread::processCommands() {
  List<Command> commands;
  {
    MutexLocker locker(m_commandMutex);
    commands = take(m_commandQueue);
  }

  for (auto& command : commands) {
    bool failed = false;
    String error;
    try {
      command.action();
    } catch (std::exception const& e) {
      failed = true;
      error = printException(e, true);
      Logger::error("SystemWorldServerThread exception caught running queued command '{}': {}", command.name, error);
    }

    ++m_commandsProcessed;
    m_commandWaitMicroseconds += Time::monotonicMicroseconds() - command.queuedAt;
    if (failed)
      ++m_commandsFailed;

    {
      MutexLocker stateLocker(command.state->mutex);
      command.state->failed = failed;
      command.state->error = std::move(error);
      command.state->finished = true;
    }
    command.state->condition.broadcast();
  }
}

void SystemWorldServerThread::failPendingCommands(String const& error) {
  List<Command> commands;
  {
    MutexLocker locker(m_commandMutex);
    commands = take(m_commandQueue);
  }

  auto now = Time::monotonicMicroseconds();
  for (auto& command : commands) {
    ++m_commandsFailed;
    m_commandWaitMicroseconds += now - command.queuedAt;
    {
      MutexLocker stateLocker(command.state->mutex);
      command.state->failed = true;
      command.state->error = error;
      command.state->finished = true;
    }
    command.state->condition.broadcast();
  }
}

void SystemWorldServerThread::update() {
  WriteLocker queueLocker(m_queueMutex);
  WriteLocker locker(m_mutex);

  processCommands();

  for (auto p : take(m_incomingPacketQueue))
    m_systemWorld->handleIncomingPacket(p.first, p.second);

  for (auto p : take(m_clientShipActions))
    p.second(m_systemWorld->clientShip(p.first).get());

  if (!m_pause || *m_pause == false)
    m_systemWorld->update(SystemWorldTimestep * GlobalTimescale);
  m_triggerStorage = m_systemWorld->triggeredStorage();

  // important to set destinations before getting locations
  // setting a destination nullifies the current location
  for (auto p : take(m_clientShipDestinations))
    m_systemWorld->setClientDestination(p.first, p.second);

  m_activeInstanceWorlds = m_systemWorld->activeInstanceWorlds();

  for (auto clientId : m_clients) {
    m_outgoingPacketQueue[clientId].appendAll(m_systemWorld->pullOutgoingPackets(clientId));
    auto shipSystemLocation = m_systemWorld->clientShipLocation(clientId);
    auto& shipLocation = m_clientShipLocations[clientId];
    if (shipLocation.first != shipSystemLocation) {
      shipLocation.first = shipSystemLocation;
      shipLocation.second = m_systemWorld->clientSkyParameters(clientId);
    }
    if (auto warpAction = m_systemWorld->clientWarpAction(clientId))
      m_clientWarpActions.set(clientId, *warpAction);
    else if (m_clientWarpActions.contains(clientId))
      m_clientWarpActions.remove(clientId);
  }
  queueLocker.unlock();

  if (m_updateAction)
    m_updateAction(this);
}

SystemWorldServerThread::CommandStats SystemWorldServerThread::commandStats() const {
  CommandStats stats{};
  {
    MutexLocker locker(m_commandMutex);
    stats.pending = m_commandQueue.size();
    auto now = Time::monotonicMicroseconds();
    for (auto const& command : m_commandQueue)
      stats.oldestPendingAgeMicroseconds = max<int64_t>(stats.oldestPendingAgeMicroseconds, now - command.queuedAt);
  }
  stats.processed = m_commandsProcessed;
  stats.direct = m_commandsProcessedDirect;
  stats.failed = m_commandsFailed;
  stats.waitMicroseconds = m_commandWaitMicroseconds;
  return stats;
}

void SystemWorldServerThread::setClientDestination(ConnectionId clientId, SystemLocation const& destination) {
  WriteLocker locker(m_queueMutex);
  m_clientShipDestinations.set(clientId, destination);
}

void SystemWorldServerThread::executeClientShipAction(ConnectionId clientId, ClientShipAction action) {
  WriteLocker locker(m_queueMutex);
  m_clientShipActions.append({clientId, std::move(action)});
}

SystemLocation SystemWorldServerThread::clientShipLocation(ConnectionId clientId) {
  ReadLocker locker(m_queueMutex);
  // while a ship destination is pending the ship is assumed to be flying
  if (m_clientShipDestinations.contains(clientId))
    return {};
  return m_clientShipLocations.get(clientId).first;
}

Maybe<pair<WarpAction, WarpMode>> SystemWorldServerThread::clientWarpAction(ConnectionId clientId) {
  ReadLocker locker(m_queueMutex);
  if (m_clientShipDestinations.contains(clientId))
    return {};
  return m_clientWarpActions.maybe(clientId);
}

SkyParameters SystemWorldServerThread::clientSkyParameters(ConnectionId clientId) {
  ReadLocker locker(m_queueMutex);
  return m_clientShipLocations.get(clientId).second;
}

List<InstanceWorldId> SystemWorldServerThread::activeInstanceWorlds() const {
  ReadLocker locker(m_queueMutex);
  return m_activeInstanceWorlds;
}

void SystemWorldServerThread::setUpdateAction(function<void(SystemWorldServerThread*)> updateAction) {
  m_updateAction = updateAction;
}

void SystemWorldServerThread::pushIncomingPacket(ConnectionId clientId, PacketPtr packet) {
  WriteLocker locker(m_queueMutex);
  m_incomingPacketQueue.append({std::move(clientId), std::move(packet)});
}

List<PacketPtr> SystemWorldServerThread::pullOutgoingPackets(ConnectionId clientId) {
  WriteLocker locker(m_queueMutex);
  return take(m_outgoingPacketQueue[clientId]);
}

SystemWorldServerThread::SystemWorldStorageSnapshot SystemWorldServerThread::buildStorageSnapshot() {
  ReadLocker locker(m_mutex);
  Json store = m_systemWorld->diskStore();
  locker.unlock();

  auto versioningDatabase = Root::singleton().versioningDatabase();
  return {m_systemLocation, m_storageFile, versioningDatabase->makeCurrentVersionedJson("System", std::move(store))};
}

void SystemWorldServerThread::writeStorageSnapshot(SystemWorldStorageSnapshot snapshot) {
  Logger::debug("Trigger disk storage for system world {}:{}:{}", snapshot.location.x(), snapshot.location.y(), snapshot.location.z());
  VersionedJson::writeFile(snapshot.store, snapshot.file);
}

void SystemWorldServerThread::store() {
  writeStorageSnapshot(buildStorageSnapshot());
}

}

#include "StarWorldServerThread.hpp"
#include "StarTickRateMonitor.hpp"
#include "StarNpc.hpp"
#include "StarRoot.hpp"
#include "StarLogging.hpp"
#include "StarAssets.hpp"
#include "StarPlayer.hpp"
#include "StarTime.hpp"

namespace Star {

WorldServerThread::WorldServerThread(WorldServerPtr server, WorldId worldId)
  : Thread("WorldServerThread: " + printWorldId(worldId)),
    m_worldServer(std::move(server)),
    m_worldId(std::move(worldId)),
    m_stop(false),
    m_errorOccurred(false),
    m_shouldExpire(true) {
  if (m_worldServer)
    m_worldServer->setWorldId(printWorldId(m_worldId));
}

WorldServerThread::~WorldServerThread() {
  m_stop = true;
  join();

  RecursiveMutexLocker locker(m_mutex);
  for (auto clientId : m_worldServer->clientIds())
    removeClient(clientId);
}

WorldId WorldServerThread::worldId() const {
  return m_worldId;
}

void WorldServerThread::start() {
  m_stop = false;
  m_errorOccurred = false;
  Thread::start();
}

void WorldServerThread::stop() {
  m_stop = true;
  Thread::join();
}

void WorldServerThread::setPause(shared_ptr<const atomic<bool>> pause) {
  m_pause = pause;
}

bool WorldServerThread::serverErrorOccurred() {
  return m_errorOccurred;
}

bool WorldServerThread::shouldExpire() {
  return m_shouldExpire;
}

WorldServerThread::CommandStats WorldServerThread::commandStats() const {
  CommandStats stats{};
  {
    MutexLocker locker(m_commandMutex);
    stats.pending = m_commandQueue.size();
  }
  stats.processed = m_commandsProcessed;
  stats.direct = m_commandsProcessedDirect;
  stats.failed = m_commandsFailed;
  stats.waitMicroseconds = m_commandWaitMicroseconds;
  return stats;
}

void WorldServerThread::executeCommand(String const& name, WorldServerAction action) {
  if (!isRunning() || m_stop || m_errorOccurred) {
    RecursiveMutexLocker locker(m_mutex);
    action(this, m_worldServer.get());
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
    throw StarException::format("World server queued command failed: {}", state->error);
}

void WorldServerThread::processCommands() {
  List<Command> commands;
  {
    MutexLocker locker(m_commandMutex);
    commands = take(m_commandQueue);
  }

  for (auto& command : commands) {
    bool failed = false;
    String error;
    try {
      command.action(this, m_worldServer.get());
    } catch (std::exception const& e) {
      failed = true;
      error = printException(e, true);
      Logger::error("WorldServerThread exception caught running queued command '{}': {}", command.name, error);
      m_errorOccurred = true;
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

void WorldServerThread::failPendingCommands(String const& error) {
  List<Command> commands;
  {
    MutexLocker locker(m_commandMutex);
    commands = take(m_commandQueue);
  }

  for (auto& command : commands) {
    ++m_commandsFailed;
    {
      MutexLocker stateLocker(command.state->mutex);
      command.state->failed = true;
      command.state->error = error;
      command.state->finished = true;
    }
    command.state->condition.broadcast();
  }
}

bool WorldServerThread::spawnTargetValid(SpawnTarget const& spawnTarget) {
  try {
    bool result = false;
    executeCommand("spawnTargetValid", [&spawnTarget, &result](WorldServerThread*, WorldServer* worldServer) {
        result = worldServer->spawnTargetValid(spawnTarget);
      });
    return result;
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
    return false;
  }
}

bool WorldServerThread::addClient(ConnectionId clientId, SpawnTarget const& spawnTarget, bool isLocal, bool isAdmin, NetCompatibilityRules netRules) {
  try {
    bool added = false;
    executeCommand("addClient", [this, clientId, spawnTarget, isLocal, isAdmin, netRules, &added](WorldServerThread*, WorldServer* worldServer) {
        if (worldServer->addClient(clientId, spawnTarget, isLocal, isAdmin, netRules)) {
          m_clients.add(clientId);
          added = true;
        }
      });
    return added;
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
    return false;
  }
}

List<PacketPtr> WorldServerThread::removeClient(ConnectionId clientId) {
  List<PacketPtr> outgoingPackets;
  try {
    executeCommand("removeClient", [this, clientId, &outgoingPackets](WorldServerThread*, WorldServer* worldServer) {
        if (!m_clients.contains(clientId))
          return;

        RecursiveMutexLocker queueLocker(m_queueMutex);
        try {
          auto incomingPackets = take(m_incomingPacketQueue[clientId]);
          if (worldServer->hasClient(clientId))
            worldServer->handleIncomingPackets(clientId, std::move(incomingPackets));

          outgoingPackets = take(m_outgoingPacketQueue[clientId]);
          if (worldServer->hasClient(clientId))
            outgoingPackets.appendAll(worldServer->removeClient(clientId));

        } catch (std::exception const& e) {
          Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
          m_errorOccurred = true;
        }

        m_clients.remove(clientId);
        m_incomingPacketQueue.remove(clientId);
        m_outgoingPacketQueue.remove(clientId);
      });

  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
  }
  return outgoingPackets;
}

List<ConnectionId> WorldServerThread::clients() const {
  RecursiveMutexLocker locker(m_mutex);
  return m_clients.values();
}

bool WorldServerThread::hasClient(ConnectionId clientId) const {
  RecursiveMutexLocker locker(m_mutex);
  return m_clients.contains(clientId);
}

bool WorldServerThread::noClients() const {
  RecursiveMutexLocker locker(m_mutex);
  return m_clients.empty();
}


List<ConnectionId> WorldServerThread::erroredClients() const {
  RecursiveMutexLocker locker(m_mutex);
  auto unerroredClients = HashSet<ConnectionId>::from(m_worldServer->clientIds());
  return m_clients.difference(unerroredClients).values();
}

void WorldServerThread::pushIncomingPackets(ConnectionId clientId, List<PacketPtr> packets) {
  RecursiveMutexLocker queueLocker(m_queueMutex);
  m_incomingPacketQueue[clientId].appendAll(std::move(packets));
}

List<PacketPtr> WorldServerThread::pullOutgoingPackets(ConnectionId clientId) {
  RecursiveMutexLocker queueLocker(m_queueMutex);
  return take(m_outgoingPacketQueue[clientId]);
}

Maybe<Vec2F> WorldServerThread::playerRevivePosition(ConnectionId clientId) const {
  try {
    Maybe<Vec2F> result;
    const_cast<WorldServerThread*>(this)->executeCommand("playerRevivePosition", [clientId, &result](WorldServerThread*, WorldServer* worldServer) {
        if (auto player = worldServer->clientPlayer(clientId))
          result = player->position() + player->feetOffset();
      });
    return result;
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
    return {};
  }
}

Maybe<pair<String, String>> WorldServerThread::pullNewPlanetType() {
  try {
    Maybe<pair<String, String>> result;
    executeCommand("pullNewPlanetType", [&result](WorldServerThread*, WorldServer* worldServer) {
        result = worldServer->pullNewPlanetType();
      });
    return result;
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
    return {};
  }
}

void WorldServerThread::executeAction(WorldServerAction action) {
  RecursiveMutexLocker locker(m_mutex);
  action(this, m_worldServer.get());
}

void WorldServerThread::setUpdateAction(WorldServerAction updateAction) {
  RecursiveMutexLocker locker(m_mutex);
  m_updateAction = updateAction;
}

void WorldServerThread::passMessages(List<Message>&& messages) {
  RecursiveMutexLocker locker(m_messageMutex);
  m_messages.appendAll(std::move(messages));
}

void WorldServerThread::unloadAll(bool force) {
  try {
    executeCommand("unloadAll", [force](WorldServerThread*, WorldServer* worldServer) {
        worldServer->unloadAll(force);
      });
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
  }
}

WorldChunks WorldServerThread::readChunks() {
  try {
    WorldChunks chunks;
    executeCommand("readChunks", [&chunks](WorldServerThread*, WorldServer* worldServer) {
        chunks = worldServer->readChunks();
      });
    return chunks;
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
    return {};
  }
}

void WorldServerThread::run() {
  try {
    auto& root = Root::singleton();
    double updateMeasureWindow = root.assets()->json("/universe_server.config:updateMeasureWindow").toDouble();
    double fidelityDecrementScore = root.assets()->json("/universe_server.config:fidelityDecrementScore").toDouble();
    double fidelityIncrementScore = root.assets()->json("/universe_server.config:fidelityIncrementScore").toDouble();

    String serverFidelityMode = root.configuration()->get("serverFidelity").toString();
    Maybe<WorldServerFidelity> lockedFidelity;
    if (!serverFidelityMode.equalsIgnoreCase("automatic"))
      lockedFidelity = WorldServerFidelityNames.getLeft(serverFidelityMode);

    double storageInterval = root.assets()->json("/universe_server.config:worldStorageInterval").toDouble() / 1000.0;
    Timer storageTimer = Timer::withTime(storageInterval);

    TickRateApproacher tickApproacher(1.0f / ServerGlobalTimestep, updateMeasureWindow);
    double fidelityScore = 0.0;
    WorldServerFidelity automaticFidelity = WorldServerFidelity::Medium;

    while (!m_stop && !m_errorOccurred) {
      auto fidelity = lockedFidelity.value(automaticFidelity);
      LogMap::set(strf("server_{}_fidelity", m_worldId), WorldServerFidelityNames.getRight(fidelity));
      LogMap::set(strf("server_{}_update", m_worldId), strf("{:4.2f}Hz", tickApproacher.rate()));

      update(fidelity);
      tickApproacher.setTargetTickRate(1.0f / ServerGlobalTimestep);
      tickApproacher.tick();

      if (storageTimer.timeUp()) {
        sync();
        storageTimer.restart(storageInterval);
      }

      double spareTime = tickApproacher.spareTime();
      fidelityScore += spareTime;

      if (fidelityScore <= fidelityDecrementScore) {
        if (automaticFidelity > WorldServerFidelity::Minimum)
          automaticFidelity = (WorldServerFidelity)((int)automaticFidelity - 1);
        fidelityScore = 0.0;
      }

      if (fidelityScore >= fidelityIncrementScore) {
        if (automaticFidelity < WorldServerFidelity::High)
          automaticFidelity = (WorldServerFidelity)((int)automaticFidelity + 1);
        fidelityScore = 0.0;
      }

      int64_t spareMilliseconds = floor(spareTime * 1000);
      if (spareMilliseconds > 0)
        Thread::sleepPrecise(spareMilliseconds);
    }
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
  }

  failPendingCommands("World server thread stopped before queued command could run");
}

void WorldServerThread::update(WorldServerFidelity fidelity) {
  RecursiveMutexLocker locker(m_mutex);
  processCommands();

  auto unerroredClientIds = m_worldServer->clientIds();
  for (auto clientId : unerroredClientIds) {
    RecursiveMutexLocker queueLocker(m_queueMutex);
    auto incomingPackets = take(m_incomingPacketQueue[clientId]);
    queueLocker.unlock();
    try {
      m_worldServer->handleIncomingPackets(clientId, std::move(incomingPackets));
    } catch (std::exception const& e) {
      Logger::error("WorldServerThread exception caught handling incoming packets for client {}: {}",
          clientId, outputException(e, true));
      RecursiveMutexLocker queueLocker(m_queueMutex);
      m_outgoingPacketQueue[clientId].appendAll(m_worldServer->removeClient(clientId));
      unerroredClientIds.remove(clientId);
    }
  }

  float dt = ServerGlobalTimestep * GlobalTimescale;
  m_worldServer->setFidelity(fidelity);
  if (dt > 0.0f && (!m_pause || *m_pause == false))
    m_worldServer->update(dt);

  List<Message> messages;
  {
    RecursiveMutexLocker locker(m_messageMutex);
    messages = std::move(m_messages);
  }
  for (auto& message : messages) {
    if (auto resp = m_worldServer->receiveMessage(ServerConnectionId, message.message, message.args))
      message.promise.fulfill(*resp);
    else
      message.promise.fail("Message not handled by world");
  }

  for (auto& clientId : unerroredClientIds) {
    auto outgoingPackets = m_worldServer->getOutgoingPackets(clientId);
    RecursiveMutexLocker queueLocker(m_queueMutex);
    m_outgoingPacketQueue[clientId].appendAll(std::move(outgoingPackets));
  }

  m_shouldExpire = m_worldServer->shouldExpire();

  if (m_updateAction)
    m_updateAction(this, m_worldServer.get());
}

void WorldServerThread::sync() {
  RecursiveMutexLocker locker(m_mutex);
  Logger::debug("WorldServer: periodic sync to disk of world {}", m_worldId);
  m_worldServer->sync();
}

}

#include "StarSocketPoller.hpp"
#include "StarTime.hpp"

namespace Star {

void SocketPoller::registerSocket(SocketPtr socket, Interest interest) {
  MutexLocker locker(m_mutex);
  m_query.set(std::move(socket), {interest.readable, interest.writable});
  m_condition.broadcast();
}

void SocketPoller::updateSocket(SocketPtr const& socket, Interest interest) {
  MutexLocker locker(m_mutex);
  if (m_query.contains(socket))
    m_query.set(socket, {interest.readable, interest.writable});
  m_condition.broadcast();
}

void SocketPoller::unregisterSocket(SocketPtr const& socket) {
  MutexLocker locker(m_mutex);
  m_query.remove(socket);
  m_condition.broadcast();
}

void SocketPoller::wake() {
  MutexLocker locker(m_mutex);
  m_woken = true;
  m_condition.broadcast();
}

List<SocketPoller::ReadySocket> SocketPoller::poll(unsigned timeoutMillis) {
  auto deadline = Time::monotonicMilliseconds() + timeoutMillis;

  while (true) {
    SocketPollQuery query;
    {
      MutexLocker locker(m_mutex);
      if (m_woken) {
        m_woken = false;
        return {};
      }

      if (m_query.empty()) {
        m_condition.wait(m_mutex, timeoutMillis);
        if (m_woken)
          m_woken = false;
        return {};
      }

      query = m_query;
    }

    int64_t now = Time::monotonicMilliseconds();
    if (timeoutMillis != 0 && now >= deadline)
      return {};

    unsigned pollTimeout = 0;
    if (timeoutMillis != 0) {
      auto remaining = deadline - now;
      pollTimeout = static_cast<unsigned>(remaining < PollWakeQuantumMillis ? remaining : PollWakeQuantumMillis);
    }

    if (auto pollResult = Socket::poll(query, pollTimeout)) {
      List<ReadySocket> ready;
      for (auto const& pair : *pollResult)
        ready.append({pair.first, pair.second.readable, pair.second.writable, pair.second.exception});
      return ready;
    }

    if (timeoutMillis == 0)
      return {};
  }
}

size_t SocketPoller::registeredSockets() const {
  MutexLocker locker(m_mutex);
  return m_query.size();
}

}

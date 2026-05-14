#pragma once

#include "StarList.hpp"
#include "StarSocket.hpp"

namespace Star {

STAR_CLASS(SocketPoller);

class SocketPoller {
public:
  struct Interest {
    bool readable = false;
    bool writable = false;
  };

  struct ReadySocket {
    SocketPtr socket;
    bool readable = false;
    bool writable = false;
    bool exception = false;
  };

  void registerSocket(SocketPtr socket, Interest interest);
  void updateSocket(SocketPtr const& socket, Interest interest);
  void unregisterSocket(SocketPtr const& socket);
  void wake();

  List<ReadySocket> poll(unsigned timeoutMillis);
  size_t registeredSockets() const;

private:
  static unsigned const PollWakeQuantumMillis = 10;

  mutable Mutex m_mutex;
  ConditionVariable m_condition;
  SocketPollQuery m_query;
  bool m_woken = false;
};

}

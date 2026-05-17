#include "StarThread.hpp"
#include <ctime>

namespace Star {

// STUB: Phase 1 N3DS threading is single-threaded placeholder behavior.
struct ThreadImpl {
  ThreadImpl(std::function<void()> threadFunction, String name)
    : threadFunction(std::move(threadFunction)), name(std::move(name)), stopped(true), joined(true) {}

  bool start() {
    if (!joined)
      return false;

    joined = false;
    stopped = false;

    try {
      threadFunction();
    } catch (...) {
      // STUB: placeholder thread execution captures no logging on N3DS phase 1.
    }

    stopped = true;
    return true;
  }

  bool join() {
    if (joined)
      return false;

    joined = true;
    return true;
  }

  std::function<void()> threadFunction;
  String name;
  bool stopped;
  bool joined;
};

struct ThreadFunctionImpl : ThreadImpl {
  ThreadFunctionImpl(std::function<void()> threadFunction, String name)
    : ThreadImpl(wrapFunction(std::move(threadFunction)), std::move(name)) {}

  std::function<void()> wrapFunction(std::function<void()> threadFunction) {
    return [threadFunction = std::move(threadFunction), this]() {
      try {
        threadFunction();
      } catch (...) {
        exception = std::current_exception();
      }
    };
  }

  std::exception_ptr exception;
};

struct MutexImpl {
  bool locked = false;
};

struct ConditionVariableImpl {
  void wait(Mutex&, unsigned millis) {
    if (millis > 0)
      Thread::sleep(millis);
    else
      Thread::yield();
  }

  void wait(Mutex&) {
    Thread::yield();
  }

  void signal() {}
  void broadcast() {}
};

struct RecursiveMutexImpl {
  unsigned lockCount = 0;
};

void Thread::sleepPrecise(unsigned msecs) {
  sleep(msecs);
}

void Thread::sleep(unsigned msecs) {
  // PLACEHOLDER: busy-wait fallback until native N3DS sleep primitive is wired.
  clock_t start = std::clock();
  clock_t duration = (CLOCKS_PER_SEC * static_cast<clock_t>(msecs)) / 1000;
  while ((std::clock() - start) < duration)
    ;
}

void Thread::yield() {
  // PLACEHOLDER: no scheduler hint available in phase 1 stub.
}

unsigned Thread::numberOfProcessors() {
  // PLACEHOLDER: Start single-core scheduling assumptions for phase 1.
  return 1;
}

Thread::Thread(String const& name) {
  m_impl.reset(new ThreadImpl([this]() {
      run();
    }, name));
}

Thread::Thread(Thread&&) = default;

Thread::~Thread() {}

Thread& Thread::operator=(Thread&&) = default;

bool Thread::start() {
  return m_impl->start();
}

bool Thread::join() {
  return m_impl->join();
}

String Thread::name() {
  return m_impl->name;
}

bool Thread::isJoined() const {
  return m_impl->joined;
}

bool Thread::isRunning() const {
  return !m_impl->stopped;
}

ThreadFunction<void>::ThreadFunction() {}

ThreadFunction<void>::ThreadFunction(ThreadFunction&&) = default;

ThreadFunction<void>::ThreadFunction(function<void()> function, String const& name) {
  m_impl.reset(new ThreadFunctionImpl(std::move(function), name));
  m_impl->start();
}

ThreadFunction<void>::~ThreadFunction() {
  finish();
}

ThreadFunction<void>& ThreadFunction<void>::operator=(ThreadFunction&&) = default;

void ThreadFunction<void>::finish() {
  if (m_impl) {
    m_impl->join();

    if (m_impl->exception)
      std::rethrow_exception(take(m_impl->exception));
  }
}

bool ThreadFunction<void>::isFinished() const {
  return !m_impl || m_impl->joined;
}

bool ThreadFunction<void>::isRunning() const {
  return m_impl && !m_impl->stopped;
}

ThreadFunction<void>::operator bool() const {
  return !isFinished();
}

String ThreadFunction<void>::name() {
  if (m_impl)
    return m_impl->name;
  else
    return "";
}

Mutex::Mutex()
  : m_impl(new MutexImpl()) {}

Mutex::Mutex(Mutex&&) = default;

Mutex::~Mutex() {}

Mutex& Mutex::operator=(Mutex&&) = default;

void Mutex::lock() {
  m_impl->locked = true;
}

bool Mutex::tryLock() {
  if (m_impl->locked)
    return false;

  m_impl->locked = true;
  return true;
}

void Mutex::unlock() {
  m_impl->locked = false;
}

ConditionVariable::ConditionVariable()
  : m_impl(new ConditionVariableImpl()) {}

ConditionVariable::ConditionVariable(ConditionVariable&&) = default;

ConditionVariable::~ConditionVariable() {}

ConditionVariable& ConditionVariable::operator=(ConditionVariable&&) = default;

void ConditionVariable::wait(Mutex& mutex, Maybe<unsigned> millis) {
  if (millis)
    m_impl->wait(mutex, *millis);
  else
    m_impl->wait(mutex);
}

void ConditionVariable::signal() {
  m_impl->signal();
}

void ConditionVariable::broadcast() {
  m_impl->broadcast();
}

RecursiveMutex::RecursiveMutex()
  : m_impl(new RecursiveMutexImpl()) {}

RecursiveMutex::RecursiveMutex(RecursiveMutex&&) = default;

RecursiveMutex::~RecursiveMutex() {}

RecursiveMutex& RecursiveMutex::operator=(RecursiveMutex&&) = default;

void RecursiveMutex::lock() {
  ++m_impl->lockCount;
}

bool RecursiveMutex::tryLock() {
  ++m_impl->lockCount;
  return true;
}

void RecursiveMutex::unlock() {
  if (m_impl->lockCount > 0)
    --m_impl->lockCount;
}

}

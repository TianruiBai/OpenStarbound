#include "StarSignalHandler.hpp"

namespace Star {

struct SignalHandlerImpl {
  bool handleFatal = false;
  bool handleInterrupt = false;
  bool interrupted = false;
};

SignalHandlerImplUPtr SignalHandler::s_singleton;

SignalHandler::SignalHandler() {
  if (!s_singleton)
    s_singleton.reset(new SignalHandlerImpl());
}

SignalHandler::~SignalHandler() {}

void SignalHandler::setHandleFatal(bool handleFatal) {
  if (s_singleton)
    s_singleton->handleFatal = handleFatal;
}

bool SignalHandler::handlingFatal() const {
  return s_singleton && s_singleton->handleFatal;
}

void SignalHandler::setHandleInterrupt(bool handleInterrupt) {
  if (s_singleton)
    s_singleton->handleInterrupt = handleInterrupt;
}

bool SignalHandler::handlingInterrupt() const {
  return s_singleton && s_singleton->handleInterrupt;
}

bool SignalHandler::interruptCaught() const {
  // STUB: no real signal/interrupt wiring exists for N3DS phase 1.
  return s_singleton && s_singleton->interrupted;
}

}

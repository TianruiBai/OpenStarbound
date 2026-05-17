#include "StarException.hpp"
#include "StarCasting.hpp"

#include <cstdlib>

namespace Star {

StarException::StarException() noexcept
  : StarException(std::string("StarException"), false) {}

StarException::~StarException() noexcept {}

StarException::StarException(std::string message, bool genStackTrace) noexcept
  : StarException("StarException", std::move(message), genStackTrace) {}

StarException::StarException(std::exception const& cause) noexcept
  : StarException("StarException", std::string(), cause) {}

StarException::StarException(std::string message, std::exception const& cause) noexcept
  : StarException("StarException", std::move(message), cause) {}

const char* StarException::what() const noexcept {
  if (m_whatBuffer.empty()) {
    std::ostringstream os;
    m_printException(os, false);
    m_whatBuffer = os.str();
  }

  return m_whatBuffer.c_str();
}

StarException::StarException(char const* type, std::string message, bool) noexcept {
  // STUB: stacktrace capture is a placeholder until N3DS diagnostics backend exists.
  std::string typeString = type ? type : "StarException";
  m_printException = [typeString = std::move(typeString), message = std::move(message)](std::ostream& os, bool) {
    os << "(" << typeString << ")";
    if (!message.empty())
      os << " " << message;
  };
}

StarException::StarException(char const* type, std::string message, std::exception const& cause) noexcept
  : StarException(type, std::move(message), false) {
  auto printException = [](std::ostream& os, bool fullStacktrace, function<void(std::ostream&, bool)> self, function<void(std::ostream&, bool)> causeFn) {
    self(os, fullStacktrace);
    os << std::endl << "Caused by: ";
    causeFn(os, fullStacktrace);
  };

  function<void(std::ostream&, bool)> printCause;
  if (auto starException = as<StarException>(&cause)) {
    printCause = bind(starException->m_printException, _1, _2);
  } else {
    printCause = bind([](std::ostream& os, bool, std::string causeWhat) {
      os << "std::exception: " << causeWhat;
    }, _1, _2, std::string(cause.what()));
  }

  m_printException = bind(printException, _1, _2, m_printException, std::move(printCause));
}

void printException(std::ostream& os, std::exception const& e, bool fullStacktrace) {
  if (auto starException = as<StarException>(&e))
    starException->m_printException(os, fullStacktrace);
  else
    os << "std::exception: " << e.what();
}

std::string printException(std::exception const& e, bool fullStacktrace) {
  std::ostringstream os;
  printException(os, e, fullStacktrace);
  return os.str();
}

OutputProxy outputException(std::exception const& e, bool fullStacktrace) {
  if (auto starException = as<StarException>(&e))
    return OutputProxy(bind(starException->m_printException, _1, fullStacktrace));
  else
    return OutputProxy(bind([](std::ostream& os, std::string what) { os << "std::exception: " << what; }, _1, std::string(e.what())));
}

void printStack(char const*) {
  // PLACEHOLDER: no N3DS stackwalker wired in phase 1.
}

void fatalError(char const* message, bool) {
  (void)message;
  std::abort();
}

void fatalException(std::exception const&, bool) {
  std::abort();
}

}

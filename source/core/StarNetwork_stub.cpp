#include "StarHostAddress.hpp"
#include "StarSocket.hpp"
#include "StarTcp.hpp"
#include "StarUdp.hpp"
#include "StarHash.hpp"

#include <tuple>

namespace Star {

namespace {

HostAddressWithPort makeUnsupportedAddress(NetworkMode mode) {
  uint8_t ipv4[4] = {0, 0, 0, 0};
  uint8_t ipv6[16] = {0};
  if (mode == NetworkMode::IPv6)
    return HostAddressWithPort(mode, ipv6, 0);
  return HostAddressWithPort(mode, ipv4, 0);
}

}

HostAddress HostAddress::localhost(NetworkMode mode) {
  if (mode == NetworkMode::IPv4) {
    uint8_t addr[4] = {127, 0, 0, 1};
    return HostAddress(mode, addr);
  }

  uint8_t addr[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  return HostAddress(mode, addr);
}

Either<String, HostAddress> HostAddress::lookup(String const&) {
  // STUB: Nintendo 3DS phase1 networking name lookup is not implemented yet.
  return makeLeft(String("Networking hostname lookup is unavailable on STAR_PLATFORM_N3DS placeholder build"));
}

HostAddress::HostAddress(NetworkMode mode, uint8_t* address) {
  set(mode, address);
}

HostAddress::HostAddress(String const& address) {
  auto result = lookup(address);
  if (result.isLeft())
    throw NetworkException(result.left().takeUtf8());
  *this = std::move(result.right());
}

NetworkMode HostAddress::mode() const {
  return m_mode;
}

uint8_t const* HostAddress::bytes() const {
  return m_address;
}

uint8_t HostAddress::octet(size_t i) const {
  return m_address[i];
}

size_t HostAddress::size() const {
  return m_mode == NetworkMode::IPv4 ? 4 : 16;
}

bool HostAddress::isLocalHost() const {
  if (m_mode == NetworkMode::IPv4)
    return m_address[0] == 127 && m_address[1] == 0 && m_address[2] == 0 && m_address[3] == 1;

  for (size_t i = 0; i < 15; ++i) {
    if (m_address[i] != 0)
      return false;
  }

  return m_address[15] == 1;
}

bool HostAddress::isZero() const {
  for (size_t i = 0; i < size(); ++i) {
    if (m_address[i] != 0)
      return false;
  }
  return true;
}

bool HostAddress::operator==(HostAddress const& rhs) const {
  if (m_mode != rhs.m_mode)
    return false;

  for (size_t i = 0; i < size(); ++i) {
    if (m_address[i] != rhs.m_address[i])
      return false;
  }

  return true;
}

void HostAddress::set(String const& address) {
  auto result = lookup(address);
  if (result.isLeft())
    throw NetworkException(result.left().takeUtf8());
  *this = std::move(result.right());
}

void HostAddress::set(NetworkMode mode, uint8_t const* addr) {
  m_mode = mode;
  size_t len = size();
  for (size_t i = 0; i < len; ++i)
    m_address[i] = addr ? addr[i] : 0;
}

std::ostream& operator<<(std::ostream& os, HostAddress const& address) {
  if (address.mode() == NetworkMode::IPv4) {
    format(os, "{}.{}.{}.{}", address.octet(0), address.octet(1), address.octet(2), address.octet(3));
    return os;
  }

  format(os,
      "{:02x}{:02x}:{:02x}{:02x}:{:02x}{:02x}:{:02x}{:02x}:{:02x}{:02x}:{:02x}{:02x}:{:02x}{:02x}:{:02x}{:02x}",
      address.octet(0),
      address.octet(1),
      address.octet(2),
      address.octet(3),
      address.octet(4),
      address.octet(5),
      address.octet(6),
      address.octet(7),
      address.octet(8),
      address.octet(9),
      address.octet(10),
      address.octet(11),
      address.octet(12),
      address.octet(13),
      address.octet(14),
      address.octet(15));

  return os;
}

size_t hash<HostAddress>::operator()(HostAddress const& address) const {
  PLHasher hasher;
  for (size_t i = 0; i < address.size(); ++i)
    hasher.put(address.octet(i));
  return hasher.hash();
}

Either<String, HostAddressWithPort> HostAddressWithPort::lookup(String const& address, uint16_t port) {
  auto hostAddress = HostAddress::lookup(address);
  if (hostAddress.isLeft())
    return makeLeft(std::move(hostAddress.left()));
  return makeRight(HostAddressWithPort(std::move(hostAddress.right()), port));
}

Either<String, HostAddressWithPort> HostAddressWithPort::lookupWithPort(String const&) {
  // PLACEHOLDER: STAR_PLATFORM_N3DS phase1 does not parse endpoint text yet.
  return makeLeft(String("Networking endpoint parsing is unavailable on STAR_PLATFORM_N3DS placeholder build"));
}

HostAddressWithPort::HostAddressWithPort() : m_port(0) {}

HostAddressWithPort::HostAddressWithPort(HostAddress const& address, uint16_t port)
  : m_address(address), m_port(port) {}

HostAddressWithPort::HostAddressWithPort(NetworkMode mode, uint8_t* address, uint16_t port) {
  m_address = HostAddress(mode, address);
  m_port = port;
}

HostAddressWithPort::HostAddressWithPort(String const& address, uint16_t port) {
  auto result = lookup(address, port);
  if (result.isLeft())
    throw NetworkException(result.left().takeUtf8());
  *this = std::move(result.right());
}

HostAddressWithPort::HostAddressWithPort(String const& address) {
  auto result = lookupWithPort(address);
  if (result.isLeft())
    throw NetworkException(result.left().takeUtf8());
  *this = std::move(result.right());
}

HostAddress HostAddressWithPort::address() const {
  return m_address;
}

uint16_t HostAddressWithPort::port() const {
  return m_port;
}

bool HostAddressWithPort::operator==(HostAddressWithPort const& rhs) const {
  return std::tie(m_address, m_port) == std::tie(rhs.m_address, rhs.m_port);
}

std::ostream& operator<<(std::ostream& os, HostAddressWithPort const& addressWithPort) {
  os << addressWithPort.address() << ":" << addressWithPort.port();
  return os;
}

size_t hash<HostAddressWithPort>::operator()(HostAddressWithPort const& addressWithPort) const {
  return hashOf(addressWithPort.address(), addressWithPort.port());
}

Maybe<SocketPollResult> Socket::poll(SocketPollQuery const&, unsigned) {
  // PLACEHOLDER: no socket polling support in Nintendo 3DS phase1 build.
  return {};
}

Socket::~Socket() {
  close();
}

void Socket::bind(HostAddressWithPort const&) {
  throw NetworkException("Socket bind is unavailable on STAR_PLATFORM_N3DS placeholder build");
}

void Socket::listen(int) {
  throw NetworkException("Socket listen is unavailable on STAR_PLATFORM_N3DS placeholder build");
}

void Socket::setNonBlocking(bool) {
  // STUB: non-blocking mode is a no-op for placeholder sockets.
}

void Socket::setTimeout(unsigned) {
  // STUB: timeout configuration is a no-op for placeholder sockets.
}

NetworkMode Socket::networkMode() const {
  return m_networkMode;
}

SocketMode Socket::socketMode() const {
  return m_socketMode;
}

bool Socket::isActive() const {
  return m_socketMode == SocketMode::Bound || m_socketMode == SocketMode::Connected;
}

bool Socket::isOpen() const {
  return m_socketMode != SocketMode::Closed;
}

void Socket::shutdown() {
  doShutdown();
}

void Socket::close() {
  doShutdown();
  doClose();
}

Socket::Socket(SocketType, NetworkMode networkMode)
  : m_networkMode(networkMode), m_impl({}), m_socketMode(SocketMode::Shutdown), m_localAddress(makeUnsupportedAddress(networkMode)) {}

Socket::Socket(NetworkMode networkMode, SocketImplPtr, SocketMode socketMode)
  : m_networkMode(networkMode), m_impl({}), m_socketMode(socketMode), m_localAddress(makeUnsupportedAddress(networkMode)) {}

void Socket::checkOpen(char const* methodName) const {
  if (!isOpen())
    throw SocketClosedException::format("Socket not open in {}", methodName);
}

void Socket::doShutdown() {
  if (m_socketMode != SocketMode::Closed)
    m_socketMode = SocketMode::Shutdown;
}

void Socket::doClose() {
  m_socketMode = SocketMode::Closed;
}

TcpSocketPtr TcpSocket::connectTo(HostAddressWithPort const&) {
  throw NetworkException("Tcp connect is unavailable on STAR_PLATFORM_N3DS placeholder build");
}

TcpSocketPtr TcpSocket::listen(HostAddressWithPort const&) {
  throw NetworkException("Tcp listen is unavailable on STAR_PLATFORM_N3DS placeholder build");
}

TcpSocketPtr TcpSocket::accept() {
  return {};
}

void TcpSocket::setNoDelay(bool) {
  // STUB: no-op in Nintendo 3DS placeholder socket path.
}

size_t TcpSocket::receive(char*, size_t) {
  throw SocketClosedException("Tcp receive is unavailable on STAR_PLATFORM_N3DS placeholder build");
}

size_t TcpSocket::send(char const*, size_t) {
  throw SocketClosedException("Tcp send is unavailable on STAR_PLATFORM_N3DS placeholder build");
}

HostAddressWithPort TcpSocket::localAddress() const {
  return m_localAddress;
}

HostAddressWithPort TcpSocket::remoteAddress() const {
  return m_remoteAddress;
}

TcpSocket::TcpSocket(NetworkMode networkMode)
  : Socket(SocketType::Tcp, networkMode), m_remoteAddress(makeUnsupportedAddress(networkMode)) {}

TcpSocket::TcpSocket(NetworkMode networkMode, SocketImplPtr)
  : Socket(SocketType::Tcp, networkMode), m_remoteAddress(makeUnsupportedAddress(networkMode)) {}

void TcpSocket::connect(HostAddressWithPort const&) {
  throw NetworkException("Tcp connect is unavailable on STAR_PLATFORM_N3DS placeholder build");
}

TcpServer::TcpServer(HostAddressWithPort const& address)
  : m_hostAddress(address), m_listenSocket({}) {}

TcpServer::TcpServer(uint16_t port) : TcpServer(makeUnsupportedAddress(NetworkMode::IPv4)) {
  (void)port;
}

TcpServer::~TcpServer() {
  stop();
}

void TcpServer::stop() {
  m_callbackThread.finish();
}

bool TcpServer::isListening() const {
  return false;
}

TcpSocketPtr TcpServer::accept(unsigned) {
  return {};
}

void TcpServer::setAcceptCallback(AcceptCallback callback, unsigned) {
  MutexLocker locker(m_mutex);
  m_callback = std::move(callback);
}

UdpSocket::UdpSocket(NetworkMode networkMode)
  : Socket(SocketType::Udp, networkMode) {}

size_t UdpSocket::receive(HostAddressWithPort*, char*, size_t) {
  throw SocketClosedException("Udp receive is unavailable on STAR_PLATFORM_N3DS placeholder build");
}

size_t UdpSocket::send(HostAddressWithPort const&, char const*, size_t) {
  throw SocketClosedException("Udp send is unavailable on STAR_PLATFORM_N3DS placeholder build");
}

UdpServer::UdpServer(HostAddressWithPort const& address)
  : m_hostAddress(address), m_listenSocket({}) {}

UdpServer::~UdpServer() {
  close();
}

void UdpServer::close() {}

bool UdpServer::isListening() const {
  return false;
}

size_t UdpServer::receive(HostAddressWithPort*, char*, size_t, unsigned) {
  return 0;
}

size_t UdpServer::send(HostAddressWithPort const&, char const*, size_t) {
  return 0;
}

}

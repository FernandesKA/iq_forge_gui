#include "iq_forge_device.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace iqforge {

namespace {

constexpr int kDefaultPort = 7373; // must match iq_forge_fw's net::kDefaultControlPort
constexpr int kTimeoutMs = 2000;

#if defined(_WIN32)
void ensureWinsockInitialized() {
  static std::once_flag once;
  std::call_once(once, [] {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
    std::atexit([] { WSACleanup(); });
  });
}
int closeSocket(std::uintptr_t s) { return closesocket(static_cast<SOCKET>(s)); }
#else
int closeSocket(int s) { return ::close(s); }
#endif

// Splits "host[:port]" -- port defaults to kDefaultPort if absent or empty.
void parseHostPort(const std::string& uri, std::string& hostOut, int& portOut) {
  hostOut = uri;
  portOut = kDefaultPort;
  auto colon = uri.rfind(':');
  if (colon != std::string::npos) {
    std::string portStr = uri.substr(colon + 1);
    if (!portStr.empty()) {
      char* end = nullptr;
      long p = std::strtol(portStr.c_str(), &end, 10);
      if (end != portStr.c_str() && *end == '\0' && p > 0 && p <= 65535) {
        hostOut = uri.substr(0, colon);
        portOut = static_cast<int>(p);
      }
    }
  }
  if (hostOut.empty()) hostOut = "192.168.0.7"; // this project's usual bench IP
}

std::uint64_t doubleToBits(double d) {
  std::uint64_t u;
  std::memcpy(&u, &d, sizeof(u));
  return u;
}

double bitsToDouble(std::uint64_t u) {
  double d;
  std::memcpy(&d, &u, sizeof(d));
  return d;
}

void encode(const IqForgePacket& p, std::uint8_t out[kIqForgePacketSize]) {
  out[0] = static_cast<std::uint8_t>(p.magic >> 24);
  out[1] = static_cast<std::uint8_t>(p.magic >> 16);
  out[2] = static_cast<std::uint8_t>(p.magic >> 8);
  out[3] = static_cast<std::uint8_t>(p.magic);
  out[4] = static_cast<std::uint8_t>(p.command >> 8);
  out[5] = static_cast<std::uint8_t>(p.command);
  out[6] = static_cast<std::uint8_t>(p.code >> 8);
  out[7] = static_cast<std::uint8_t>(p.code);
  out[8] = static_cast<std::uint8_t>(p.queryId >> 24);
  out[9] = static_cast<std::uint8_t>(p.queryId >> 16);
  out[10] = static_cast<std::uint8_t>(p.queryId >> 8);
  out[11] = static_cast<std::uint8_t>(p.queryId);
  for (int i = 0; i < 8; ++i) {
    out[12 + i] = static_cast<std::uint8_t>(p.arg >> (56 - 8 * i));
  }
}

IqForgePacket decode(const std::uint8_t in[kIqForgePacketSize]) {
  IqForgePacket p;
  p.magic = (static_cast<std::uint32_t>(in[0]) << 24) | (static_cast<std::uint32_t>(in[1]) << 16) |
            (static_cast<std::uint32_t>(in[2]) << 8) | static_cast<std::uint32_t>(in[3]);
  p.command = static_cast<std::uint16_t>((in[4] << 8) | in[5]);
  p.code = static_cast<std::uint16_t>((in[6] << 8) | in[7]);
  p.queryId = (static_cast<std::uint32_t>(in[8]) << 24) | (static_cast<std::uint32_t>(in[9]) << 16) |
              (static_cast<std::uint32_t>(in[10]) << 8) | static_cast<std::uint32_t>(in[11]);
  p.arg = 0;
  for (int i = 0; i < 8; ++i) {
    p.arg = (p.arg << 8) | in[12 + i];
  }
  return p;
}

} // namespace

IqForgeDevice::IqForgeDevice() {
#if defined(_WIN32)
  ensureWinsockInitialized();
#endif
}

IqForgeDevice::~IqForgeDevice() { close(); }

bool IqForgeDevice::open(const DeviceConfig& cfg, std::string& errorOut) {
  close();

  std::string host;
  int port = 0;
  parseHostPort(cfg.uri, host, port);

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result = nullptr;
  char portStr[16];
  std::snprintf(portStr, sizeof(portStr), "%d", port);
  if (getaddrinfo(host.c_str(), portStr, &hints, &result) != 0 || !result) {
    errorOut = "Cannot resolve host '" + host + "'";
    return false;
  }

  SocketHandle s;
#if defined(_WIN32)
  s = static_cast<SocketHandle>(::socket(result->ai_family, result->ai_socktype, result->ai_protocol));
  bool valid = s != kInvalidSocket;
#else
  s = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
  bool valid = s >= 0;
#endif
  if (!valid) {
    freeaddrinfo(result);
    errorOut = "Cannot create socket";
    return false;
  }

#if defined(_WIN32)
  DWORD timeout = kTimeoutMs;
  setsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
  setsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
  int connectResult = ::connect(static_cast<SOCKET>(s), result->ai_addr, static_cast<int>(result->ai_addrlen));
#else
  struct timeval timeout {};
  timeout.tv_sec = kTimeoutMs / 1000;
  timeout.tv_usec = (kTimeoutMs % 1000) * 1000;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  int connectResult = ::connect(s, result->ai_addr, result->ai_addrlen);
#endif
  freeaddrinfo(result);

  if (connectResult != 0) {
    errorOut = "Cannot connect to " + host + ":" + portStr + " (is iq_forge_app running there?)";
    closeSocket(s);
    return false;
  }

  int one = 1;
#if defined(_WIN32)
  setsockopt(static_cast<SOCKET>(s), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
#else
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif

  socket_ = s;
  nextQueryId_ = 1;

  IqForgePacket response;
  if (!request(IqForgeCommand::Ping, 0, response) || response.code != static_cast<std::uint16_t>(IqForgeCode::Ack)) {
    errorOut = "Connected, but board did not respond to ping (wrong host/port or stale firmware?)";
    close();
    return false;
  }

  // Push the initial center frequency immediately, same as PlutoDevice/
  // HackRFDevice do during open() -- the Device panel doesn't otherwise
  // call setFrequency() until it changes after connecting.
  setFrequency(cfg.centerFreqHz);

  return true;
}

void IqForgeDevice::close() {
  if (socket_ != kInvalidSocket) {
    closeSocket(socket_);
    socket_ = kInvalidSocket;
  }
  txRunning_.store(false);
}

bool IqForgeDevice::checkAlive() {
  IqForgePacket response;
  return request(IqForgeCommand::Ping, 0, response) && response.code == static_cast<std::uint16_t>(IqForgeCode::Ack);
}

bool IqForgeDevice::request(IqForgeCommand command, std::uint64_t arg, IqForgePacket& responseOut) {
  std::lock_guard<std::mutex> lock(ioMutex_);
  if (socket_ == kInvalidSocket) return false;

  IqForgePacket req;
  req.command = static_cast<std::uint16_t>(command);
  req.queryId = nextQueryId_++;
  req.arg = arg;

  std::uint8_t outBuf[kIqForgePacketSize];
  encode(req, outBuf);

  std::size_t sent = 0;
  while (sent < kIqForgePacketSize) {
#if defined(_WIN32)
    int n = ::send(static_cast<SOCKET>(socket_), reinterpret_cast<const char*>(outBuf) + sent,
                    static_cast<int>(kIqForgePacketSize - sent), 0);
#else
    ssize_t n = ::send(socket_, outBuf + sent, kIqForgePacketSize - sent, 0);
#endif
    if (n <= 0) return false;
    sent += static_cast<std::size_t>(n);
  }

  std::uint8_t inBuf[kIqForgePacketSize];
  std::size_t have = 0;
  while (have < kIqForgePacketSize) {
#if defined(_WIN32)
    int n = ::recv(static_cast<SOCKET>(socket_), reinterpret_cast<char*>(inBuf) + have,
                    static_cast<int>(kIqForgePacketSize - have), 0);
#else
    ssize_t n = ::recv(socket_, inBuf + have, kIqForgePacketSize - have, 0);
#endif
    if (n <= 0) return false; // disconnected, timed out, or error
    have += static_cast<std::size_t>(n);
  }

  IqForgePacket resp = decode(inBuf);
  if (resp.magic != kIqForgeProtocolMagic || resp.queryId != req.queryId) {
    return false; // desynced framing or stale/mismatched response -- treat as dead
  }
  responseOut = resp;
  return true;
}

bool IqForgeDevice::startTx(std::shared_ptr<ISampleSource> /*source*/, std::string& errorOut) {
  // No IQ streaming for this device: the DDS runs entirely on-board. This
  // just enables it at whatever frequency setFrequency() last set.
  IqForgePacket response;
  if (!request(IqForgeCommand::Enable, 0, response) ||
      response.code != static_cast<std::uint16_t>(IqForgeCode::Ack)) {
    errorOut = "Board rejected DDS enable (code " + std::to_string(response.code) + ")";
    return false;
  }
  txRunning_.store(true);
  return true;
}

void IqForgeDevice::stopTx() {
  IqForgePacket response;
  request(IqForgeCommand::Disable, 0, response);
  txRunning_.store(false);
}

bool IqForgeDevice::startRx(RxCallback /*callback*/, std::string& errorOut) {
  errorOut = "IqForge device does not support RX yet (sine-only DDS TX for now)";
  return false;
}

bool IqForgeDevice::setFrequency(double hz) {
  IqForgePacket response;
  return request(IqForgeCommand::SetFreq, doubleToBits(hz), response) &&
         response.code == static_cast<std::uint16_t>(IqForgeCode::Ack);
}

bool IqForgeDevice::setSampleRate(double /*sps*/) { return false; }
bool IqForgeDevice::setBandwidth(double /*hz*/) { return false; }
bool IqForgeDevice::setTxGain(double /*db*/) { return false; }
bool IqForgeDevice::setRxGain(double /*db*/) { return false; }
bool IqForgeDevice::setRxGainMode(RxGainMode /*mode*/) { return false; }

} // namespace iqforge

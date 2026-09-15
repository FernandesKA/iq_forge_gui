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
  recvBuf_.clear();

  std::string response;
  if (!sendCommand("PING", response) || response.substr(0, 2) != "OK") {
    errorOut = "Connected, but board did not respond to PING (wrong host/port or stale firmware?)";
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
  std::string response;
  return sendCommand("PING", response) && response.substr(0, 2) == "OK";
}

bool IqForgeDevice::sendCommand(const std::string& line, std::string& responseOut) {
  std::lock_guard<std::mutex> lock(ioMutex_);
  if (socket_ == kInvalidSocket) return false;

  std::string toSend = line + "\n";
#if defined(_WIN32)
  int sent = ::send(static_cast<SOCKET>(socket_), toSend.data(), static_cast<int>(toSend.size()), 0);
#else
  ssize_t sent = ::send(socket_, toSend.data(), toSend.size(), 0);
#endif
  if (sent <= 0 || static_cast<size_t>(sent) != toSend.size()) {
    return false;
  }

  for (;;) {
    auto pos = recvBuf_.find('\n');
    if (pos != std::string::npos) {
      responseOut = recvBuf_.substr(0, pos);
      if (!responseOut.empty() && responseOut.back() == '\r') responseOut.pop_back();
      recvBuf_.erase(0, pos + 1);
      return true;
    }

    char chunk[256];
#if defined(_WIN32)
    int n = ::recv(static_cast<SOCKET>(socket_), chunk, sizeof(chunk), 0);
#else
    ssize_t n = ::recv(socket_, chunk, sizeof(chunk), 0);
#endif
    if (n <= 0) return false; // disconnected, timed out, or error
    recvBuf_.append(chunk, static_cast<size_t>(n));
  }
}

bool IqForgeDevice::startTx(std::shared_ptr<ISampleSource> /*source*/, std::string& errorOut) {
  // No IQ streaming for this device: the DDS runs entirely on-board. This
  // just enables it at whatever frequency setFrequency() last set.
  std::string response;
  if (!sendCommand("ENABLE", response) || response.substr(0, 2) != "OK") {
    errorOut = response.empty() ? "Connection lost" : response;
    return false;
  }
  txRunning_.store(true);
  return true;
}

void IqForgeDevice::stopTx() {
  std::string response;
  sendCommand("DISABLE", response);
  txRunning_.store(false);
}

bool IqForgeDevice::startRx(RxCallback /*callback*/, std::string& errorOut) {
  errorOut = "IqForge device does not support RX yet (sine-only DDS TX for now)";
  return false;
}

bool IqForgeDevice::setFrequency(double hz) {
  std::string cmd = "SET_FREQ " + std::to_string(hz);
  std::string response;
  return sendCommand(cmd, response) && response.substr(0, 2) == "OK";
}

bool IqForgeDevice::setSampleRate(double /*sps*/) { return false; }
bool IqForgeDevice::setBandwidth(double /*hz*/) { return false; }
bool IqForgeDevice::setTxGain(double /*db*/) { return false; }
bool IqForgeDevice::setRxGain(double /*db*/) { return false; }
bool IqForgeDevice::setRxGainMode(RxGainMode /*mode*/) { return false; }

} // namespace iqforge

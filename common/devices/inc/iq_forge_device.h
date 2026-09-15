#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "i_device.h"

namespace iqforge {

// Wire protocol for talking to iq_forge_fw's net::control_server, over
// Ethernet (plain TCP, not serial). Fixed 20-byte packets both ways, all
// multi-byte integers in network (big-endian) byte order -- mirrors
// iq_forge_fw/project/inc/control_server.h; keep the two in sync by hand
// (separate repos, no shared header).
//
//   offset  size  field
//   0       4     magic     (kProtocolMagic, checked on every packet)
//   4       2     command   (IqForgeCommand; request only, 0 in responses)
//   6       2     code      (0 in requests; IqForgeCode in responses)
//   8       4     query_id  (assigned per request here, echoed back by the board)
//   12      8     arg       (command-specific, see IqForgeCommand)
constexpr std::uint32_t kIqForgeProtocolMagic = 0x49514631; // "IQF1"
constexpr std::size_t kIqForgePacketSize = 20;

enum class IqForgeCommand : std::uint16_t {
  Ping = 0,
  SetFreq = 1,
  GetFreq = 2,
  Enable = 3,
  Disable = 4,
  GetEnabled = 5,
};

enum class IqForgeCode : std::uint16_t {
  Ack = 0,
  Nack = 1,
  NackUnknownCommand = 2,
  NackBadArg = 3,
};

struct IqForgePacket {
  std::uint32_t magic = kIqForgeProtocolMagic;
  std::uint16_t command = 0;
  std::uint16_t code = 0;
  std::uint32_t queryId = 0;
  std::uint64_t arg = 0;
};

// Talks to this project's own board firmware (iq_forge_hdl + iq_forge_fw,
// see control_server.h on that side) over the binary protocol above -- not
// libiio, not the HackRF protocol. Sine-only for now: there is no IQ
// streaming here, just the on-board DDS's frequency and enable/disable.
// startTx()/stopTx() therefore ignore the given ISampleSource entirely --
// they just enable/disable the DDS at whatever frequency setFrequency()
// last set (typically the "DDS freq" field, which the Device panel already
// pushes via setFrequency() for every device kind).
//
// Unsupported for this device kind (setSampleRate/setBandwidth/setTxGain/
// setRxGain/setRxGainMode, and RX entirely): return false so the GUI can
// treat the request as rejected, matching how PlutoDevice/HackRFDevice
// report an unsupported request, and leaving room to reroute these onto the
// AD9361 transceiver controls (iq_forge_app menu items 1-7) later.
class IqForgeDevice : public IDevice {
 public:
  IqForgeDevice();
  ~IqForgeDevice() override;

  bool open(const DeviceConfig& cfg, std::string& errorOut) override;
  void close() override;
  bool isOpen() const override { return socket_ != kInvalidSocket; }
  bool checkAlive() override;

  bool startTx(std::shared_ptr<ISampleSource> source, std::string& errorOut) override;
  void stopTx() override;
  bool isTxRunning() const override { return txRunning_.load(); }

  bool startRx(RxCallback callback, std::string& errorOut) override;
  void stopRx() override {}
  bool isRxRunning() const override { return false; }

  bool setFrequency(double hz) override;
  bool setSampleRate(double sps) override;
  bool setBandwidth(double hz) override;
  bool setTxGain(double db) override;
  bool setRxGain(double db) override;
  bool setRxGainMode(RxGainMode mode) override;

  std::string name() const override { return "IqForge"; }

 private:
  // Sends one request packet (command/arg filled in, magic/queryId set
  // here) and waits for the matching response (same queryId). Returns
  // false on any socket error/disconnect/timeout or a queryId mismatch, in
  // which case the connection is considered dead (see isOpen()/checkAlive()).
  bool request(IqForgeCommand command, std::uint64_t arg, IqForgePacket& responseOut);

#if defined(_WIN32)
  using SocketHandle = std::uintptr_t; // SOCKET
  static constexpr SocketHandle kInvalidSocket = static_cast<SocketHandle>(~0ull);
#else
  using SocketHandle = int;
  static constexpr SocketHandle kInvalidSocket = -1;
#endif

  SocketHandle socket_ = kInvalidSocket;
  std::mutex ioMutex_; // serializes request() -- one in flight at a time
  std::uint32_t nextQueryId_ = 1;
  std::atomic<bool> txRunning_{false};
};

} // namespace iqforge

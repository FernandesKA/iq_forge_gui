#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "i_device.h"

namespace iqforge {

// Talks to this project's own board firmware (iq_forge_hdl + iq_forge_fw,
// see control_server.h on that side) over a small line-based TCP protocol --
// not libiio, not the HackRF protocol. Sine-only for now: there is no IQ
// streaming here, just the on-board DDS's frequency and enable/disable.
// startTx()/stopTx() therefore ignore the given ISampleSource entirely --
// they just enable/disable the DDS at whatever frequency setFrequency()
// last set (typically the "Center freq" field, which the Device panel
// already pushes via setFrequency() for every device kind).
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
  // Sends "<line>\n", reads back one "\n"-terminated response line (without
  // the newline). Returns false on any socket error/disconnect/timeout, in
  // which case the connection is considered dead (see isOpen()/checkAlive()).
  bool sendCommand(const std::string& line, std::string& responseOut);

#if defined(_WIN32)
  using SocketHandle = std::uintptr_t; // SOCKET
  static constexpr SocketHandle kInvalidSocket = static_cast<SocketHandle>(~0ull);
#else
  using SocketHandle = int;
  static constexpr SocketHandle kInvalidSocket = -1;
#endif

  SocketHandle socket_ = kInvalidSocket;
  std::mutex ioMutex_; // serializes sendCommand() -- one request in flight at a time
  std::string recvBuf_;
  std::atomic<bool> txRunning_{false};
};

} // namespace iqforge

#pragma once

#include <atomic>
#include <thread>

#include "i_device.h"

struct iio_context;
struct iio_device;
struct iio_channel;
struct iio_buffer;

namespace iqforge {

// PlutoSDR backend via libiio. Works with both USB ("usb:", "usb:1.5.5") and
// network ("ip:192.168.2.1", "ip:pluto.local") context URIs — libiio treats
// them identically once the context is created.
class PlutoDevice : public IDevice {
 public:
  PlutoDevice();
  ~PlutoDevice() override;

  bool open(const DeviceConfig& cfg, std::string& errorOut) override;
  void close() override;
  bool isOpen() const override { return ctx_ != nullptr; }
  bool checkAlive() override;

  bool startTx(std::shared_ptr<ISampleSource> source, std::string& errorOut) override;
  void stopTx() override;
  bool isTxRunning() const override { return txRunning_.load(); }

  bool startRx(RxCallback callback, std::string& errorOut) override;
  void stopRx() override;
  bool isRxRunning() const override { return rxRunning_.load(); }

  bool setRxFrequency(double hz) override;
  bool setTxFrequency(double hz) override;
  bool setSampleRate(double sps) override;
  bool setBandwidth(double hz) override;
  bool setTxGain(double db) override;
  bool setRxGain(double db) override;
  bool setRxGainMode(RxGainMode mode) override;

  std::string name() const override { return "PlutoSDR"; }

 private:
  bool configurePhy(std::string& errorOut);
  void txThreadFunc(std::shared_ptr<ISampleSource> source);
  void rxThreadFunc(RxCallback callback);

  // Separate iio_context per role (config, TX stream, RX stream) rather than
  // one shared context: libiio's IIOD client protocol (used by both the
  // "usb" and "ip" backends) multiplexes buffer open/close and attribute
  // I/O over one connection per context, and destroying one direction's
  // streaming buffer while the other is mid-refill/push on the SAME shared
  // context can desync that connection -- observed as libiio's own
  // "READ LINE"/"WRITE ALL" ETIMEDOUT errors on stderr when stopping TX
  // (which pushes a silencing buffer and destroys it) while RX kept
  // streaming. Each direction gets its own independent connection so
  // tearing one down can never interfere with the other's data flow.
  iio_context* ctx_ = nullptr;   // phy config only: frequency/gain/rate/bandwidth/checkAlive
  iio_context* txCtx_ = nullptr; // TX buffer streaming only
  iio_context* rxCtx_ = nullptr; // RX buffer streaming only
  iio_device* phy_ = nullptr;
  iio_device* txDev_ = nullptr;
  iio_device* rxDev_ = nullptr;
  iio_channel* txChanI_ = nullptr;
  iio_channel* txChanQ_ = nullptr;
  iio_channel* rxChanI_ = nullptr;
  iio_channel* rxChanQ_ = nullptr;
  iio_buffer* txBuf_ = nullptr;
  iio_buffer* rxBuf_ = nullptr;

  std::thread txThread_;
  std::thread rxThread_;
  std::atomic<bool> txRunning_{false};
  std::atomic<bool> rxRunning_{false};
  std::atomic<bool> txStopFlag_{false};
  std::atomic<bool> rxStopFlag_{false};

  DeviceConfig cfg_;

  static constexpr size_t kBufferSamples = 1 << 14; // 16384 IQ samples per hardware buffer
};

} // namespace iqforge

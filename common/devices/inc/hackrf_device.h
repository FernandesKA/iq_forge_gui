#pragma once

#include <libhackrf/hackrf.h>

#include <atomic>
#include <vector>

#include "i_device.h"

namespace iqforge {

// HackRF backend via libhackrf. USB only. Note the gain model differs a lot
// from PlutoSDR's single hardwaregain value:
//  - RX gain (setRxGain, 0..~102 dB) is a generic knob split heuristically
//    between LNA (0-40dB, 8dB steps) and baseband VGA (0-62dB, 2dB steps).
//  - TX gain (setTxGain, 0..47 dB) maps directly to the TX VGA gain.
class HackRFDevice : public IDevice {
 public:
  HackRFDevice();
  ~HackRFDevice() override;

  bool open(const DeviceConfig& cfg, std::string& errorOut) override;
  void close() override;
  bool isOpen() const override { return dev_ != nullptr; }
  bool checkAlive() override;

  bool startTx(std::shared_ptr<ISampleSource> source, std::string& errorOut) override;
  void stopTx() override;
  bool isTxRunning() const override { return txRunning_.load(); }

  bool startRx(RxCallback callback, std::string& errorOut) override;
  void stopRx() override;
  bool isRxRunning() const override { return rxRunning_.load(); }

  // HackRF has one physical RF conversion chain shared between RX/TX --
  // both just retune that single synthesizer, keeping cfg_.rxCenterFreqHz/
  // txCenterFreqHz equal (true half-duplex, see i_device.h).
  bool setRxFrequency(double hz) override;
  bool setTxFrequency(double hz) override;
  bool setSampleRate(double sps) override;
  bool setBandwidth(double hz) override;
  bool setTxGain(double db) override;
  bool setRxGain(double db) override;
  // HackRF has no hardware AGC -- returns false for anything but Manual.
  bool setRxGainMode(RxGainMode mode) override;

  std::string name() const override { return "HackRF"; }

 private:
  bool setSharedFrequency(double hz);

  static int txCallbackTrampoline(hackrf_transfer* transfer);
  static int rxCallbackTrampoline(hackrf_transfer* transfer);
  int handleTx(hackrf_transfer* transfer);
  int handleRx(hackrf_transfer* transfer);

  hackrf_device* dev_ = nullptr;
  DeviceConfig cfg_;

  std::shared_ptr<ISampleSource> txSource_;
  RxCallback rxCallback_;
  std::atomic<bool> txRunning_{false};
  std::atomic<bool> rxRunning_{false};

  // Reused across callbacks to avoid per-transfer heap allocation.
  std::vector<Sample> txScratch_;
  std::vector<Sample> rxScratch_;
};

} // namespace iqforge

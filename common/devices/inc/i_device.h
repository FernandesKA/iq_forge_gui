#pragma once

#include <functional>
#include <memory>
#include <string>

#include "sample_source.h"

namespace iqforge {

// IqForge: this project's own rk7020f/pluto_sky boards (iq_forge_hdl +
// iq_forge_fw), talked to over TCP -- not libiio, not the HackRF protocol.
// Sine-only for now: no IQ streaming, just the on-board DDS's frequency and
// enable/disable (see iq_forge_device.h).
enum class DeviceKind { PlutoSDR, HackRF, IqForge };

// AD9361 (PlutoSDR) RX gain control mode. AgcSlow/AgcFast map to the
// AD9361's "slow_attack"/"fast_attack" gain_control_mode values -- slow is
// the better default for continuous monitoring (spectrum/waterfall), fast
// reacts quicker to bursty signals at the cost of more gain "pumping" on
// noise. HackRF has no hardware AGC at all (see hackrf_device.h) -- Manual
// is the only mode it actually supports.
enum class RxGainMode { AgcSlow, AgcFast, Manual };

struct DeviceConfig {
  DeviceKind kind = DeviceKind::PlutoSDR;

  // PlutoSDR: libiio context URI, e.g. "usb:" (first found), "usb:1.5.5",
  // "ip:192.168.2.1", "ip:pluto.local". Empty = auto ("usb:" then "ip:pluto.local").
  // HackRF: device serial number, empty = first device found.
  // IqForge: "host[:port]" of the board's iq_forge_app control server, e.g.
  // "192.168.0.7" (port defaults to 7373, see iq_forge_device.h).
  std::string uri;

  // 3 MSPS default: safely inside the AD9361's standard-firmware range
  // (roughly 2.083-61.44 MSPS without a custom FIR filter loaded; below
  // ~2.083 MSPS the device rejects the rate outright).
  double sampleRateHz = 3e6;
  double centerFreqHz = 915e6;
  double bandwidthHz = 2e6;

  // Pluto: TX attenuation in dB, 0 (max power) .. -89.75. HackRF: mapped to
  // txvga gain 0..47 dB.
  double txGainDb = -10.0;
  // Pluto: RX gain in dB, 0..77 (manual gain control). HackRF: mapped to
  // lna (0-40, 8dB steps) + vga (0-62, 2dB steps) gain.
  double rxGainDb = 30.0;
  // Pluto only -- see RxGainMode above. Ignored by HackRF (always manual).
  RxGainMode rxGainMode = RxGainMode::AgcSlow;

  // Pluto only: which physical TX chain to stream on -- 0 = TX1 (default),
  // 1 = TX2. TX2 only exists on units running AD9361 in 2T2R/dual-channel
  // mode (stock single-channel Pluto firmware doesn't expose it, in which
  // case open() fails with a clear error). Ignored by HackRF (single TX).
  int txChannel = 0;
  // Pluto only: which physical RX chain to stream on -- 0 = RX1 (default),
  // 1 = RX2. Same 2T2R/dual-channel firmware requirement as txChannel.
  // Ignored by HackRF (single RX).
  int rxChannel = 0;
};

using RxCallback = std::function<void(const Sample* data, size_t count)>;

// Common interface for a TX/RX capable SDR device. Implementations own a
// background thread for TX (pulling from an ISampleSource) and/or RX
// (pushing captured blocks to a callback); the GUI thread only calls the
// control methods below and never touches hardware directly.
class IDevice {
 public:
  virtual ~IDevice() = default;

  virtual bool open(const DeviceConfig& cfg, std::string& errorOut) = 0;
  virtual void close() = 0;
  virtual bool isOpen() const = 0;

  // Round-trips a cheap query to the hardware to confirm it is still
  // reachable (e.g. still plugged in). isOpen() alone only reflects local
  // handle state, which stays "open" even after the device physically
  // disappears -- callers should poll this periodically and disconnect on
  // failure to keep the UI's connected state honest.
  virtual bool checkAlive() = 0;

  virtual bool startTx(std::shared_ptr<ISampleSource> source, std::string& errorOut) = 0;
  virtual void stopTx() = 0;
  virtual bool isTxRunning() const = 0;

  // callback is invoked from the device's RX thread — keep it cheap
  // (e.g. push into a RingBuffer) and never block on the GUI.
  virtual bool startRx(RxCallback callback, std::string& errorOut) = 0;
  virtual void stopRx() = 0;
  virtual bool isRxRunning() const = 0;

  virtual bool setFrequency(double hz) = 0;
  virtual bool setSampleRate(double sps) = 0;
  virtual bool setBandwidth(double hz) = 0;
  virtual bool setTxGain(double db) = 0;
  virtual bool setRxGain(double db) = 0;
  // Returns false if the device doesn't support the requested mode (e.g.
  // any AGC mode on HackRF, which has no hardware AGC at all).
  virtual bool setRxGainMode(RxGainMode mode) = 0;

  virtual std::string name() const = 0;
};

std::unique_ptr<IDevice> createDevice(DeviceKind kind);

} // namespace iqforge

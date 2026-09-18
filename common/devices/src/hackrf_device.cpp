#include "hackrf_device.h"

#include "hackrf_init_guard.h"

#include <algorithm>
#include <cmath>

namespace iqforge {

namespace {
constexpr float kHostToDevice = 127.0f;
constexpr float kDeviceToHost = 1.0f / 128.0f;

// Heuristic split of a single "RX gain" knob into HackRF's discrete LNA
// (0-40dB, 8dB steps) + VGA (0-62dB, 2dB steps) stages.
void splitRxGain(double totalDb, uint32_t& lnaOut, uint32_t& vgaOut) {
  double clamped = std::clamp(totalDb, 0.0, 102.0);
  double lna = std::min(40.0, std::floor(clamped / 8.0) * 8.0);
  double vga = std::clamp(std::floor((clamped - lna) / 2.0) * 2.0, 0.0, 62.0);
  lnaOut = static_cast<uint32_t>(lna);
  vgaOut = static_cast<uint32_t>(vga);
}
} // namespace

HackRFDevice::HackRFDevice() = default;

HackRFDevice::~HackRFDevice() {
  close();
}

bool HackRFDevice::open(const DeviceConfig& cfg, std::string& errorOut) {
  close();
  ensureHackrfInitialized();
  cfg_ = cfg;

  int rc = cfg.uri.empty() ? hackrf_open(&dev_) : hackrf_open_by_serial(cfg.uri.c_str(), &dev_);
  if (rc != HACKRF_SUCCESS || !dev_) {
    errorOut = std::string("Failed to open HackRF: ") + hackrf_error_name(static_cast<hackrf_error>(rc));
    dev_ = nullptr;
    return false;
  }

  hackrf_set_amp_enable(dev_, 0);

  std::vector<std::string> warnings;
  if (!setRxFrequency(cfg.rxCenterFreqHz)) warnings.push_back("center frequency rejected");
  if (!setSampleRate(cfg.sampleRateHz)) warnings.push_back("sample rate rejected");
  if (!setBandwidth(cfg.bandwidthHz)) warnings.push_back("bandwidth rejected");
  if (!setTxGain(cfg.txGainDb)) warnings.push_back("TX gain rejected");
  if (!setRxGain(cfg.rxGainDb)) warnings.push_back("RX gain rejected");

  if (!warnings.empty()) {
    errorOut = "Device rejected some settings (still connected, actual values may differ): ";
    for (size_t i = 0; i < warnings.size(); ++i) {
      if (i) errorOut += "; ";
      errorOut += warnings[i];
    }
  }
  return true;
}

bool HackRFDevice::checkAlive() {
  if (!dev_) return false;
  uint8_t boardId = 0;
  return hackrf_board_id_read(dev_, &boardId) == HACKRF_SUCCESS;
}

void HackRFDevice::close() {
  stopTx();
  stopRx();
  if (dev_) {
    hackrf_close(dev_);
    dev_ = nullptr;
  }
}

bool HackRFDevice::startTx(std::shared_ptr<ISampleSource> source, std::string& errorOut) {
  if (!isOpen()) {
    errorOut = "Device not open";
    return false;
  }
  if (txRunning_.load()) return true;

  txSource_ = std::move(source);
  txRunning_ = true;
  int rc = hackrf_start_tx(dev_, &HackRFDevice::txCallbackTrampoline, this);
  if (rc != HACKRF_SUCCESS) {
    txRunning_ = false;
    txSource_.reset();
    errorOut = std::string("hackrf_start_tx failed: ") + hackrf_error_name(static_cast<hackrf_error>(rc));
    return false;
  }
  return true;
}

void HackRFDevice::stopTx() {
  if (!txRunning_.exchange(false)) return;
  if (dev_) hackrf_stop_tx(dev_);
  txSource_.reset();
}

bool HackRFDevice::startRx(RxCallback callback, std::string& errorOut) {
  if (!isOpen()) {
    errorOut = "Device not open";
    return false;
  }
  if (rxRunning_.load()) return true;

  rxCallback_ = std::move(callback);
  rxRunning_ = true;
  int rc = hackrf_start_rx(dev_, &HackRFDevice::rxCallbackTrampoline, this);
  if (rc != HACKRF_SUCCESS) {
    rxRunning_ = false;
    rxCallback_ = nullptr;
    errorOut = std::string("hackrf_start_rx failed: ") + hackrf_error_name(static_cast<hackrf_error>(rc));
    return false;
  }
  return true;
}

void HackRFDevice::stopRx() {
  if (!rxRunning_.exchange(false)) return;
  if (dev_) hackrf_stop_rx(dev_);
  rxCallback_ = nullptr;
}

int HackRFDevice::txCallbackTrampoline(hackrf_transfer* transfer) {
  return static_cast<HackRFDevice*>(transfer->tx_ctx)->handleTx(transfer);
}
int HackRFDevice::rxCallbackTrampoline(hackrf_transfer* transfer) {
  return static_cast<HackRFDevice*>(transfer->rx_ctx)->handleRx(transfer);
}

int HackRFDevice::handleTx(hackrf_transfer* transfer) {
  size_t numSamples = static_cast<size_t>(transfer->buffer_length) / 2;
  txScratch_.resize(numSamples);

  size_t got = txSource_ ? txSource_->generate(txScratch_.data(), numSamples) : 0;

  int8_t* buf = reinterpret_cast<int8_t*>(transfer->buffer);
  for (size_t i = 0; i < got; ++i) {
    buf[2 * i] = static_cast<int8_t>(std::clamp(txScratch_[i].real(), -1.0f, 1.0f) * kHostToDevice);
    buf[2 * i + 1] = static_cast<int8_t>(std::clamp(txScratch_[i].imag(), -1.0f, 1.0f) * kHostToDevice);
  }
  for (size_t i = got; i < numSamples; ++i) {
    buf[2 * i] = 0;
    buf[2 * i + 1] = 0;
  }
  transfer->valid_length = transfer->buffer_length;

  if (got < numSamples) {
    // Source exhausted (non-looping file reached EOF): flush zeros this
    // round and signal no further callbacks are needed.
    txRunning_ = false;
    return 1;
  }
  return 0;
}

int HackRFDevice::handleRx(hackrf_transfer* transfer) {
  size_t numSamples = static_cast<size_t>(transfer->valid_length) / 2;
  const int8_t* buf = reinterpret_cast<const int8_t*>(transfer->buffer);

  rxScratch_.resize(numSamples);
  for (size_t i = 0; i < numSamples; ++i) {
    rxScratch_[i] = Sample(buf[2 * i] * kDeviceToHost, buf[2 * i + 1] * kDeviceToHost);
  }
  if (rxCallback_ && numSamples > 0) rxCallback_(rxScratch_.data(), rxScratch_.size());
  return 0;
}

bool HackRFDevice::setSharedFrequency(double hz) {
  if (!dev_) return false;
  cfg_.rxCenterFreqHz = hz;
  cfg_.txCenterFreqHz = hz;
  return hackrf_set_freq(dev_, static_cast<uint64_t>(hz)) == HACKRF_SUCCESS;
}

bool HackRFDevice::setRxFrequency(double hz) { return setSharedFrequency(hz); }
bool HackRFDevice::setTxFrequency(double hz) { return setSharedFrequency(hz); }

bool HackRFDevice::setSampleRate(double sps) {
  if (!dev_) return false;
  cfg_.sampleRateHz = sps;
  return hackrf_set_sample_rate(dev_, sps) == HACKRF_SUCCESS;
}

bool HackRFDevice::setBandwidth(double hz) {
  if (!dev_) return false;
  cfg_.bandwidthHz = hz;
  uint32_t bw = hackrf_compute_baseband_filter_bw(static_cast<uint32_t>(hz));
  return hackrf_set_baseband_filter_bandwidth(dev_, bw) == HACKRF_SUCCESS;
}

bool HackRFDevice::setTxGain(double db) {
  if (!dev_) return false;
  cfg_.txGainDb = db;
  uint32_t vga = static_cast<uint32_t>(std::clamp(db, 0.0, 47.0));
  return hackrf_set_txvga_gain(dev_, vga) == HACKRF_SUCCESS;
}

bool HackRFDevice::setRxGain(double db) {
  if (!dev_) return false;
  cfg_.rxGainDb = db;
  uint32_t lna = 0, vga = 0;
  splitRxGain(db, lna, vga);
  bool ok = hackrf_set_lna_gain(dev_, lna) == HACKRF_SUCCESS;
  ok &= hackrf_set_vga_gain(dev_, vga) == HACKRF_SUCCESS;
  return ok;
}

bool HackRFDevice::setRxGainMode(RxGainMode mode) {
  return mode == RxGainMode::Manual; // no hardware AGC to switch to
}

} // namespace iqforge

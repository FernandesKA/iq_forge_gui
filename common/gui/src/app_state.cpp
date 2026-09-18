#include "app_state.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace iqforge {

namespace {
void appendTrim(std::vector<Sample>& buf, const SampleBuffer& block, size_t maxLen) {
  buf.insert(buf.end(), block.begin(), block.end());
  if (buf.size() > maxLen) {
    buf.erase(buf.begin(), buf.begin() + static_cast<long>(buf.size() - maxLen));
  }
}

// plotWaterfall() (plot_waterfall.cpp) renders one filled rectangle per
// cell -- rows x cols, no GPU texture caching -- every single frame,
// whether or not new data arrived. A screen is never anywhere near 32768
// pixels wide, so past this many columns there's nothing extra to actually
// see but plenty of frame time to lose (this is what made "real-time RX"
// visibly stutter once the FFT size control allowed picking large sizes).
// Max-pool rather than average so a narrow peak isn't smeared/dimmed by
// neighboring noise bins when several collapse into one column.
constexpr int kWaterfallMaxCols = 1024;

std::vector<float> downsampleMaxPool(const std::vector<float>& src, int targetCols) {
  int n = static_cast<int>(src.size());
  if (n <= targetCols) return src;
  std::vector<float> out(static_cast<size_t>(targetCols));
  for (int c = 0; c < targetCols; ++c) {
    int lo = static_cast<int>(static_cast<int64_t>(c) * n / targetCols);
    int hi = static_cast<int>(static_cast<int64_t>(c + 1) * n / targetCols);
    hi = std::max(hi, lo + 1);
    float peak = src[static_cast<size_t>(lo)];
    for (int i = lo + 1; i < hi; ++i) peak = std::max(peak, src[static_cast<size_t>(i)]);
    out[static_cast<size_t>(c)] = peak;
  }
  return out;
}

void pushWaterfallRow(std::deque<WaterfallRow>& rows, const std::vector<float>& row, int maxRows) {
  rows.push_back(WaterfallRow{downsampleMaxPool(row, kWaterfallMaxCols), std::chrono::steady_clock::now()});
  while (static_cast<int>(rows.size()) > maxRows) rows.pop_front();
}

void processSpectrum(FftProcessor& fft, const SampleBuffer& block, std::vector<float>& outDb) {
  size_t n = fft.fftSize();
  if (block.size() >= n) {
    fft.process(block.data() + (block.size() - n), n, outDb);
  } else {
    fft.process(block.data(), block.size(), outDb);
  }
}

// Samples generated per frame for the idle TX preview below -- arbitrary,
// just enough to give the spectrum/time-domain views a decent-sized window;
// unrelated to any hardware TX buffer size since nothing is actually being
// streamed to a device here.
constexpr size_t kGeneratorPreviewSamples = 4096;
} // namespace

void AppState::updateDisplays() {
  if (deviceManager.pollHealth()) {
    logError("Device disconnected (lost communication)");
  }

  // At high sample rates several blocks can drain from a ring in a single
  // frame; the spectrum only ever shows the *last* one anyway (each
  // processSpectrum call below overwrites rxSpectrumDb/txSpectrumDb), so
  // computing it once after the loop instead of once per block avoids
  // doing -- and immediately discarding -- most of that work. Time-domain
  // accumulation still sees every block, since that view is a genuine
  // rolling history, not a "just the latest" snapshot.
  SampleBuffer block;
  bool gotRx = false;
  while (rxRing.tryPop(block)) {
    // Recording is independent of the freeze below -- pausing the display
    // shouldn't silently pause a capture the user explicitly started.
    if (rxRecording) {
      rxRecordBuffer.insert(rxRecordBuffer.end(), block.begin(), block.end());
    }
    if (rxFrozen) continue;
    gotRx = true;
    appendTrim(rxTimeDomain, block, kTimeDomainMaxSamples);
  }
  if (gotRx) {
    processSpectrum(rxFft, block, rxSpectrumDb);
    pushWaterfallRow(rxWaterfallRows, rxSpectrumDb, kWaterfallMaxRows);
  }

  bool gotTx = false;
  if (!txFrozen) {
    while (txPreviewRing.tryPop(block)) {
      gotTx = true;
      appendTrim(txTimeDomain, block, kTimeDomainMaxSamples);
    }
    if (gotTx) processSpectrum(txFft, block, txSpectrumDb);

    // Without an active TX, neither the generator nor a loaded file source
    // otherwise ever runs, so the signal being configured is invisible until
    // the user actually starts transmitting -- preview it directly here
    // instead, using the very same source instance startTx() would hand to
    // the device (safe: isTxActive() and the device's own thread-join on
    // stop ensure this and the real TX thread never call generate() at once).
    if (!gotTx && !isTxActive()) {
      block.resize(kGeneratorPreviewSamples);
      size_t got = 0;
      if (txSourceMode == 0) {
        got = generator->generate(block.data(), block.size());
      } else if (fileSource) {
        got = fileSource->generate(block.data(), block.size());
      }
      if (got > 0) {
        block.resize(got);
        gotTx = true;
        appendTrim(txTimeDomain, block, kTimeDomainMaxSamples);
        processSpectrum(txFft, block, txSpectrumDb);
      }
    }
  }

  if (gotTx) pushWaterfallRow(txWaterfallRows, txSpectrumDb, kWaterfallMaxRows);

  // Signal Viewer has no "active" state to gate on (no device, no TX/RX) --
  // whatever's loaded just previews continuously, same idle-preview trick as
  // the TX file source above, until frozen or nothing is loaded.
  bool gotSv = false;
  if (!svFrozen && svSource) {
    block.resize(kGeneratorPreviewSamples);
    size_t got = svSource->generate(block.data(), block.size());
    if (got > 0) {
      block.resize(got);
      gotSv = true;
      appendTrim(svTimeDomain, block, kTimeDomainMaxSamples);
      processSpectrum(svFft, block, svSpectrumDb);
    }
  }
  if (gotSv) pushWaterfallRow(svWaterfallRows, svSpectrumDb, kWaterfallMaxRows);

  updateSweep();
}

void AppState::setFftSize(int newSize) {
  if (newSize == fftSize) return;
  fftSize = newSize;
  txFft.setFftSize(static_cast<size_t>(newSize));
  rxFft.setFftSize(static_cast<size_t>(newSize));
  txWaterfallRows.clear();
  rxWaterfallRows.clear();
}

void AppState::startSweep() {
  if (sweepRunning) return;
  if (selectedSweepBand < 0 || selectedSweepBand >= static_cast<int>(sweepBands.size())) return;
  IDevice* dev = deviceManager.device();
  if (!dev) return;

  const SweepBand& band = sweepBands[static_cast<size_t>(selectedSweepBand)];
  double lo = std::min(band.startFreqHz, band.endFreqHz);
  double hi = std::max(band.startFreqHz, band.endFreqHz);
  if (!(hi > lo) || sampleRateHz <= 0.0) return;

  sweepRangeStartHz = lo;
  sweepRangeEndHz = hi;
  sweepStepHz = sampleRateHz;

  int nBins = static_cast<int>(rxFft.fftSize());
  size_t numSteps = static_cast<size_t>(std::ceil((hi - lo) / sweepStepHz));
  sweepSpectrumDb.assign(std::max<size_t>(numSteps, 1) * static_cast<size_t>(nBins), -160.0f);

  sweepCurrentCenterHz = lo + sweepStepHz / 2.0;
  dev->setRxFrequency(sweepCurrentCenterHz);
  rxFft.resetAveraging();

  // Only take over RX if it wasn't already running -- so Stop sweep later
  // doesn't yank RX out from under something the user started by hand.
  sweepWeStartedRx = !dev->isRxRunning();
  if (sweepWeStartedRx) {
    std::string startErr;
    dev->startRx([this](const Sample* data, size_t count) { rxRing.pushLatest(SampleBuffer(data, data + count)); },
                 startErr);
  }

  sweepRetuneDeadline =
      std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                              std::chrono::duration<double>(kSweepSettleSec));
  sweepRunning = true;
  sweepStatus = "Sweeping...";
}

void AppState::stopSweep() {
  if (!sweepRunning) return;
  sweepRunning = false;
  if (sweepWeStartedRx && deviceManager.isConnected()) {
    deviceManager.device()->stopRx();
  }
  sweepStatus = "Stopped";
}

void AppState::updateSweep() {
  if (!sweepRunning) return;
  if (!deviceManager.isConnected()) {
    stopSweep();
    log("Sweep stopped: device disconnected");
    return;
  }
  auto now = std::chrono::steady_clock::now();
  if (now < sweepRetuneDeadline) return;

  // Capture the step that's had time to settle into the composite buffer
  // at its absolute-frequency offset, then retune to the next one --
  // wrapping back to the start of the range for a continuously repeating
  // sweep rather than a single one-shot pass.
  int nBins = static_cast<int>(rxFft.fftSize());
  size_t stepIndex =
      static_cast<size_t>(std::llround((sweepCurrentCenterHz - sweepStepHz / 2.0 - sweepRangeStartHz) / sweepStepHz));
  size_t offset = stepIndex * static_cast<size_t>(nBins);
  if (rxSpectrumDb.size() == static_cast<size_t>(nBins) && offset + static_cast<size_t>(nBins) <= sweepSpectrumDb.size()) {
    std::copy(rxSpectrumDb.begin(), rxSpectrumDb.end(), sweepSpectrumDb.begin() + static_cast<long>(offset));
  }

  double nextCenterHz = sweepCurrentCenterHz + sweepStepHz;
  if (nextCenterHz > sweepRangeEndHz) {
    nextCenterHz = sweepRangeStartHz + sweepStepHz / 2.0;
  }
  sweepCurrentCenterHz = nextCenterHz;
  deviceManager.device()->setRxFrequency(sweepCurrentCenterHz);
  rxFft.resetAveraging();
  sweepRetuneDeadline =
      now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(kSweepSettleSec));

  char buf[64];
  std::snprintf(buf, sizeof buf, "Sweeping: %.3f / %.3f MHz", sweepCurrentCenterHz / 1e6, sweepRangeEndHz / 1e6);
  sweepStatus = buf;
}

} // namespace iqforge

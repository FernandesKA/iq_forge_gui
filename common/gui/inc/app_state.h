#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "ring_buffer.h"
#include "sample_types.h"
#include "tee_sample_source.h"
#include "device_manager.h"
#include "device_scanner.h"
#include "fft_processor.h"
#include "iq_file.h"
#include "signal_generator.h"
#include "duration_input.h"
#include "freq_input.h"
#include "waterfall_row.h"

namespace iqforge {

// Everything the GUI panels read/write, gathered in one place so panel draw
// functions can just take an AppState& instead of threading a dozen
// references through. Owned by App, updated once per frame on the GUI
// thread; device I/O threads only ever touch the RingBuffers below.
struct AppState {
  static constexpr size_t kTimeDomainMaxSamples = 8192;
  static constexpr int kWaterfallMaxRows = 100;

  DeviceManager deviceManager;

  // --- Device panel ---
  DeviceKind selectedKind = DeviceKind::PlutoSDR;
  char uriBuffer[256] = "";
  double sampleRateHz = 3e6; // PlutoSDR's standard firmware rejects rates below ~2.083 MSPS
  double centerFreqHz = 915e6;
  double bandwidthHz = 2e6;
  FreqUnit sampleRateUnit = FreqUnit::MHz;
  FreqUnit centerFreqUnit = FreqUnit::MHz;
  FreqUnit bandwidthUnit = FreqUnit::MHz;
  double txGainDb = -89.75; // maximum PlutoSDR TX attenuation (safest startup level)
  double rxGainDb = 0.0;    // minimum RX gain
  RxGainMode rxGainMode = RxGainMode::AgcSlow; // PlutoSDR only -- HackRF has no AGC, always Manual there
  int txChannel = 0; // PlutoSDR only -- 0 = TX1 (default), 1 = TX2 (needs 2T2R firmware). Ignored by HackRF.
  int rxChannel = 0; // PlutoSDR only -- 0 = RX1 (default), 1 = RX2 (needs 2T2R firmware). Ignored by HackRF.
  std::string connectError;
  std::vector<ScannedDevice> scanResults;

  // FFT size used for both RX and TX spectrum/waterfall processing (see
  // fft_processor.h) -- one shared value rather than per-direction, since
  // the GUI exposes a single control (panel_device.cpp) for it. Declared
  // here, before txFft/rxFft below, so their default member initializers
  // can read it. Change it through setFftSize() below, not directly --
  // txFft/rxFft and the waterfall row histories all need to stay in sync
  // with it.
  int fftSize = 2048;

  // --- TX ---
  int txSourceMode = 0; // 0 = generator, 1 = IQ file
  std::shared_ptr<SignalGenerator> generator = std::make_shared<SignalGenerator>();
  GeneratorConfig genConfig;
  FreqUnit toneFreqUnit = FreqUnit::kHz;
  FreqUnit multiToneUnit = FreqUnit::kHz; // shared by all entries in genConfig.multiToneFreqsHz
  FreqUnit chirpDeviationUnit = FreqUnit::kHz;
  TimeUnit chirpDurationUnit = TimeUnit::Ms;
  FreqUnit barkerChipRateUnit = FreqUnit::kHz;
  FreqUnit prbsBitRateUnit = FreqUnit::kHz;
  TimeUnit pulseDurationUnit = TimeUnit::Us;      // Pulse waveform's own ДИ
  TimeUnit pulsePeriodUnit = TimeUnit::Us;        // Pulse waveform's own ППИ
  TimeUnit envelopeRectDurationUnit = TimeUnit::Us; // Rectangular envelope's own ДИ
  TimeUnit envelopeRectPeriodUnit = TimeUnit::Us;   // Rectangular envelope's own ППИ
  FreqUnit envelopeSineFreqUnit = FreqUnit::kHz;
  FreqUnit envelopeSincFreqUnit = FreqUnit::kHz;
  FreqUnit envelopeGaussianFreqUnit = FreqUnit::kHz;
  TimeUnit envelopeGaussianSigmaUnit = TimeUnit::Us;
  char filePathBuffer[512] = "";
  bool fileLoop = true;
  // Raw formats (.cf32/.ci16) don't store their own sample rate, so the user
  // states what the file was recorded at. On Load, resampleIq() converts it
  // to fileSourceRateHz * fileResampleCoefficient (the resulting rate shown
  // in the UI) if resampling is enabled.
  double fileSourceRateHz = 3e6;
  FreqUnit fileSourceRateUnit = FreqUnit::MHz;
  bool fileResampleEnabled = false;
  double fileResampleCoefficient = 1.0;
  std::string fileLoadedPath; // path fileSource below was actually loaded from, "" if none/stale
  std::string fileLoadError;
  // Set by Load whenever fileLoadedPath was a SigMF Recording (.sigmf-data/
  // .sigmf-meta) -- carries the sample rate/frequency/annotations recovered
  // from the sidecar for display, since a bare .cf32/.ci16 has no way to
  // store any of that. fileSourceRateHz above is already auto-filled from
  // this at Load time; nullopt for a plain .cf32/.ci16/.wav file.
  std::optional<SigmfMeta> fileSigmfInfo;
  // Shared by the idle preview in updateDisplays() and "Start TX" -- Start
  // TX just hands this same instance to the device, so playback continues
  // from wherever the preview's read position left off instead of jumping
  // back to the start.
  std::shared_ptr<IQFileSource> fileSource;
  std::string txError;
  // Bumped whenever the TX signal itself changes discontinuously (generator
  // config edited, source mode switched, a new file loaded) rather than just
  // continuing to stream -- drawVisualizationWindow() diffs this against the
  // last value it saw to force a plot re-fit, since otherwise a changed
  // waveform/spectrum shape would sit inside whatever zoom/scale was left
  // over from the previous signal.
  int txSignalGeneration = 0;
  RingBuffer<SampleBuffer> txPreviewRing{8};
  std::vector<Sample> txTimeDomain;
  std::vector<float> txSpectrumDb;
  FftProcessor txFft{SpectrumConfig{static_cast<size_t>(fftSize), WindowType::Hann, 0.3f}};
  std::deque<WaterfallRow> txWaterfallRows;
  // Pauses updateDisplays()'s writes to the fields above -- TX itself (and,
  // for RX below, recording) keeps running untouched; only what's shown
  // stops changing, so a signal can be inspected/measured without it
  // scrolling out from under the cursor.
  bool txFrozen = false;

  bool isTxActive() const {
    return deviceManager.device() && deviceManager.device()->isTxRunning();
  }

  // --- RX ---
  std::string rxError;
  RingBuffer<SampleBuffer> rxRing{8};
  std::vector<Sample> rxTimeDomain;
  std::vector<float> rxSpectrumDb;
  FftProcessor rxFft{SpectrumConfig{static_cast<size_t>(fftSize), WindowType::Hann, 0.3f}};
  std::deque<WaterfallRow> rxWaterfallRows;
  // See txFrozen above. Recording (below) is unaffected by this -- it's
  // fed from the same drained blocks regardless of whether the display is
  // frozen, since pausing the view shouldn't silently pause a recording.
  bool rxFrozen = false;

  bool rxRecording = false;
  // SigMF is the default: unlike raw Cf32Raw, it records sample rate, center
  // frequency, recording hardware, and annotations alongside the samples
  // (see saveSigmf() in iq_file.h) instead of losing all of that on save.
  enum class RxSaveFormat { Sigmf, Cf32Raw };
  RxSaveFormat rxSaveFormat = RxSaveFormat::Sigmf;
  char rxRecordPathBuffer[512] = "recording.sigmf-data";
  char rxRecordDescriptionBuffer[256] = ""; // optional SigMF core:description
  std::vector<Sample> rxRecordBuffer;

  // Events the user flags while recording ("Mark now"), turned into SigMF
  // core:annotations on save. Sample offsets are relative to the start of
  // rxRecordBuffer, so they're only meaningful until it's cleared (Save &
  // clear resets both together).
  struct RxAnnotation {
    uint64_t sampleStart = 0;
    uint64_t sampleCount = 1;
    std::string label;
  };
  std::vector<RxAnnotation> rxRecordAnnotations;
  char rxAnnotationLabelBuffer[128] = "";

  bool isRxActive() const {
    return deviceManager.device() && deviceManager.device()->isRxRunning();
  }

  // --- SpectrumViewer (wideband sweep) ---
  // A user-defined frequency range wider than the device can capture in one
  // instantaneous acquisition. startFreqHz/endFreqHz are always the source
  // of truth; editCenterSpan just picks which pair of fields the panel
  // shows for editing (Start/End, or the equivalent Center/Span) -- entering
  // one always recomputes the other, per-field units remembered
  // independently like every other frequency input in the app.
  struct SweepBand {
    std::string name = "Band";
    double startFreqHz = 100e6;
    double endFreqHz = 200e6;
    bool editCenterSpan = false;
    FreqUnit startUnit = FreqUnit::MHz;
    FreqUnit endUnit = FreqUnit::MHz;
    FreqUnit centerUnit = FreqUnit::MHz;
    FreqUnit spanUnit = FreqUnit::MHz;
  };
  // Session-only by design (not part of app_settings.h's persisted
  // snapshot) -- these are ad hoc scan ranges, not durable configuration.
  std::vector<SweepBand> sweepBands;
  int selectedSweepBand = -1; // index into sweepBands, -1 = none selected

  bool sweepRunning = false;
  // Whether the sweep itself started RX (vs. it already being running when
  // the sweep began) -- only stop RX on Stop sweep if the sweep is the one
  // that started it, so it doesn't yank RX out from under the user.
  bool sweepWeStartedRx = false;
  double sweepRangeStartHz = 0.0; // locked in from the selected band when the sweep (re)starts a pass
  double sweepRangeEndHz = 0.0;
  double sweepStepHz = 0.0; // == sampleRateHz at the moment the sweep started
  double sweepCurrentCenterHz = 0.0;
  std::chrono::steady_clock::time_point sweepRetuneDeadline{};
  // Composite spectrum across [sweepRangeStartHz, sweepRangeEndHz], one
  // rxFft-sized segment per step, filled in in place as each step completes
  // -- so it's visibly "filling in" during a sweep rather than only
  // appearing once the whole range is done.
  std::vector<float> sweepSpectrumDb;
  std::string sweepStatus;

  static constexpr double kSweepSettleSec = 0.2;

  // Starts sweeping the selected band: retunes to its first step and lets
  // updateSweep() (called from updateDisplays()) step through the rest.
  // No-op if already running, nothing selected, or no device connected.
  void startSweep();
  void stopSweep();
  void updateSweep();

  // --- Signal Viewer (no device connection needed: load/preview/resample/
  // write an IQ file directly, independent of TX/RX) ---
  char svFilePathBuffer[512] = "";
  bool svLoop = true;
  // See fileSourceRateHz above for why raw formats need this typed in by
  // hand; same SigMF auto-recovery applies here too.
  double svSourceRateHz = 3e6;
  FreqUnit svSourceRateUnit = FreqUnit::MHz;
  bool svResampleEnabled = false;
  double svResampleCoefficient = 1.0;
  std::string svLoadedPath;
  std::string svLoadError;
  std::optional<SigmfMeta> svSigmfInfo;
  // The buffer actually being previewed/saved -- already resampled at Load
  // time if svResampleEnabled was on, so Save just writes svSource->data()
  // as-is with no separate resample step of its own.
  std::shared_ptr<IQFileSource> svSource;
  // See txSignalGeneration above -- bumped on every successful file load so
  // the plots re-fit to the newly loaded signal instead of keeping the
  // previous file's zoom/scale.
  int svSignalGeneration = 0;
  double svActiveRateHz = 0.0; // svSource's sample rate (post-resample); drives the spectrum/time-domain X axis
  double svCenterFreqHz = 0.0; // display reference only -- from SigMF capture freq if present, else 0; no device involved
  std::vector<Sample> svTimeDomain;
  std::vector<float> svSpectrumDb;
  FftProcessor svFft{SpectrumConfig{static_cast<size_t>(fftSize), WindowType::Hann, 0.3f}};
  std::deque<WaterfallRow> svWaterfallRows;
  bool svFrozen = false;

  enum class SvSaveFormat { Sigmf, Cf32Raw };
  SvSaveFormat svSaveFormat = SvSaveFormat::Sigmf;
  char svSavePathBuffer[512] = "";

  // --- Main-area tab visibility (Settings panel) ---
  // Whether each plot/preview window is drawn at all this frame -- simply
  // skipping its Begin()/End() call, rather than an ImGui p_open close
  // button, is enough to hide it from the main-area tab bar entirely; it
  // keeps its docked position for whenever it's turned back on. Independent
  // of the *Control panels on the left, which always stay available since
  // they're configuration, not a per-mode view. SpectrumViewer defaults to
  // off since it's the least commonly used of the four.
  bool showTxTab = true;
  bool showRxTab = true;
  bool showSignalViewerTab = true;
  bool showSpectrumViewerTab = false;

  // --- Log ---
  std::mutex logMutex;
  std::deque<std::string> logMessages;
  void log(const std::string& msg) {
    std::lock_guard<std::mutex> lock(logMutex);
    logMessages.push_back(msg);
    if (logMessages.size() > 500) logMessages.pop_front();
  }

  // Called once per frame on the GUI thread: drains ring buffers, updates
  // time-domain/spectrum/waterfall data.
  void updateDisplays();

  // Applies a new FFT size to both txFft/rxFft (rebuilding their FFTW plans)
  // and drops both waterfall row histories: plotWaterfall() flattens rows
  // into a single fixed-stride buffer assuming every row is the same length
  // (see plot_waterfall.cpp), so leaving old-sized rows in the deque after
  // a resize would corrupt that flatten. No-op if newSize == fftSize.
  void setFftSize(int newSize);
};

} // namespace iqforge

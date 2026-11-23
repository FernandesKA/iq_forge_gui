#pragma once

#include <array>
#include <deque>
#include <vector>

#include "sample_types.h"
#include "plot_zoom_controls.h"

namespace iqforge {

constexpr int kMaxSpectrumMarkers = 4;

// Width reserved for the measurements table to the right of the spectrum
// plot (see plotSpectrum below). The waterfall plot below it reserves the
// same width so both plots' X axes line up pixel-for-pixel -- otherwise the
// waterfall (drawn full-width) is visibly wider than the spectrum plot above
// it and a centered signal appears offset between the two.
constexpr float kSpectrumMeasurementsWidth = 230.0f;

// A single frequency marker. `binIndex` (not a raw Hz value) is what's
// persisted, so a marker stays pinned to the same trace point across frames
// even though its displayed frequency depends on the current sample rate.
struct SpectrumMarker {
  bool active = false;
  int binIndex = 0;
  // Reference marker index for a delta readout (this marker's freq/amplitude
  // shown relative to that marker instead of absolute), -1 = off.
  int deltaRef = -1;
};

enum class SpectrumTraceMode { Live, MaxHold, MinHold };

struct SpectrumViewState {
  bool hadData = false;
  double sampleRateHz = 0.0;
  AxisZoomState zoom;

  // --- Markers ---
  std::array<SpectrumMarker, kMaxSpectrumMarkers> markers{};
  int selectedMarker = 0;
  // Local maxima found by the last Peak Search, sorted by descending
  // amplitude, so repeated "Next Peak" presses walk down the list.
  std::vector<int> peakOrder;
  size_t peakOrderPos = 0;
  // Set by plotSpectrum() when "-> Center Freq" is clicked; the caller
  // (which owns AppState/the device) consumes and clears this.
  bool centerFreqRequested = false;
  double requestedCenterFreqHz = 0.0;

  // --- Band marker ---
  bool bandEnabled = false;
  double bandLoHz = 0.0;
  double bandHiHz = 0.0;
  bool bandInit = false; // seeds Lo/Hi to a sane span the first time it's enabled

  // --- Trace mode (Live / Max Hold / Min Hold) ---
  SpectrumTraceMode traceMode = SpectrumTraceMode::Live;
  std::vector<float> holdDb;
  bool holdInit = false;

  // --- Peak hold overlay (independent of traceMode) ---
  bool peakHoldEnabled = false;
  std::vector<float> peakHoldDb;
  bool peakHoldInit = false;

  // --- Persistence (afterglow of recent sweeps) ---
  bool persistenceEnabled = false;
  static constexpr int kPersistenceDepth = 24;
  std::deque<std::vector<float>> persistenceFrames;

  // Last sweep seen by the trace-mode/peak-hold/persistence bookkeeping
  // above, so repeated GUI frames between actual new sweeps don't re-push
  // the same data (see plot_spectrum.cpp).
  std::vector<float> lastSeenDb;
};

// Draws one dB-magnitude spectrum plot for `db` (baseband, -sampleRateHz/2
// .. +sampleRateHz/2), plus a compact measurements table to its right (see
// measurements.h). `view` persists zoom/fit/marker/trace state across
// frames and automatically re-fits when the sample rate changes.
// `centerFreqHz` is the device's current absolute center frequency; it's
// only used to compute the absolute target frequency for "-> Center Freq"
// (the plot itself, like its X axis, stays in baseband terms). `timeDomain`
// is the raw rolling IQ buffer for the same direction as `db`, needed for
// the measurements table's RMS/peak/crest/DC/clipping rows.
void plotSpectrum(const char* plotId, const std::vector<float>& db, double sampleRateHz, double centerFreqHz,
                   const std::vector<Sample>& timeDomain, SpectrumViewState& view);

} // namespace iqforge

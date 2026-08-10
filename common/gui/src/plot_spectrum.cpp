#include "plot_spectrum.h"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "measurements.h"
#include "plot_format.h"

namespace iqforge {

namespace {

double binToFreq(int bin, double sampleRateHz, int n) {
  if (n <= 0) return 0.0;
  return -sampleRateHz / 2.0 + static_cast<double>(bin) * (sampleRateHz / n);
}

int freqToBin(double freqHz, double sampleRateHz, int n) {
  if (n <= 0) return 0;
  double lo = -sampleRateHz / 2.0, hi = sampleRateHz / 2.0;
  double f = std::clamp(freqHz, lo, hi);
  long bin = std::lround((f - lo) / (sampleRateHz / n));
  return static_cast<int>(std::clamp<long>(bin, 0, n - 1));
}

// Minimum fall (or rise) in dB required before a local extremum counts as a
// confirmed peak/trough. Without this, every noise-floor wiggle would count
// as its own "peak" and flood Next Peak. 6 dB matches the default peak
// excursion used by typical spectrum analyzers.
constexpr float kPeakExcursionDb = 6.0f;

// Classic single-pass "peak excursion" detector: walks the trace tracking
// the current rising/falling extremum, and only confirms it as a peak once
// the signal has fallen back by at least kPeakExcursionDb (confirming it was
// a real local maximum, not noise on the way to a taller one). Returned bins
// are sorted by descending amplitude, so callers can walk them as "next
// highest peak, then next, ...".
std::vector<int> findPeaks(const std::vector<float>& db) {
  std::vector<int> peaks;
  int n = static_cast<int>(db.size());
  if (n == 0) return peaks;
  if (n < 3) {
    peaks.push_back(static_cast<int>(std::max_element(db.begin(), db.end()) - db.begin()));
    return peaks;
  }

  bool searchingPeak = true;
  int extremeIdx = 0;
  float extremeVal = db[0];
  for (int i = 1; i < n; ++i) {
    float v = db[i];
    if (searchingPeak) {
      if (v > extremeVal) {
        extremeVal = v;
        extremeIdx = i;
      } else if (extremeVal - v >= kPeakExcursionDb) {
        peaks.push_back(extremeIdx);
        extremeVal = v;
        extremeIdx = i;
        searchingPeak = false;
      }
    } else {
      if (v < extremeVal) {
        extremeVal = v;
        extremeIdx = i;
      } else if (v - extremeVal >= kPeakExcursionDb) {
        extremeVal = v;
        extremeIdx = i;
        searchingPeak = true;
      }
    }
  }
  // A candidate still rising (or on a plateau) at the end of the trace was
  // never disqualified by a fall, so it's a real peak too.
  if (searchingPeak) peaks.push_back(extremeIdx);
  if (peaks.empty()) peaks.push_back(static_cast<int>(std::max_element(db.begin(), db.end()) - db.begin()));

  std::sort(peaks.begin(), peaks.end(), [&](int a, int b) { return db[a] > db[b]; });
  return peaks;
}

struct BandStats {
  double powerDb = 0.0;
  double peakDb = 0.0;
  double peakFreqHz = 0.0;
};

// Integrates linear power across [loHz, hiHz] for a total band-power
// readout, and separately reports the single highest bin in that span.
BandStats computeBandStats(const std::vector<float>& db, double sampleRateHz, double loHz, double hiHz) {
  BandStats st;
  int n = static_cast<int>(db.size());
  if (n == 0) return st;
  int iLo = freqToBin(std::min(loHz, hiHz), sampleRateHz, n);
  int iHi = freqToBin(std::max(loHz, hiHz), sampleRateHz, n);
  double sumLinear = 0.0;
  float peak = db[iLo];
  int peakBin = iLo;
  for (int i = iLo; i <= iHi; ++i) {
    sumLinear += std::pow(10.0, db[i] / 10.0);
    if (db[i] > peak) {
      peak = db[i];
      peakBin = i;
    }
  }
  st.powerDb = 10.0 * std::log10(std::max(sumLinear, 1e-30));
  st.peakDb = peak;
  st.peakFreqHz = binToFreq(peakBin, sampleRateHz, n);
  return st;
}

const ImVec4 kMarkerColors[kMaxSpectrumMarkers] = {
    ImVec4(1.0f, 1.0f, 0.2f, 1.0f),
    ImVec4(0.2f, 1.0f, 1.0f, 1.0f),
    ImVec4(1.0f, 0.4f, 1.0f, 1.0f),
    ImVec4(0.6f, 1.0f, 0.4f, 1.0f),
};

// Ctrl+click places/moves the selected marker at the nearest bin to the
// mouse -- same modifier convention as the time-domain sample cursor (see
// plot_line_view.h). Must run after the plot's axes are locked.
void updateMarkerPlacement(SpectrumViewState& view, const std::vector<float>& db, double sampleRateHz) {
  if (db.empty() || !ImPlot::IsPlotHovered()) return;
  if (!ImGui::GetIO().KeyCtrl || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
  ImPlotPoint mouse = ImPlot::GetPlotMousePos();
  auto& m = view.markers[view.selectedMarker];
  m.active = true;
  m.binIndex = freqToBin(mouse.x, sampleRateHz, static_cast<int>(db.size()));
  // A manual placement invalidates whatever Peak Search context Next Peak
  // was walking through.
  view.peakOrder.clear();
  view.peakOrderPos = 0;
}

void drawMarkerControls(SpectrumViewState& view, const std::vector<float>& db, double sampleRateHz,
                         double centerFreqHz) {
  if (!ImGui::TreeNodeEx("Markers (Ctrl+click to place)")) return;

  for (int i = 0; i < kMaxSpectrumMarkers; ++i) {
    char label[24];
    std::snprintf(label, sizeof label, "M%d%s", i + 1, view.markers[i].active ? "" : " (off)");
    bool isSel = (view.selectedMarker == i);
    if (isSel) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    bool clicked = ImGui::Button(label);
    if (isSel) ImGui::PopStyleColor();
    if (clicked) view.selectedMarker = i;
    if (i + 1 < kMaxSpectrumMarkers) sameLineOrWrap(wrapButtonWidth("M4 (off)"));
  }

  SpectrumMarker& sel = view.markers[view.selectedMarker];

  ImGui::BeginDisabled(db.empty());
  if (ImGui::Button("Peak search")) {
    view.peakOrder = findPeaks(db);
    view.peakOrderPos = 0;
    if (!view.peakOrder.empty()) {
      sel.active = true;
      sel.binIndex = view.peakOrder[0];
    }
  }
  sameLineOrWrap(wrapButtonWidth("Next peak"));
  ImGui::BeginDisabled(view.peakOrder.empty());
  if (ImGui::Button("Next peak")) {
    view.peakOrderPos = std::min(view.peakOrderPos + 1, view.peakOrder.size() - 1);
    sel.active = true;
    sel.binIndex = view.peakOrder[view.peakOrderPos];
  }
  ImGui::EndDisabled();
  sameLineOrWrap(wrapButtonWidth("-> Center freq"));
  ImGui::BeginDisabled(!sel.active);
  if (ImGui::Button("-> Center freq")) {
    view.requestedCenterFreqHz = centerFreqHz + binToFreq(sel.binIndex, sampleRateHz, static_cast<int>(db.size()));
    view.centerFreqRequested = true;
  }
  ImGui::EndDisabled();
  ImGui::EndDisabled();
  sameLineOrWrap(wrapButtonWidth("Clear"));
  if (ImGui::Button("Clear")) sel = SpectrumMarker{};

  char preview[8];
  std::snprintf(preview, sizeof preview, sel.deltaRef < 0 ? "Off" : "M%d", sel.deltaRef + 1);
  ImGui::SetNextItemWidth(90.0f);
  if (ImGui::BeginCombo("Delta ref", preview)) {
    if (ImGui::Selectable("Off", sel.deltaRef < 0)) sel.deltaRef = -1;
    for (int j = 0; j < kMaxSpectrumMarkers; ++j) {
      if (j == view.selectedMarker) continue;
      char item[8];
      std::snprintf(item, sizeof item, "M%d", j + 1);
      if (ImGui::Selectable(item, sel.deltaRef == j)) sel.deltaRef = j;
    }
    ImGui::EndCombo();
  }

  int n = static_cast<int>(db.size());
  for (int i = 0; i < kMaxSpectrumMarkers; ++i) {
    const SpectrumMarker& m = view.markers[i];
    if (!m.active || db.empty() || m.binIndex >= n) continue;
    double f = binToFreq(m.binIndex, sampleRateHz, n);
    float amp = db[m.binIndex];
    ImGui::TextColored(kMarkerColors[i], "M%d:", i + 1);
    ImGui::SameLine();
    ImGui::Text("%s   %.2f dBFS", formatHz(f).c_str(), amp);
    if (m.deltaRef >= 0 && m.deltaRef < kMaxSpectrumMarkers && view.markers[m.deltaRef].active) {
      const SpectrumMarker& r = view.markers[m.deltaRef];
      double fr = binToFreq(r.binIndex, sampleRateHz, n);
      float ar = db[r.binIndex];
      ImGui::SameLine();
      ImGui::TextDisabled("(delta vs M%d: %s, %.2f dB)", m.deltaRef + 1, formatHz(f - fr).c_str(), amp - ar);
    }
  }

  ImGui::TreePop();
}

void drawBandMarkerControls(SpectrumViewState& view, const std::vector<float>& db, double sampleRateHz) {
  if (!ImGui::TreeNodeEx("Band marker")) return;

  ImGui::Checkbox("Enabled", &view.bandEnabled);
  if (view.bandEnabled && !view.bandInit) {
    view.bandLoHz = -sampleRateHz * 0.1;
    view.bandHiHz = sampleRateHz * 0.1;
    view.bandInit = true;
  }
  if (view.bandEnabled) {
    double lo = -sampleRateHz / 2.0, hi = sampleRateHz / 2.0;
    double step = std::max(1.0, sampleRateHz * 0.001);
    // ~130px input frame plus its trailing "Lo (Hz)"/"Hi (Hz)" label.
    constexpr float kDragFieldWidth = 210.0f;
    sameLineOrWrap(kDragFieldWidth);
    ImGui::SetNextItemWidth(130.0f);
    ImGui::DragScalar("Lo (Hz)", ImGuiDataType_Double, &view.bandLoHz, static_cast<float>(step), &lo, &hi, "%.0f");
    sameLineOrWrap(kDragFieldWidth);
    ImGui::SetNextItemWidth(130.0f);
    ImGui::DragScalar("Hi (Hz)", ImGuiDataType_Double, &view.bandHiHz, static_cast<float>(step), &lo, &hi, "%.0f");

    if (!db.empty()) {
      BandStats st = computeBandStats(db, sampleRateHz, view.bandLoHz, view.bandHiHz);
      ImGui::Text("Band power: %.2f dBFS   Peak: %.2f dBFS @ %s", st.powerDb, st.peakDb,
                  formatHz(st.peakFreqHz).c_str());
    }
  }

  ImGui::TreePop();
}

void drawTraceControls(SpectrumViewState& view) {
  if (!ImGui::TreeNodeEx("Trace / peak hold / persistence")) return;

  const char* kTraceNames[] = {"Live", "Max hold", "Min hold"};
  int traceIdx = static_cast<int>(view.traceMode);
  ImGui::SetNextItemWidth(110.0f);
  if (ImGui::Combo("Trace", &traceIdx, kTraceNames, IM_ARRAYSIZE(kTraceNames))) {
    view.traceMode = static_cast<SpectrumTraceMode>(traceIdx);
    view.holdInit = false;
    view.holdDb.clear();
  }
  sameLineOrWrap(wrapButtonWidth("Reset hold"));
  ImGui::BeginDisabled(view.traceMode == SpectrumTraceMode::Live);
  if (ImGui::Button("Reset hold")) {
    view.holdInit = false;
    view.holdDb.clear();
  }
  ImGui::EndDisabled();

  bool peakHoldChanged = ImGui::Checkbox("Peak hold", &view.peakHoldEnabled);
  if (peakHoldChanged && view.peakHoldEnabled) {
    view.peakHoldInit = false;
    view.peakHoldDb.clear();
  }
  sameLineOrWrap(wrapButtonWidth("Clear peak hold"));
  ImGui::BeginDisabled(!view.peakHoldEnabled);
  if (ImGui::Button("Clear peak hold")) {
    view.peakHoldInit = false;
    view.peakHoldDb.clear();
  }
  ImGui::EndDisabled();

  ImGui::Checkbox("Persistence", &view.persistenceEnabled);
  sameLineOrWrap(wrapButtonWidth("Clear persistence"));
  ImGui::BeginDisabled(view.persistenceFrames.empty());
  if (ImGui::Button("Clear persistence")) view.persistenceFrames.clear();
  ImGui::EndDisabled();

  ImGui::TreePop();
}

// Feeds a newly-arrived sweep into the trace-hold/peak-hold/persistence
// accumulators. Gated on the sweep actually having changed (rather than
// running every GUI frame at ~60Hz) by comparing against the last sweep
// seen; skipped entirely when none of the three features are in use.
void updateAccumulators(SpectrumViewState& view, const std::vector<float>& db) {
  bool needed = view.traceMode != SpectrumTraceMode::Live || view.peakHoldEnabled || view.persistenceEnabled;
  if (!needed || db.empty()) return;
  bool isNewSweep = view.lastSeenDb.size() != db.size() || view.lastSeenDb != db;
  if (!isNewSweep) return;
  view.lastSeenDb = db;

  if (view.traceMode != SpectrumTraceMode::Live) {
    if (!view.holdInit || view.holdDb.size() != db.size()) {
      view.holdDb = db;
      view.holdInit = true;
    } else {
      for (size_t i = 0; i < db.size(); ++i) {
        view.holdDb[i] = view.traceMode == SpectrumTraceMode::MaxHold ? std::max(view.holdDb[i], db[i])
                                                                       : std::min(view.holdDb[i], db[i]);
      }
    }
  }

  if (view.peakHoldEnabled) {
    if (!view.peakHoldInit || view.peakHoldDb.size() != db.size()) {
      view.peakHoldDb = db;
      view.peakHoldInit = true;
    } else {
      for (size_t i = 0; i < db.size(); ++i) view.peakHoldDb[i] = std::max(view.peakHoldDb[i], db[i]);
    }
  }

  if (view.persistenceEnabled) {
    view.persistenceFrames.push_back(db);
    while (static_cast<int>(view.persistenceFrames.size()) > SpectrumViewState::kPersistenceDepth) {
      view.persistenceFrames.pop_front();
    }
  }
}

void drawPersistenceFrames(const SpectrumViewState& view, double sampleRateHz) {
  if (!view.persistenceEnabled || view.persistenceFrames.empty()) return;
  int depth = static_cast<int>(view.persistenceFrames.size());
  int idx = 0;
  for (const auto& frame : view.persistenceFrames) {
    ++idx;
    if (frame.empty()) continue;
    float alpha = 0.04f + 0.30f * (static_cast<float>(idx) / static_cast<float>(depth));
    ImPlot::SetNextLineStyle(ImVec4(0.4f, 0.8f, 1.0f, alpha), 1.0f);
    char id[24];
    std::snprintf(id, sizeof id, "##persist%d", idx);
    int n = static_cast<int>(frame.size());
    double xscale = sampleRateHz / n;
    double xstart = -sampleRateHz / 2.0;
    ImPlot::PlotLine(id, frame.data(), n, xscale, xstart);
  }
}

void drawMarkersOnPlot(const SpectrumViewState& view, const std::vector<float>& db) {
  int n = static_cast<int>(db.size());
  for (int i = 0; i < kMaxSpectrumMarkers; ++i) {
    const SpectrumMarker& m = view.markers[i];
    if (!m.active || db.empty() || m.binIndex >= n) continue;
    double fx = binToFreq(m.binIndex, view.sampleRateHz, n);
    float fy = db[m.binIndex];
    ImVec4 col = kMarkerColors[i];

    ImPlot::SetNextLineStyle(ImVec4(col.x, col.y, col.z, 0.5f), 1.0f);
    char lineId[24];
    std::snprintf(lineId, sizeof lineId, "##markerline%d", i);
    double lineX = fx;
    ImPlot::PlotInfLines(lineId, &lineX, 1);

    ImPlot::SetNextMarkerStyle(ImPlotMarker_Diamond, 7, col, 1.5f, col);
    char ptId[24];
    std::snprintf(ptId, sizeof ptId, "##markerpt%d", i);
    double px = fx, py = fy;
    ImPlot::PlotScatter(ptId, &px, &py, 1);

    char label[8];
    std::snprintf(label, sizeof label, "M%d", i + 1);
    ImPlot::Annotation(fx, fy, col, ImVec2(8, (i % 2 == 0) ? -16 : 16), true, "%s", label);
  }
}

void drawBandOnPlot(SpectrumViewState& view, double sampleRateHz, const std::vector<float>& db) {
  if (!view.bandEnabled) return;
  double lo = -sampleRateHz / 2.0, hi = sampleRateHz / 2.0;
  view.bandLoHz = std::clamp(view.bandLoHz, lo, hi);
  view.bandHiHz = std::clamp(view.bandHiHz, lo, hi);

  ImPlotRect lim = ImPlot::GetPlotLimits();
  double xs[2] = {std::min(view.bandLoHz, view.bandHiHz), std::max(view.bandLoHz, view.bandHiHz)};
  double ys1[2] = {lim.Y.Min, lim.Y.Min};
  double ys2[2] = {lim.Y.Max, lim.Y.Max};
  ImPlot::SetNextFillStyle(ImVec4(0.3f, 0.6f, 1.0f, 0.15f));
  ImPlot::PlotShaded("##band", xs, ys1, ys2, 2);

  ImVec4 edgeCol(0.3f, 0.6f, 1.0f, 0.8f);
  ImPlot::DragLineX(9001, &view.bandLoHz, edgeCol, 1.5f);
  ImPlot::DragLineX(9002, &view.bandHiHz, edgeCol, 1.5f);

  // Echo the band-power/peak readout (also shown in the controls above)
  // right under the band itself, so it's visible without looking away from
  // the plot -- especially useful right after a Shift+drag selection.
  if (!db.empty()) {
    BandStats st = computeBandStats(db, sampleRateHz, view.bandLoHz, view.bandHiHz);
    double cx = (xs[0] + xs[1]) / 2.0;
    ImPlot::Annotation(cx, lim.Y.Min, edgeCol, ImVec2(0, -6), true, "%.1f dBFS  pk %.1f dBFS", st.powerDb, st.peakDb);
  }
}

} // namespace

void plotSpectrum(const char* plotId, const std::vector<float>& db, double sampleRateHz, double centerFreqHz,
                   const std::vector<Sample>& timeDomain, SpectrumViewState& view) {
  bool fitRequested = false;

  if (db.empty()) view.hadData = false;
  const bool scaleChanged = view.sampleRateHz != sampleRateHz;
  if ((!view.hadData && !db.empty()) || scaleChanged) {
    fitRequested = true;
    view.fitSettleFrames = kFitSettleFrames;
  }
  view.hadData |= !db.empty();
  view.sampleRateHz = sampleRateHz;
  if (!db.empty() && view.fitSettleFrames > 0) {
    fitRequested = true;
    --view.fitSettleFrames;
  }

  if (scaleChanged) {
    // Accumulated dB-per-bin traces no longer correspond to a consistent
    // capture once the span they're plotted against changes.
    view.holdInit = false;
    view.holdDb.clear();
    view.peakHoldInit = false;
    view.peakHoldDb.clear();
    view.persistenceFrames.clear();
    view.lastSeenDb.clear();
  }

  // Marker/band/trace controls come first, then the zoom row, then the
  // plot itself -- so the controls that shape what the plot shows are read
  // top-to-bottom before the plot rather than sandwiched below the zoom row.
  drawMarkerControls(view, db, sampleRateHz, centerFreqHz);
  drawBandMarkerControls(view, db, sampleRateHz);
  drawTraceControls(view);
  updateAccumulators(view, db);

  fitRequested |= ImGui::Button("Fit signal");
  ImGui::SameLine();
  AxisZoomRequest zoomReq = drawAxisZoomButtons(view.zoom.valid && !db.empty());
  mergeZoomRequest(zoomReq, consumeWheelZoomRequest(view.zoom));
  ImGui::SameLine();
  ImGui::TextDisabled("Ctrl+click: place marker, Shift+drag: measure a range");
  ImGui::SameLine();
  HelpMarker(
      "Wheel -- zoom X\n"
      "Shift+wheel -- zoom Y\n"
      "Drag -- pan\n"
      "Double-click -- fit\n"
      "H+/H-/V+/V- -- zoom one axis\n"
      "Ctrl+click on the plot -- place the selected marker (M1..M4)\n"
      "Shift+drag on the plot -- set the band marker to the dragged range\n"
      "Right-click while dragging -- cancel the selection");

  SpectrumMeasurements measurements = computeMeasurements(db, sampleRateHz, timeDomain);

  ImGui::BeginChild("##spectrum_plot", ImVec2(-kSpectrumMeasurementsWidth - 8.0f, 220), false,
                     ImGuiWindowFlags_NoScrollbar);
  if (ImPlot::BeginPlot(plotId, ImVec2(-1, 220))) {
    ImPlot::SetupAxes("Frequency (Hz, baseband)", "Power (dBFS)");
    if (!db.empty()) {
      auto [minIt, maxIt] = std::minmax_element(db.begin(), db.end());
      softenAxisToData(ImAxis_X1, -sampleRateHz / 2.0, sampleRateHz / 2.0, view.zoom, 0.02);
      softenAxisToData(ImAxis_Y1, *minIt, *maxIt, view.zoom, 0.1);
      if (fitRequested) {
        // X (frequency) fits flush; Y gets the same margin as the pan
        // limit above so the curve isn't drawn right up to the border.
        ImPlot::SetupAxisLimits(ImAxis_X1, -sampleRateHz / 2.0, sampleRateHz / 2.0, ImPlotCond_Always);
        fitAxisWithMargin(ImAxis_Y1, *minIt, *maxIt, 0.1);
      }
    }
    if (!fitRequested) applyAxisZoom(zoomReq, view.zoom);
    if (!db.empty()) {
      int n = static_cast<int>(db.size());
      double xscale = sampleRateHz / n;
      double xstart = -sampleRateHz / 2.0;

      drawPersistenceFrames(view, sampleRateHz);

      bool useHold = view.traceMode != SpectrumTraceMode::Live && view.holdInit && view.holdDb.size() == db.size();
      if (useHold) {
        // Dim raw live sweep behind the hold trace for context.
        ImPlot::SetNextLineStyle(ImVec4(0.6f, 0.6f, 0.6f, 0.35f), 1.0f);
        ImPlot::PlotLine("##live", db.data(), n, xscale, xstart);
        ImPlot::PlotLine("Spectrum", view.holdDb.data(), n, xscale, xstart);
      } else {
        ImPlot::PlotLine("Spectrum", db.data(), n, xscale, xstart);
      }

      if (view.peakHoldEnabled && view.peakHoldInit && view.peakHoldDb.size() == db.size()) {
        ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.55f, 0.1f, 0.9f), 1.5f);
        ImPlot::PlotLine("Peak hold", view.peakHoldDb.data(), n, xscale, xstart);
      }

      // Shift+drag selects a frequency range to measure -- reuse the band
      // marker for it (same power/peak readout, just set from the drag
      // instead of the Lo/Hi fields or dragging its edges by hand).
      RangeSelection rangeSel = readRangeSelection();
      if (rangeSel.active) {
        view.bandEnabled = true;
        view.bandInit = true;
        view.bandLoHz = rangeSel.loX;
        view.bandHiHz = rangeSel.hiX;
      }

      drawBandOnPlot(view, sampleRateHz, db);
      drawMarkersOnPlot(view, db);
      updateMarkerPlacement(view, db, sampleRateHz);
    }
    captureWheelZoomRequest(view.zoom);
    captureAxisZoomState(view.zoom);
    ImPlot::EndPlot();
  }
  ImGui::EndChild();

  ImGui::SameLine();
  ImGui::BeginChild("##spectrum_measurements", ImVec2(kSpectrumMeasurementsWidth, 220), true);
  drawMeasurementsTable(measurements);
  ImGui::EndChild();
}

} // namespace iqforge

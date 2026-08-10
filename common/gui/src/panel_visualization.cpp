#include "panel_visualization.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "measurements.h"
#include "plot_constellation.h"
#include "plot_format.h"
#include "plot_instfreq.h"
#include "plot_phase.h"
#include "plot_spectrum.h"
#include "plot_time_domain.h"
#include "plot_trigger.h"
#include "plot_waterfall.h"
#include "plot_zoom_controls.h"

namespace iqforge {

namespace {
struct VisualizationTabState {
  bool showSpectrum = true;
  bool showWaterfall = true;
  bool showIQ = true;
  bool showPhase = true;
  bool showInstFreq = true;
  bool showConstellation = false;

  SpectrumViewState spectrumView;
  WaterfallViewState waterfallView;
  // I/Q, Phase, and Instantaneous Frequency all derive from the same
  // triggered sample window, so they share one trigger (and one set of
  // trigger controls) instead of each independently re-deriving it. They
  // also share one X-axis zoom/pan link and one sample-selection cursor, so
  // zooming or Ctrl+clicking a sample in any one of them is reflected in
  // the other two. Constellation derives from the same triggered window too,
  // but plots I against Q rather than against sample index, so it keeps its
  // own fit/zoom state (ConstellationViewState) instead of joining that link.
  TriggerState trigger;
  TimeDomainViewState iqView;
  PhaseViewState phaseView;
  InstFreqViewState instFreqView;
  ConstellationViewState constellationView;
  SharedXAxisLink timeDomainXLink;
  TimeMarkerState timeDomainMarkers;
  TimeRangeSelection timeDomainRangeSelection;
  // Last signalGeneration seen by drawVisualizationWindow(); -1 so the very
  // first frame (generation starts at 0) also counts as "changed" and gets
  // the initial fit for free, same as the hadData transition below.
  int lastSignalGeneration = -1;
};

// Reported back to the caller (which owns AppState/the device) when a
// spectrum marker's "-> Center freq" button was clicked, since this
// function only ever sees raw fields, not AppState itself.
struct VisualizationRequest {
  bool retuneRequested = false;
  double retuneToHz = 0.0;
};

// Marker controls for the I/Q/phase/inst.-freq family, mirroring
// drawMarkerControls() in plot_spectrum.cpp: M1..M4 selection, Clear, and a
// Delta-ref combo -- except the delta readout here is dt/1-over-dt/
// dAmplitude between the selected marker and its reference, generalizing
// the old fixed cursor-A/cursor-B period measurement to any pair of markers.
void drawTimeMarkerControls(TimeMarkerState& state, const Sample* data, size_t count, double sampleRateHz) {
  for (int i = 0; i < kMaxTimeMarkers; ++i) {
    char label[16];
    std::snprintf(label, sizeof label, "M%d%s", i + 1, state.markers[i].active ? "" : " (off)");
    bool isSel = (state.selectedMarker == i);
    if (isSel) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    bool clicked = ImGui::Button(label);
    if (isSel) ImGui::PopStyleColor();
    if (clicked) state.selectedMarker = i;
    if (i + 1 < kMaxTimeMarkers) sameLineOrWrap(wrapButtonWidth("M4 (off)"));
  }

  TimeMarker& sel = state.markers[state.selectedMarker];
  sameLineOrWrap(wrapButtonWidth("Clear"));
  if (ImGui::Button("Clear")) sel = TimeMarker{};

  char preview[8];
  std::snprintf(preview, sizeof preview, sel.deltaRef < 0 ? "Off" : "M%d", sel.deltaRef + 1);
  ImGui::SetNextItemWidth(90.0f);
  if (ImGui::BeginCombo("Delta ref", preview)) {
    if (ImGui::Selectable("Off", sel.deltaRef < 0)) sel.deltaRef = -1;
    for (int j = 0; j < kMaxTimeMarkers; ++j) {
      if (j == state.selectedMarker) continue;
      char item[8];
      std::snprintf(item, sizeof item, "M%d", j + 1);
      if (ImGui::Selectable(item, sel.deltaRef == j)) sel.deltaRef = j;
    }
    ImGui::EndCombo();
  }

  bool anyActive = false;
  for (int i = 0; i < kMaxTimeMarkers; ++i) {
    const TimeMarker& m = state.markers[i];
    if (!m.active || data == nullptr || static_cast<size_t>(m.index) >= count) continue;
    anyActive = true;
    Sample s = data[m.index];
    float mag = std::abs(s);
    double t = sampleRateHz > 0.0 ? static_cast<double>(m.index) / sampleRateHz : 0.0;
    ImGui::TextColored(timeMarkerColor(i), "M%d:", i + 1);
    ImGui::SameLine();
    ImGui::Text("sample %d (%s)   I=%.4g Q=%.4g |A|=%.4g", m.index, formatSeconds(t).c_str(), s.real(), s.imag(),
                mag);

    if (m.deltaRef >= 0 && m.deltaRef < kMaxTimeMarkers && state.markers[m.deltaRef].active) {
      const TimeMarker& r = state.markers[m.deltaRef];
      if (static_cast<size_t>(r.index) < count && sampleRateHz > 0.0) {
        double dt = std::abs(m.index - r.index) / sampleRateHz;
        double freq = dt > 0.0 ? 1.0 / dt : 0.0;
        float dAmp = mag - std::abs(data[r.index]);
        ImGui::SameLine();
        ImGui::TextDisabled("(delta vs M%d: dt=%s, 1/dt=%s, dAmplitude=%.4g)", m.deltaRef + 1,
                             formatSeconds(dt).c_str(), formatHz(freq).c_str(), dAmp);
      }
    }
  }
  if (!anyActive) {
    ImGui::TextDisabled("Ctrl+click a plot below to place the selected marker (M1..M4)");
  }
}

// RMS/peak/crest/DC/clipping for a Shift+drag range selection (see
// TimeRangeSelection in plot_line_view.h), reusing the same
// computeTimeDomainStats() the spectrum's measurements table uses for the
// whole buffer -- just scoped to the dragged sample span instead.
void drawTimeRangeReadout(const TimeRangeSelection& sel, const Sample* data, size_t count, double sampleRateHz) {
  if (!sel.active || count == 0) {
    ImGui::TextDisabled("Shift+drag a plot below to measure a sample range");
    return;
  }
  int lo = std::clamp(sel.loIndex, 0, static_cast<int>(count) - 1);
  int hi = std::clamp(sel.hiIndex, 0, static_cast<int>(count) - 1);
  if (hi < lo) std::swap(lo, hi);
  size_t segCount = static_cast<size_t>(hi - lo + 1);
  TimeDomainStats st = computeTimeDomainStats(data + lo, segCount);
  double dt = sampleRateHz > 0.0 ? static_cast<double>(segCount) / sampleRateHz : 0.0;
  ImGui::Text("Selection: samples %d..%d (%s)   RMS %.1f dBFS   Peak %.1f dBFS   Crest %.1f dB   DC %.1f dBFS   "
              "Clip %ld (%.2f%%)",
              lo, hi, formatSeconds(dt).c_str(), st.rmsDbFs, st.peakDbFs, st.crestFactorDb, st.dcOffsetDbFs,
              st.clippingCount, st.clippingPct);
}

VisualizationRequest drawVisualizationWindow(const char* windowTitle, VisualizationTabState& tab,
                                              const std::vector<Sample>& timeDomain,
                                              const std::vector<float>& spectrumDb,
                                              const std::deque<WaterfallRow>& waterfallRows, double sampleRateHz,
                                              double centerFreqHz, bool& frozen, int signalGeneration) {
  VisualizationRequest request;
  // The signal itself (not just the sample window sliding forward) changed
  // discontinuously since last frame -- e.g. a new file was loaded or the TX
  // generator's config was edited -- so force every plot below to re-fit
  // instead of keeping whatever zoom/scale was left over from the previous
  // signal.
  bool signalChanged = tab.lastSignalGeneration != signalGeneration;
  tab.lastSignalGeneration = signalGeneration;
  if (signalChanged) tab.spectrumView.hadData = false;

  ImGui::Begin(windowTitle);

  ImGui::Checkbox("Spectrum", &tab.showSpectrum);
  sameLineOrWrap(wrapButtonWidth("Waterfall"));
  ImGui::Checkbox("Waterfall", &tab.showWaterfall);
  sameLineOrWrap(wrapButtonWidth("I/Q"));
  ImGui::Checkbox("I/Q", &tab.showIQ);
  sameLineOrWrap(wrapButtonWidth("Phase"));
  ImGui::Checkbox("Phase", &tab.showPhase);
  sameLineOrWrap(wrapButtonWidth("Inst. freq"));
  ImGui::Checkbox("Inst. freq", &tab.showInstFreq);
  sameLineOrWrap(wrapButtonWidth("Constellation"));
  ImGui::Checkbox("Constellation", &tab.showConstellation);
  sameLineOrWrap(wrapButtonWidth("Freeze"));
  ImGui::Checkbox("Freeze", &frozen);
  if (frozen) {
    sameLineOrWrap(wrapButtonWidth("(display paused -- RX/TX itself keeps running)"));
    ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.0f), "(display paused -- RX/TX itself keeps running)");
  }
  ImGui::Separator();

  // Each section gets its own ID scope: "Fit signal"/"H+"/"H-"/"V+"/"V-"
  // are drawn once per section, and without this they'd collide (same
  // labels, same window) once more than one section is visible at a time.
  if (tab.showSpectrum) {
    ImGui::SeparatorText("Spectrum");
    ImGui::PushID("spectrum");
    plotSpectrum("##spectrum", spectrumDb, sampleRateHz, centerFreqHz, timeDomain, tab.spectrumView);
    if (tab.spectrumView.centerFreqRequested) {
      tab.spectrumView.centerFreqRequested = false;
      request.retuneRequested = true;
      request.retuneToHz = tab.spectrumView.requestedCenterFreqHz;
    }
    ImGui::PopID();
  }

  if (tab.showWaterfall) {
    ImGui::SeparatorText("Waterfall");
    ImGui::PushID("waterfall");
    plotWaterfall("##waterfall", waterfallRows, sampleRateHz, tab.waterfallView);
    ImGui::PopID();
  }

  if (tab.showIQ || tab.showPhase || tab.showInstFreq || tab.showConstellation) {
    ImGui::SeparatorText("Time domain");
    ImGui::PushID("timedomain");
    bool resetFromTrigger = drawTriggerControls(tab.trigger) || signalChanged;
    auto [triggeredData, triggeredCount] = applyTrigger(timeDomain, tab.trigger);

    drawTimeMarkerControls(tab.timeDomainMarkers, triggeredData, triggeredCount, sampleRateHz);
    drawTimeRangeReadout(tab.timeDomainRangeSelection, triggeredData, triggeredCount, sampleRateHz);

    if (tab.showIQ) {
      ImGui::Text("I/Q");
      ImGui::PushID("iq");
      plotIQLines("##iq", triggeredData, triggeredCount, tab.iqView, resetFromTrigger, tab.timeDomainXLink,
                  tab.timeDomainMarkers, tab.timeDomainRangeSelection, tab.trigger);
      ImGui::PopID();
    }
    if (tab.showPhase) {
      ImGui::Text("Phase");
      ImGui::PushID("phase");
      plotPhaseLine("##phase", triggeredData, triggeredCount, tab.phaseView, resetFromTrigger, tab.timeDomainXLink,
                    tab.timeDomainMarkers, tab.timeDomainRangeSelection);
      ImGui::PopID();
    }
    if (tab.showInstFreq) {
      ImGui::Text("Instantaneous frequency");
      ImGui::PushID("instfreq");
      plotInstFreqLine("##instfreq", triggeredData, triggeredCount, sampleRateHz, tab.instFreqView, resetFromTrigger,
                        tab.timeDomainXLink, tab.timeDomainMarkers, tab.timeDomainRangeSelection);
      ImGui::PopID();
    }
    if (tab.showConstellation) {
      ImGui::Text("Constellation");
      ImGui::PushID("constellation");
      plotConstellation("##constellation", triggeredData, triggeredCount, tab.constellationView, resetFromTrigger);
      ImGui::PopID();
    }
    ImGui::PopID();
  }

  ImGui::End();
  return request;
}

// Applies a spectrum marker's "-> Center freq" request: retunes the shared
// device state and, if a device is connected, the hardware itself -- same
// pattern panel_device.cpp uses when its own frequency field changes.
void applyCenterFreqRetune(AppState& state, double hz) {
  state.centerFreqHz = hz;
  IDevice* dev = state.deviceManager.device();
  if (!dev) return;
  if (!dev->setFrequency(hz)) {
    state.log("Marker -> center freq: rejected by device");
  } else {
    state.log("Marker -> center freq: tuned to " + formatHz(hz));
  }
}
} // namespace

void drawTxVisualizationPanel(AppState& state) {
  static VisualizationTabState tab;
  VisualizationRequest req =
      drawVisualizationWindow("TX", tab, state.txTimeDomain, state.txSpectrumDb, state.txWaterfallRows,
                               state.sampleRateHz, state.centerFreqHz, state.txFrozen, state.txSignalGeneration);
  if (req.retuneRequested) applyCenterFreqRetune(state, req.retuneToHz);
}

void drawRxVisualizationPanel(AppState& state) {
  static VisualizationTabState tab;
  VisualizationRequest req =
      // RX has no discrete "signal changed" event of its own (it's a live
      // device stream) -- 0 is a constant, so this never forces a re-fit
      // beyond the initial one already handled by the hadData transition.
      drawVisualizationWindow("RX", tab, state.rxTimeDomain, state.rxSpectrumDb, state.rxWaterfallRows,
                               state.sampleRateHz, state.centerFreqHz, state.rxFrozen, 0);
  if (req.retuneRequested) applyCenterFreqRetune(state, req.retuneToHz);
}

void drawSignalViewerVisualizationPanel(AppState& state) {
  static VisualizationTabState tab;
  VisualizationRequest req =
      drawVisualizationWindow("Signal Viewer", tab, state.svTimeDomain, state.svSpectrumDb, state.svWaterfallRows,
                               state.svActiveRateHz, state.svCenterFreqHz, state.svFrozen,
                               state.svSignalGeneration);
  // No device to retune -- this mode works without one -- so a marker's
  // "-> Center freq" just updates the display reference used for the axis.
  if (req.retuneRequested) state.svCenterFreqHz = req.retuneToHz;
}

} // namespace iqforge

#include "panel_spectrum_viewer.h"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <cstdio>

#include "freq_input.h"
#include "plot_zoom_controls.h"

namespace iqforge {

namespace {
// One viewer, one persistent pan/zoom state -- unlike the per-direction
// spectrum plots, there's only ever one of these on screen.
AxisZoomState g_sweepZoom;

// Draws one band row: a select radio, an editable name, a toggle between
// entering Start/End or the equivalent Center/Span, and the two frequency
// fields for whichever mode is active. startFreqHz/endFreqHz are always the
// stored source of truth -- Center/Span is just a different view onto them,
// recomputed back into start/end the moment either field changes.
void drawBandRow(AppState& state, AppState::SweepBand& band, int index, int& pendingDeleteIndex) {
  ImGui::PushID(index);

  if (ImGui::RadioButton("##select", state.selectedSweepBand == index)) {
    state.selectedSweepBand = index;
  }
  ImGui::SameLine();
  HelpMarker("Select this band as the one Start sweep will use.");
  ImGui::SameLine();

  char nameBuf[64];
  std::snprintf(nameBuf, sizeof nameBuf, "%s", band.name.c_str());
  ImGui::SetNextItemWidth(140.0f);
  if (ImGui::InputText("##name", nameBuf, sizeof nameBuf)) {
    band.name = nameBuf;
  }

  ImGui::SameLine();
  int mode = band.editCenterSpan ? 1 : 0;
  const char* modeNames[] = {"Start / End", "Center / Span"};
  ImGui::SetNextItemWidth(120.0f);
  if (ImGui::Combo("##mode", &mode, modeNames, 2)) {
    band.editCenterSpan = (mode == 1);
  }

  ImGui::SameLine();
  if (ImGui::Button("Delete")) pendingDeleteIndex = index;

  if (!band.editCenterSpan) {
    FrequencyInputHz("Start", &band.startFreqHz, &band.startUnit);
    ImGui::SameLine();
    FrequencyInputHz("End", &band.endFreqHz, &band.endUnit);
  } else {
    double center = (band.startFreqHz + band.endFreqHz) / 2.0;
    double span = band.endFreqHz - band.startFreqHz;
    bool changed = FrequencyInputHz("Center", &center, &band.centerUnit);
    ImGui::SameLine();
    changed |= FrequencyInputHz("Span", &span, &band.spanUnit);
    if (changed) {
      band.startFreqHz = center - span / 2.0;
      band.endFreqHz = center + span / 2.0;
    }
  }

  ImGui::Separator();
  ImGui::PopID();
}
} // namespace

void drawSpectrumViewerPanel(AppState& state) {
  ImGui::Begin("SpectrumViewer");

  ImGui::TextUnformatted("Bands");
  ImGui::SameLine();
  HelpMarker(
      "Define one or more frequency ranges wider than the device can "
      "capture in one go. Select one and press Start sweep: the device "
      "retunes step by step across the whole range (step size = the "
      "current Sample rate, set in the Device panel) and the composite "
      "spectrum below fills in as each step completes.\n"
      "The sweep takes over RX -- if RX wasn't already running, Stop "
      "sweep stops it again; if it was already running, it's left alone.");

  int pendingDeleteIndex = -1;
  for (int i = 0; i < static_cast<int>(state.sweepBands.size()); ++i) {
    drawBandRow(state, state.sweepBands[static_cast<size_t>(i)], i, pendingDeleteIndex);
  }
  if (pendingDeleteIndex >= 0) {
    state.sweepBands.erase(state.sweepBands.begin() + pendingDeleteIndex);
    if (state.selectedSweepBand == pendingDeleteIndex) {
      state.selectedSweepBand = -1;
    } else if (state.selectedSweepBand > pendingDeleteIndex) {
      --state.selectedSweepBand;
    }
  }

  if (ImGui::Button("Add band")) {
    state.sweepBands.push_back(AppState::SweepBand{});
    if (state.selectedSweepBand < 0) state.selectedSweepBand = static_cast<int>(state.sweepBands.size()) - 1;
  }

  ImGui::Separator();

  bool haveSelection =
      state.selectedSweepBand >= 0 && state.selectedSweepBand < static_cast<int>(state.sweepBands.size());
  bool connected = state.deviceManager.isConnected();

  ImGui::BeginDisabled(state.sweepRunning || !haveSelection || !connected);
  if (ImGui::Button("Start sweep")) state.startSweep();
  ImGui::EndDisabled();
  sameLineOrWrap(wrapButtonWidth("Stop sweep"));
  ImGui::BeginDisabled(!state.sweepRunning);
  if (ImGui::Button("Stop sweep")) state.stopSweep();
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::TextDisabled("%s", state.sweepStatus.c_str());
  if (!connected) {
    ImGui::SameLine();
    ImGui::TextDisabled("(connect a device first)");
  }

  ImGui::Separator();

  bool fitRequested = ImGui::Button("Fit");

  if (state.sweepSpectrumDb.empty()) {
    ImGui::TextDisabled("No sweep data yet -- select a band above and press Start sweep.");
  } else if (ImPlot::BeginPlot("##sweepspectrum", ImVec2(-1, 400))) {
    ImPlot::SetupAxes("Frequency (Hz)", "Power (dBFS)");
    auto [minIt, maxIt] = std::minmax_element(state.sweepSpectrumDb.begin(), state.sweepSpectrumDb.end());
    softenAxisToData(ImAxis_X1, state.sweepRangeStartHz, state.sweepRangeEndHz, g_sweepZoom, 0.02);
    softenAxisToData(ImAxis_Y1, *minIt, *maxIt, g_sweepZoom, 0.1);
    if (fitRequested) {
      ImPlot::SetupAxisLimits(ImAxis_X1, state.sweepRangeStartHz, state.sweepRangeEndHz, ImPlotCond_Always);
      fitAxisWithMargin(ImAxis_Y1, *minIt, *maxIt, 0.1);
    } else {
      applyAxisZoom(consumeWheelZoomRequest(g_sweepZoom), g_sweepZoom);
    }
    int n = static_cast<int>(state.sweepSpectrumDb.size());
    double xscale = (state.sweepRangeEndHz - state.sweepRangeStartHz) / n;
    ImPlot::PlotLine("Spectrum", state.sweepSpectrumDb.data(), n, xscale, state.sweepRangeStartHz);
    captureWheelZoomRequest(g_sweepZoom);
    captureAxisZoomState(g_sweepZoom);
    ImPlot::EndPlot();
  }

  ImGui::End();
}

} // namespace iqforge

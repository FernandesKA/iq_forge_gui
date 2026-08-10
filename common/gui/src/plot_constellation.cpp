#include "plot_constellation.h"

#include <imgui.h>
#include <implot.h>

#include <algorithm>

namespace iqforge {

void plotConstellation(const char* plotId, const Sample* data, size_t count, ConstellationViewState& view,
                        bool resetView) {
  bool fitRequested = ImGui::Button("Fit signal");
  ImGui::SameLine();
  AxisZoomRequest zoomReq = drawAxisZoomButtons(view.zoom.valid && count > 0);
  mergeZoomRequest(zoomReq, consumeWheelZoomRequest(view.zoom));
  ImGui::SameLine();
  HelpMarker(
      "Wheel -- zoom X\n"
      "Shift+wheel -- zoom Y\n"
      "Drag -- pan\n"
      "Double-click -- fit\n"
      "H+/H-/V+/V- -- zoom one axis");

  bool hasData = count > 0;
  if (!hasData) view.hadData = false;
  if ((!view.hadData && hasData) || resetView) {
    fitRequested = true;
    view.fitSettleFrames = kFitSettleFrames;
  }
  // See drawLineView() in plot_line_view.h for why re-fitting continues
  // while `count` is still changing and for a settle window after that --
  // same reasoning applies here (a just-loaded/just-started signal's I/Q
  // range isn't representative yet on its very first frame).
  if (hasData && count != view.lastCount) fitRequested = true;
  view.lastCount = count;
  if (hasData && view.fitSettleFrames > 0) {
    fitRequested = true;
    --view.fitSettleFrames;
  }
  view.hadData |= hasData;

  if (ImPlot::BeginPlot(plotId, ImVec2(-1, 260), ImPlotFlags_Equal)) {
    ImPlot::SetupAxes("I", "Q");
    if (hasData) {
      float lo = data[0].real(), hi = data[0].real();
      for (size_t i = 0; i < count; ++i) {
        lo = std::min({lo, data[i].real(), data[i].imag()});
        hi = std::max({hi, data[i].real(), data[i].imag()});
      }
      // Same range on both axes (not each fit to its own I/Q span) so a
      // unit-amplitude signal reads as a circle, not a distorted ellipse.
      constrainAxisToData(ImAxis_X1, lo, hi, 0.1);
      constrainAxisToData(ImAxis_Y1, lo, hi, 0.1);
      if (fitRequested) {
        fitAxisWithMargin(ImAxis_X1, lo, hi, 0.1);
        fitAxisWithMargin(ImAxis_Y1, lo, hi, 0.1);
      }
    }
    if (!fitRequested) applyAxisZoom(zoomReq, view.zoom);
    if (hasData) {
      const float* base = reinterpret_cast<const float*>(data);
      int n = static_cast<int>(count);
      ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 2.5f);
      ImPlot::PlotScatter("##constellation", base, base + 1, n, 0, 0, sizeof(Sample));
    }
    captureWheelZoomRequest(view.zoom);
    captureAxisZoomState(view.zoom);
    ImPlot::EndPlot();
  }
}

} // namespace iqforge

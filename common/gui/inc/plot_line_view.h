#pragma once

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>

#include "plot_zoom_controls.h"

namespace iqforge {

struct LineViewState {
  bool hadData = false;
  // Sentinel so the very first frame with data always counts as "changed"
  // below, triggering the initial fit.
  size_t lastCount = static_cast<size_t>(-1);
  AxisZoomState zoom;
  // See kFitSettleFrames in plot_zoom_controls.h.
  int fitSettleFrames = 0;
};

// X-axis range shared by a family of line views (I/Q, phase, inst. freq)
// so that zooming/panning one keeps the others showing the same sample
// span. Bound to each plot via ImPlot::SetupAxisLinks(), which pulls this
// range in at the start of each plot's setup and pushes the plot's final
// (possibly drag-panned/zoomed) range back out at EndPlot -- so any zoom
// path (buttons, wheel, native drag-pan, native box-select) ends up synced
// here automatically without drawLineView needing to special-case each one.
// Defaults to a non-degenerate range so the first frame (before any data
// exists) doesn't hand ImPlot a zero-width axis.
struct SharedXAxisLink {
  double min = 0.0;
  double max = 1.0;
};

constexpr int kMaxTimeMarkers = 4;

// A single sample-index marker on a family of line views (I/Q, phase,
// instantaneous frequency), so placing one on any of them shows on all
// three. `index` is in the shared "Sample" domain (0..count-1 of the
// I/Q/phase window); the instantaneous-frequency view has one fewer point
// and simply skips drawing a marker when its index falls on that last,
// missing sample. Mirrors SpectrumMarker in plot_spectrum.h.
struct TimeMarker {
  bool active = false;
  int index = 0;
  // Reference marker index for a delta readout (this marker's time/sample
  // offset and amplitude shown relative to that marker), -1 = off.
  int deltaRef = -1;
};

struct TimeMarkerState {
  std::array<TimeMarker, kMaxTimeMarkers> markers{};
  int selectedMarker = 0;
};

// A Shift+drag range selection (see readRangeSelection() in
// plot_zoom_controls.h) shared across the I/Q/phase/inst.-freq family, like
// TimeMarkerState above -- dragging on any one of them measures the same
// sample range on all three. Indices are in the same shared "Sample" domain
// as TimeMarker.
struct TimeRangeSelection {
  bool active = false;
  int loIndex = 0;
  int hiIndex = 0;
};

// Colors shared by the vertical marker line here and the scatter
// point/annotation each plot_*.cpp draws at the marker's own y-value, so a
// marker reads as one thing across I/Q, phase and instantaneous frequency
// (and matches the spectrum's marker colors for consistency across the app).
inline const ImVec4& timeMarkerColor(int index) {
  static const ImVec4 kColors[kMaxTimeMarkers] = {
      ImVec4(1.0f, 1.0f, 0.2f, 1.0f),
      ImVec4(0.2f, 1.0f, 1.0f, 1.0f),
      ImVec4(1.0f, 0.4f, 1.0f, 1.0f),
      ImVec4(0.6f, 1.0f, 0.4f, 1.0f),
  };
  return kColors[index];
}

namespace detail {

// Captures a Ctrl+click on the plot as a new position for the currently
// selected marker. Must be called after the plot's axes are locked (e.g.
// after drawLines()) so GetPlotMousePos() is valid.
inline void updateMarkerPlacement(TimeMarkerState& state, size_t count) {
  if (count == 0 || !ImPlot::IsPlotHovered()) return;
  if (!ImGui::GetIO().KeyCtrl || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
  ImPlotPoint mouse = ImPlot::GetPlotMousePos();
  long idx = std::lround(mouse.x);
  idx = std::max<long>(0, std::min<long>(idx, static_cast<long>(count) - 1));
  TimeMarker& m = state.markers[state.selectedMarker];
  m.active = true;
  m.index = static_cast<int>(idx);
}

} // namespace detail

// Shared fit/zoom/pan-limit machinery for a "family of lines over a sample
// window" view -- I/Q, phase, and instantaneous frequency all take this
// shape. `count` is the number of x-positions (plotted at indices 0..count-1).
// Pass `resetView = true` the frame the underlying window changed
// discontinuously (e.g. trigger settings changed) so the plot re-fits
// instead of trying to smoothly zoom into what's now a different signal
// (this also drops any current sample selection, since it no longer
// corresponds to the same instant in the new window).
// `xLink` and `markers` are shared across the whole family (same instances
// passed to every call) so zoom/pan and marker placement stay in sync
// between them. `computeYRange(lo, hi)` fills the data's Y span;
// `drawLines()` issues the actual ImPlot::PlotLine call(s) and, if it wants
// to mark the current `markers` on its own series, may read them (they're
// updated by the time drawLines() runs). Call between BeginPlot()/EndPlot()
// is handled internally.
template <typename ComputeYRange, typename DrawLines>
void drawLineView(const char* plotId, const char* yLabel, size_t count, bool resetView, LineViewState& view,
                   SharedXAxisLink& xLink, TimeMarkerState& markers, TimeRangeSelection& rangeSel,
                   ComputeYRange&& computeYRange, DrawLines&& drawLines) {
  bool fitRequested = ImGui::Button("Fit signal");
  ImGui::SameLine();
  AxisZoomRequest zoomReq = drawAxisZoomButtons(view.zoom.valid && count > 0);
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
      "Ctrl+click -- place the selected marker (M1..M4)\n"
      "Shift+drag -- select a sample range to measure (RMS/peak/etc.)\n"
      "Right-click while dragging -- cancel the selection");

  bool hasData = count > 0;
  if (!hasData) view.hadData = false;
  if ((!view.hadData && hasData) || resetView) {
    fitRequested = true;
    view.fitSettleFrames = kFitSettleFrames;
  }
  // Keep re-fitting while `count` is still changing frame to frame -- e.g.
  // right after TX/RX starts, while the trigger's window (or the raw
  // rolling buffer, if the trigger is off) is still filling up from empty.
  // Fitting just once, on the very first frame any data exists, could lock
  // onto a tiny/incomplete initial batch (e.g. the first few samples of a
  // sine that hasn't swung negative yet) and never widen from there. Once
  // `count` stabilizes at its steady-state size, this stops firing and
  // manual zoom/pan takes over as usual.
  if (hasData && count != view.lastCount) fitRequested = true;
  view.lastCount = count;
  // Beyond that, keep re-fitting for a further settle window: `count`
  // stabilizing doesn't mean the *values* have -- e.g. a trigger-selected
  // window can hold steady in size while its amplitude is still ramping up
  // right after TX/RX starts.
  if (hasData && view.fitSettleFrames > 0) {
    fitRequested = true;
    --view.fitSettleFrames;
  }
  if (resetView) {
    for (TimeMarker& m : markers.markers) m.active = false;
    rangeSel.active = false;
  }
  view.hadData |= hasData;

  if (ImPlot::BeginPlot(plotId, ImVec2(-1, 220))) {
    ImPlot::SetupAxes("Sample", yLabel);
    ImPlot::SetupAxisLinks(ImAxis_X1, &xLink.min, &xLink.max);
    if (hasData) {
      double lo = 0.0, hi = 0.0;
      computeYRange(lo, hi);
      softenAxisToData(ImAxis_X1, 0.0, static_cast<double>(count - 1), view.zoom, 0.05);
      softenAxisToData(ImAxis_Y1, lo, hi, view.zoom, 0.1);
      if (fitRequested) {
        // Written to xLink directly (not just via SetupAxisLimits) so a
        // fit on this plot is immediately visible to its siblings too, even
        // within the same frame -- SetupAxisLinks() below only pulls *from*
        // xLink at Setup time, so waiting on ImPlot's own end-of-frame
        // link-sync to carry the fitted range over isn't reliable when more
        // than one sibling plot fits on the same frame.
        xLink.min = 0.0;
        xLink.max = static_cast<double>(count - 1);
        ImPlot::SetupAxisLimits(ImAxis_X1, xLink.min, xLink.max, ImPlotCond_Always);
        fitAxisWithMargin(ImAxis_Y1, lo, hi, 0.1);
      }
    }
    if (!fitRequested) applyAxisZoom(zoomReq, view.zoom);
    if (hasData) drawLines();
    if (hasData) detail::updateMarkerPlacement(markers, count);
    if (hasData) {
      for (int i = 0; i < kMaxTimeMarkers; ++i) {
        const TimeMarker& m = markers.markers[i];
        if (!m.active || static_cast<size_t>(m.index) >= count) continue;
        double cx = static_cast<double>(m.index);
        ImPlot::SetNextLineStyle(timeMarkerColor(i), 1.5f);
        char id[16];
        std::snprintf(id, sizeof id, "##marker_%d", i);
        ImPlot::PlotInfLines(id, &cx, 1);
      }
      RangeSelection sel = readRangeSelection();
      if (sel.active) {
        long lo = std::lround(sel.loX);
        long hi = std::lround(sel.hiX);
        rangeSel.active = true;
        rangeSel.loIndex = static_cast<int>(std::max<long>(0, lo));
        rangeSel.hiIndex = static_cast<int>(std::min<long>(static_cast<long>(count) - 1, hi));
      }
    }
    captureWheelZoomRequest(view.zoom);
    captureAxisZoomState(view.zoom);
    ImPlot::EndPlot();
  }
}

} // namespace iqforge

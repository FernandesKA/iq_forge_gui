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
  // A resetView request that arrived while the plot wasn't actually drawn
  // (scrolled out of view, collapsed window, hidden dock tab) -- held until
  // the next frame BeginPlot() returns true instead of being dropped, see
  // drawLineView() below.
  bool resetPending = false;
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
  // True once the user has taken over the shared X range by hand (H+/H-/wheel)
  // since the last signal/trigger reset or explicit "Fit signal". A sibling
  // that only now starts being drawn (scrolled into view, tab shown) must then
  // adopt the current min/max instead of running its own initial fit over them
  // and discarding the manual X zoom.
  bool xManual = false;
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
  if (fitRequested) {
    // "Fit signal" re-arms the shared X family: the fit below may write the
    // shared range again.
    xLink.xManual = false;
  }
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
  if (resetView) {
    // Latch the re-fit until the plot is actually drawn: a scrolled-out-of-
    // view (or otherwise hidden) plot returns false from BeginPlot() below,
    // and dropping the request there would leave it stuck showing the
    // previous signal's zoom/scale once it comes back.
    view.resetPending = true;
    // A new signal / trigger reset re-arms auto-fit for the whole shared X
    // family.
    xLink.xManual = false;
    for (TimeMarker& m : markers.markers) m.active = false;
    rangeSel.active = false;
  }

  if (ImPlot::BeginPlot(plotId, ImVec2(-1, 220))) {
    // Commit fit bookkeeping only now that the plot is really being drawn.
    // While it's scrolled out of view (or its dock tab/window is hidden)
    // BeginPlot() returns false with no axes to touch; consuming the one-time
    // fit there would leave the axis at ImPlot's empty default 0..1 once the
    // plot was scrolled back into view, with the actual sample data
    // (0..count-1) off-screen.
    //
    // Auto-fit is one-shot: it runs when this plot first gets data, when the
    // caller reports a discontinuous signal/window change, or while the
    // window size is still settling (e.g. TX/RX's rolling buffer filling up).
    // After that the view stays exactly where the user left it -- no
    // continuous "catch-up" re-fitting -- until "Fit signal"/double-click.
    if (!hasData) view.hadData = false;
    if ((!view.hadData && hasData) || view.resetPending || (hasData && count != view.lastCount)) {
      fitRequested = true;
      view.resetPending = false;
    }
    view.lastCount = count;
    view.hadData |= hasData;

    ImPlot::SetupAxes("Sample", yLabel);
    ImPlot::SetupAxisLinks(ImAxis_X1, &xLink.min, &xLink.max);
    if (hasData) {
      double lo = 0.0, hi = 0.0;
      computeYRange(lo, hi);
      if (fitRequested) {
        // Written to xLink directly (not just via SetupAxisLimits) so a
        // fit on this plot is immediately visible to its siblings too, even
        // within the same frame -- SetupAxisLinks() below only pulls *from*
        // xLink at Setup time, so waiting on ImPlot's own end-of-frame
        // link-sync to carry the fitted range over isn't reliable when more
        // than one sibling plot fits on the same frame.
        //
        // Skipped once the user has hand-zoomed the shared X range, so a
        // sibling that is only now being drawn (its own local "first data"
        // fit) can't overwrite that manual zoom. Y still fits, since it isn't
        // shared between the family members.
        if (!xLink.xManual) {
          xLink.min = 0.0;
          xLink.max = static_cast<double>(count - 1);
          ImPlot::SetupAxisLimits(ImAxis_X1, xLink.min, xLink.max, ImPlotCond_Always);
        }
        fitAxisWithMargin(ImAxis_Y1, lo, hi, 0.1);
      }
    }
    if (!fitRequested) {
      // The shared X range is now user-owned: later fits on a sibling view
      // leave it alone (see xLink.xManual).
      if (zoomReq.zoomInX || zoomReq.zoomOutX) xLink.xManual = true;
      applyAxisZoom(zoomReq, view.zoom);
    }
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

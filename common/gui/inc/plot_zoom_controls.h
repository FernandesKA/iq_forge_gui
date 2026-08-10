#pragma once

#include <imgui.h>
#include <implot.h>

#include <cmath>
#include <limits>

namespace iqforge {

// How many frames to keep force-re-fitting a plot's axes after its data
// first appears (or after a discontinuous reset), instead of fitting just
// once and leaving it. A plot's first block of data is often not
// representative yet -- spectrum averaging hasn't converged, a just-started
// TX/RX hasn't reached steady amplitude -- so fitting once, right then,
// tends to lock in an odd zoom that only "Fit signal" fixes afterward. This
// keeps re-fitting for a brief window instead, matching app.cpp's vsync
// frame rate (glfwSwapInterval(1)), so it settles on its own; ~0.75s.
constexpr int kFitSettleFrames = 45;

// Call right after drawing a widget to decide whether the *next* one
// (estimated width `nextWidth`, see wrapButtonWidth() below) should
// continue on the same line or wrap to a new one. Standard ImGui manual-
// wrapping idiom (see imgui_demo.cpp's "Wrapping" section) -- without it, a
// long SameLine() chain of buttons just overflows silently past the window
// edge on a narrower window/dock instead of reflowing.
inline void sameLineOrWrap(float nextWidth) {
  ImGuiStyle& style = ImGui::GetStyle();
  float windowVisibleX2 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
  float nextX2 = ImGui::GetItemRectMax().x + style.ItemSpacing.x + nextWidth;
  if (nextX2 < windowVisibleX2) ImGui::SameLine();
}

// Estimated on-screen width of a Button() with this label, for feeding into
// sameLineOrWrap() before the button itself is drawn.
inline float wrapButtonWidth(const char* label) {
  return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

// Compact "(?)" affordance that reveals `desc` in a tooltip on hover --
// same idiom as imgui_demo.cpp's HelpMarker(). Used to surface
// modifier-key controls (Ctrl+click, etc.) that don't fit comfortably as
// always-visible text without either cluttering the row or being easy to
// miss.
inline void HelpMarker(const char* desc) {
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
    ImGui::TextUnformatted(desc);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

struct AxisZoomRequest {
  bool zoomInX = false;
  bool zoomOutX = false;
  bool zoomInY = false;
  bool zoomOutY = false;
  // Plot-space point to keep fixed while zooming (e.g. the mouse cursor
  // during a wheel-zoom), so that spot stays under the cursor instead of
  // the view just growing/shrinking around its center. NaN ("not set",
  // the default -- what the H/V buttons leave it as) falls back to
  // centering on the current view, same as before.
  double anchorX = std::numeric_limits<double>::quiet_NaN();
  double anchorY = std::numeric_limits<double>::quiet_NaN();
};

struct AxisZoomState {
  bool valid = false;
  ImPlotRect limits{};
  // Wheel input is only known to be over *this* plot once ImPlot's setup
  // phase is locked (IsPlotHovered() forces that), which is too late in the
  // frame to still call SetupAxisLimits(). So it's captured post-lock (see
  // captureWheelZoomRequest(), called near EndPlot()) and applied at the
  // top of the *next* frame instead, the same one-frame-lag the H/V buttons
  // already rely on via `limits` above.
  AxisZoomRequest pendingWheel;
};

// Draws independent horizontal/vertical zoom buttons. Call before BeginPlot().
inline AxisZoomRequest drawAxisZoomButtons(bool enabled) {
  AxisZoomRequest req;
  ImGui::BeginDisabled(!enabled);
  req.zoomInX = ImGui::Button("H+");
  ImGui::SameLine();
  req.zoomOutX = ImGui::Button("H-");
  ImGui::SameLine();
  req.zoomInY = ImGui::Button("V+");
  ImGui::SameLine();
  req.zoomOutY = ImGui::Button("V-");
  ImGui::EndDisabled();
  return req;
}

// Pulls in the wheel-driven request captured last frame (see
// captureWheelZoomRequest below) and clears it. Call before BeginPlot(),
// alongside drawAxisZoomButtons(), and OR the result into that button
// request before applying either.
inline AxisZoomRequest consumeWheelZoomRequest(AxisZoomState& state) {
  AxisZoomRequest req = state.pendingWheel;
  state.pendingWheel = AxisZoomRequest{};
  return req;
}

inline void mergeZoomRequest(AxisZoomRequest& dst, const AxisZoomRequest& src) {
  dst.zoomInX |= src.zoomInX;
  dst.zoomOutX |= src.zoomOutX;
  dst.zoomInY |= src.zoomInY;
  dst.zoomOutY |= src.zoomOutY;
  if (src.zoomInX || src.zoomOutX) dst.anchorX = src.anchorX;
  if (src.zoomInY || src.zoomOutY) dst.anchorY = src.anchorY;
}

// Applies a zoom request against the previous frame's captured limits.
// Call after SetupAxes() and before EndPlot().
inline void applyAxisZoom(const AxisZoomRequest& req, const AxisZoomState& state) {
  if (!state.valid) return;
  constexpr double kZoomInFactor = 0.8;
  constexpr double kZoomOutFactor = 1.25;

  // Keeps `anchor` fixed in plot-space while the range scales by `factor`
  // (e.g. the point under the cursor stays under the cursor). Falls back
  // to the range's own center when no anchor was captured (button clicks).
  auto zoomAxis = [](ImAxis axis, const ImPlotRange& range, double factor, double anchor) {
    double pivot = std::isnan(anchor) ? (range.Min + range.Max) / 2.0 : anchor;
    double newMin = pivot - (pivot - range.Min) * factor;
    double newMax = pivot + (range.Max - pivot) * factor;
    ImPlot::SetupAxisLimits(axis, newMin, newMax, ImPlotCond_Always);
  };

  if (req.zoomInX) zoomAxis(ImAxis_X1, state.limits.X, kZoomInFactor, req.anchorX);
  if (req.zoomOutX) zoomAxis(ImAxis_X1, state.limits.X, kZoomOutFactor, req.anchorX);
  if (req.zoomInY) zoomAxis(ImAxis_Y1, state.limits.Y, kZoomInFactor, req.anchorY);
  if (req.zoomOutY) zoomAxis(ImAxis_Y1, state.limits.Y, kZoomOutFactor, req.anchorY);
}

// Captures the current plot limits as the baseline for the next zoom step.
// Call right before EndPlot().
inline void captureAxisZoomState(AxisZoomState& state) {
  state.limits = ImPlot::GetPlotLimits();
  state.valid = true;
}

// Captures mouse-wheel scrolling over the plot for next frame's zoom: no
// modifier zooms the X axis, Shift zooms the Y axis -- matching ImPlot's
// own convention where Shift already means "vertical" for box-select.
// ImPlot's built-in combined wheel-zoom must be disabled globally (see
// configurePlotWheelZoom()) or it fires at the same time and fights this.
// Call between BeginPlot() and EndPlot(), anywhere after the plot's own
// Setup*() calls (this forces the setup phase to lock, like PlotLine does).
inline void captureWheelZoomRequest(AxisZoomState& state) {
  state.pendingWheel = AxisZoomRequest{};
  if (!ImPlot::IsPlotHovered()) return;
  float wheel = ImGui::GetIO().MouseWheel;
  if (wheel == 0.0f) return;

  // Valid here because IsPlotHovered() above already forced the setup
  // phase to lock, so this frame's axis scale is established.
  ImPlotPoint mouse = ImPlot::GetPlotMousePos();

  bool vertical = ImGui::GetIO().KeyShift;
  bool zoomIn = wheel > 0.0f;
  if (vertical) {
    if (zoomIn) state.pendingWheel.zoomInY = true; else state.pendingWheel.zoomOutY = true;
    state.pendingWheel.anchorY = mouse.y;
  } else {
    if (zoomIn) state.pendingWheel.zoomInX = true; else state.pendingWheel.zoomOutX = true;
    state.pendingWheel.anchorX = mouse.x;
  }
}

// Call once at startup (after ImPlot::CreateContext()). Requires all four
// modifiers together for ImPlot's own built-in wheel-zoom (which always
// zooms both axes at once) so it never fires in practice, leaving plain
// wheel / Shift+wheel entirely to captureWheelZoomRequest() above.
inline void configurePlotWheelZoom() {
  ImPlot::GetInputMap().ZoomMod = ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiMod_Alt | ImGuiMod_Super;
}

// Call once at startup, alongside configurePlotWheelZoom(). Remaps ImPlot's
// box-selection gesture from its default (right-drag) to Shift+left-drag,
// so a Shift+drag on any plot reads as "select this range to measure" (see
// RangeSelection below) instead of colliding with the plain left-drag pan
// already bound everywhere in this app. ImPlot itself suppresses panning
// for the duration of a selection drag, so the two gestures never fight.
inline void configurePlotBoxSelect() {
  ImPlotInputMap& map = ImPlot::GetInputMap();
  map.Select = ImGuiMouseButton_Left;
  map.SelectMod = ImGuiMod_Shift;
  // Select and SelectCancel can't be the same button; right-click cancels
  // an in-progress selection instead (it still also opens the context menu
  // on a plain click, same as ImPlot's own default -- the two are already
  // meant to coexist on one button upstream).
  map.SelectCancel = ImGuiMouseButton_Right;
}

// A Shift+drag box selection on a plot's X axis, read via
// IsPlotSelected()/GetPlotSelection() (see configurePlotBoxSelect() above).
// Releasing the mouse zooms the plot to the selection -- ImPlot's own
// behavior for a confirmed selection -- which for a measurement gesture
// doubles as "and now show me that range up close", so it's left as-is
// rather than fought.
struct RangeSelection {
  bool active = false;
  double loX = 0.0;
  double hiX = 0.0;
};

// Call after the plot's axes are locked (e.g. after drawing the main
// series), before EndPlot().
inline RangeSelection readRangeSelection() {
  RangeSelection sel;
  if (!ImPlot::IsPlotSelected()) return sel;
  ImPlotRect r = ImPlot::GetPlotSelection();
  sel.active = true;
  sel.loX = r.X.Min;
  sel.hiX = r.X.Max;
  return sel;
}

// Stops panning/zooming (wheel, drag, or the H/V buttons above) from going
// past [lo, hi] -- the actual range spanned by what's drawn -- plus a small
// margin so the curve isn't flush against the plot border. Without this,
// zooming/panning out keeps going into blank space beyond the data. Call
// after SetupAxes(), before EndPlot(). No-op if the range is degenerate
// (e.g. a single-sample buffer).
inline void constrainAxisToData(ImAxis axis, double lo, double hi, double marginFrac = 0.05) {
  if (!(hi > lo)) return;
  double margin = (hi - lo) * marginFrac;
  ImPlot::SetupAxisLimitsConstraints(axis, lo - margin, hi + margin);
}

// Saturating "rubber band" response: maps a nonnegative overflow distance to
// a compressed distance that grows almost 1:1 near zero and flattens out as
// it approaches `capacity`, asymptotically never reaching it (e.g. an
// overflow of `capacity` itself compresses to half of `capacity`). Used by
// softenAxisToData() below so panning/zooming past a plot's data range meets
// resistance that builds the further out you go, rather than either an
// abrupt hard stop or unbounded drift into empty space.
inline double rubberBand(double overflow, double capacity) {
  if (capacity <= 0.0 || overflow <= 0.0) return 0.0;
  return capacity * overflow / (overflow + capacity);
}

// Soft alternative to constrainAxisToData() above: instead of a hard wall
// right at [lo, hi] plus a margin, panning/zooming is allowed to drift past
// that boundary with rubberBand() resistance instead of stopping dead or
// drifting unbounded -- a small nudge past the edge still moves close to
// 1:1, but wandering further off flattens out.
//
// `zoom` is the same AxisZoomState already threaded through a plot for
// wheel-zoom (see AxisZoomState/captureAxisZoomState()). ImPlot only
// reports the outcome of this frame's pan/wheel/box-select/button zoom once
// its own interaction handling runs, later in the frame -- too late to
// react to here -- so, like the wheel-zoom capture/apply pair, this reads
// back *last* frame's actual position (zoom.limits) and corrects at the top
// of Setup: one frame behind the raw interaction. Holding a drag (or
// mashing a zoom-out button) re-derives the correction fresh every frame,
// which is what gives the resistance its live, springy feel rather than a
// single after-the-fact snap-back. No-op before the first frame's position
// has been captured (zoom.valid == false) or for a degenerate
// (single-value) range. Call after SetupAxes(), before any Plot* call.
inline void softenAxisToData(ImAxis axis, double lo, double hi, const AxisZoomState& zoom, double marginFrac = 0.1) {
  if (!zoom.valid || !(hi > lo)) return;
  double margin = (hi - lo) * marginFrac;
  double softLo = lo - margin;
  double softHi = hi + margin;
  double capacity = hi - lo; // generous: up to about one more data-span's worth of drift
  const ImPlotRange& last = (axis == ImAxis_X1) ? zoom.limits.X : zoom.limits.Y;
  double newMin = last.Min;
  double newMax = last.Max;
  bool corrected = false;
  if (last.Min < softLo) {
    newMin = softLo - rubberBand(softLo - last.Min, capacity);
    corrected = true;
  }
  if (last.Max > softHi) {
    newMax = softHi + rubberBand(last.Max - softHi, capacity);
    corrected = true;
  }
  if (corrected) ImPlot::SetupAxisLimits(axis, newMin, newMax, ImPlotCond_Always);
}

// Fits an axis to exactly [lo, hi] plus a margin, for use when "fitting" a
// plot to its data. Unlike ImPlot's own SetNextAxesToFit(), which fits
// flush with the curve touching the plot border, this leaves breathing
// room -- the same margin constrainAxisToData() above uses for the pan/zoom
// limit, so a fully-fit view sits exactly at that limit. Call after
// SetupAxes(), before EndPlot(). Falls back to a small fixed margin around
// a degenerate (single-value) range so the view isn't zero-height.
inline void fitAxisWithMargin(ImAxis axis, double lo, double hi, double marginFrac = 0.1) {
  double margin = (hi > lo) ? (hi - lo) * marginFrac : 1.0;
  ImPlot::SetupAxisLimits(axis, lo - margin, hi + margin, ImPlotCond_Always);
}

} // namespace iqforge

#pragma once

#include <cstddef>

#include "plot_zoom_controls.h"
#include "sample_types.h"

namespace iqforge {

struct ConstellationViewState {
  bool hadData = false;
  size_t lastCount = static_cast<size_t>(-1);
  AxisZoomState zoom;
  // See kFitSettleFrames in plot_zoom_controls.h.
  int fitSettleFrames = 0;
};

// Draws an I/Q scatter (constellation) plot for an already-computed sample
// window -- the same triggered window the I/Q/phase/inst.-freq family uses
// (see plot_trigger.h). Unlike that family, this plots I against Q rather
// than against sample index, so its X and Y axes both span amplitude and it
// keeps its own fit/zoom state (AxisZoomState) instead of sharing a
// SharedXAxisLink with them. Pass `resetView = true` the frame the
// underlying window changed discontinuously (trigger settings changed, or
// the signal itself changed) so the plot re-fits instead of zooming into
// what's now a different signal.
void plotConstellation(const char* plotId, const Sample* data, size_t count, ConstellationViewState& view,
                        bool resetView);

} // namespace iqforge

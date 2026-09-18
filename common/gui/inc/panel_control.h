#pragma once

#include "app_state.h"

namespace iqforge {

// Single "Control" window whose content follows whichever main-area window
// (TX/RX/Signal Viewer/SpectrumViewer) the user last focused -- rather than
// separately-tabbed TX Control/RX Control/Signal Viewer Control windows the
// user has to click through independently of the main-area tabs. Works the
// same whether those windows are tabbed together or dragged apart into
// separate dock nodes (see AppState::lastActiveMainTab).
void drawControlPanel(AppState& state);

} // namespace iqforge

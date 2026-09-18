#include "panel_control.h"

#include <imgui.h>

#include "dock_tab_utils.h"
#include "panel_rx.h"
#include "panel_signal_viewer.h"
#include "panel_tx.h"

namespace iqforge {

void drawControlPanel(AppState& state) {
  // Prefer actual input focus over dock-tab selection: isTabActive() alone
  // can't tell "the user is looking at this window" apart from "this window
  // is merely alone in its own dock node" once TX/RX have been dragged out
  // of the shared tab well into separate, simultaneously-visible nodes --
  // in that arrangement a window alone in its own node is trivially always
  // "the selected tab" of that node, so the old tab-based check would lock
  // onto whichever one happens to be checked first, forever. Focus tracks
  // whichever window the user actually last clicked into, which works the
  // same whether they're tabbed together or side by side. If none of the
  // four is currently focused (e.g. the user is adjusting a widget right
  // here in Control), leave lastActiveMainTab as it was rather than
  // snapping back to a fixed default mid-edit.
  if (isWindowFocused("RX")) {
    state.lastActiveMainTab = AppState::MainDockTab::Rx;
  } else if (isWindowFocused("Signal Viewer")) {
    state.lastActiveMainTab = AppState::MainDockTab::SignalViewer;
  } else if (isWindowFocused("SpectrumViewer")) {
    state.lastActiveMainTab = AppState::MainDockTab::SpectrumViewer;
  } else if (isWindowFocused("TX")) {
    state.lastActiveMainTab = AppState::MainDockTab::Tx;
  }

  ImGui::Begin("Control");

  switch (state.lastActiveMainTab) {
    case AppState::MainDockTab::Rx:
      ImGui::SeparatorText("RX");
      drawRxControlContents(state);
      break;
    case AppState::MainDockTab::SignalViewer:
      ImGui::SeparatorText("Signal Viewer");
      drawSignalViewerControlContents(state);
      break;
    case AppState::MainDockTab::SpectrumViewer:
      ImGui::SeparatorText("SpectrumViewer");
      ImGui::TextDisabled("SpectrumViewer's controls live in that window -- nothing to show here.");
      break;
    case AppState::MainDockTab::Tx:
    default:
      ImGui::SeparatorText("TX");
      drawTxControlContents(state);
      break;
  }

  ImGui::End();
}

} // namespace iqforge

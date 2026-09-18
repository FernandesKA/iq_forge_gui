#include "dock_tab_utils.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace iqforge {

bool isWindowFocused(const char* windowName) {
  ImGuiWindow* window = ImGui::FindWindowByName(windowName);
  if (!window) return false;
  return GImGui->NavWindow == window;
}

} // namespace iqforge

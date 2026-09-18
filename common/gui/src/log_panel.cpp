#include "log_panel.h"

#include <imgui.h>

namespace iqforge {

void drawLogPanel(AppState& state) {
  ImGui::Begin("Log");
  {
    std::lock_guard<std::mutex> lock(state.logMutex);
    for (const auto& entry : state.logMessages) {
      if (entry.isError) {
        ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "%s", entry.text.c_str());
      } else {
        ImGui::TextUnformatted(entry.text.c_str());
      }
    }
  }
  if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
    ImGui::SetScrollHereY(1.0f);
  }
  ImGui::End();
}

} // namespace iqforge

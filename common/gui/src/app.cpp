#include "app.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h>
#include <implot.h>

#include <cstdio>

#include "app_settings.h"
#include "log_panel.h"
#include "panel_control.h"
#include "panel_device.h"
#include "panel_help.h"
#include "panel_settings.h"
#include "panel_spectrum_viewer.h"
#include "panel_visualization.h"
#include "plot_zoom_controls.h"

namespace iqforge {

namespace {
void glfwErrorCallback(int error, const char* description) {
  std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

// Builds the default docked layout on first run: Device and the single
// merged Control panel (panel_control.cpp -- its content follows whichever
// of TX/RX/Signal Viewer/SpectrumViewer the user last focused, so there's
// nothing further to tab-switch here) stacked in the left column, plots
// tabbed together in the main area, Log/Settings tabbed together along the
// bottom. Only runs once — after that the user's own arrangement (persisted
// in imgui.ini) takes over.
void buildDefaultLayout(ImGuiID dockspaceId) {
  ImGui::DockBuilderRemoveNode(dockspaceId);
  ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

  ImGuiID mainId = dockspaceId;
  ImGuiID left = ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Left, 0.28f, nullptr, &mainId);
  ImGuiID bottom = ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Down, 0.22f, nullptr, &mainId);

  ImGuiID leftTop = ImGui::DockBuilderSplitNode(left, ImGuiDir_Up, 0.3f, nullptr, &left);

  ImGui::DockBuilderDockWindow("Device", leftTop);
  ImGui::DockBuilderDockWindow("Control", left);
  ImGui::DockBuilderDockWindow("TX", mainId);
  ImGui::DockBuilderDockWindow("RX", mainId);
  ImGui::DockBuilderDockWindow("SpectrumViewer", mainId);
  ImGui::DockBuilderDockWindow("Signal Viewer", mainId);
  ImGui::DockBuilderDockWindow("Help", mainId);
  ImGui::DockBuilderDockWindow("Log", bottom);
  ImGui::DockBuilderDockWindow("Settings", bottom);

  ImGui::DockBuilderFinish(dockspaceId);
}
} // namespace

App::App() = default;

App::~App() {
  saveSessionSettings(state_);
  state_.deviceManager.disconnect();

  if (window_) {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window_);
    glfwTerminate();
  }
}

bool App::init() {
  glfwSetErrorCallback(glfwErrorCallback);
  if (!glfwInit()) return false;

  const char* glslVersion = "#version 130";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

  window_ = glfwCreateWindow(1280, 800, "IQ Forge", nullptr, nullptr);
  if (!window_) {
    glfwTerminate();
    return false;
  }
  glfwMakeContextCurrent(window_);
  glfwSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImPlot::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  configurePlotWheelZoom();
  configurePlotBoxSelect();

  ImGui::StyleColorsDark();

  ImGui_ImplGlfw_InitForOpenGL(window_, true);
  ImGui_ImplOpenGL3_Init(glslVersion);

  loadSessionSettings(state_); // no-op if auto-save is disabled or no prior session exists

  return true;
}

void App::run() {
  while (!glfwWindowShouldClose(window_)) {
    glfwPollEvents();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
    static bool layoutBuilt = false;
    if (!layoutBuilt) {
      layoutBuilt = true;
      buildDefaultLayout(dockspaceId);
    }

    state_.updateDisplays();

    drawDevicePanel(state_);
    drawControlPanel(state_);
    drawSettingsPanel(state_);
    // Each of these is a main-area tab the Settings panel can hide -- simply
    // not calling Begin() for a window this frame drops it from its dock
    // node's tab bar without losing its docked position for later.
    if (state_.showTxTab) drawTxVisualizationPanel(state_);
    if (state_.showRxTab) drawRxVisualizationPanel(state_);
    if (state_.showSignalViewerTab) drawSignalViewerVisualizationPanel(state_);
    if (state_.showSpectrumViewerTab) drawSpectrumViewerPanel(state_);
    drawHelpPanel(state_);
    drawLogPanel(state_);

    ImGui::Render();
    int displayW, displayH;
    glfwGetFramebufferSize(window_, &displayW, &displayH);
    glViewport(0, 0, displayW, displayH);
    glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glfwSwapBuffers(window_);
  }
}

} // namespace iqforge

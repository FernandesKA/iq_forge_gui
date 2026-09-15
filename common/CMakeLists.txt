add_library(iqforge_core STATIC
  dsp/src/signal_generator.cpp
  dsp/src/iq_file.cpp
  dsp/src/fft_processor.cpp
  dsp/src/resampler.cpp
  dsp/src/async_iq_load.cpp
  devices/src/pluto_device.cpp
  devices/src/hackrf_device.cpp
  devices/src/iq_forge_device.cpp
  devices/src/device_manager.cpp
  devices/src/device_scanner.cpp
)
# PUBLIC so consumers (iqforge below, iqforge_tests) can #include headers
# from these modules by bare filename without needing relative "../" paths.
target_include_directories(iqforge_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/core/inc
  ${CMAKE_CURRENT_SOURCE_DIR}/devices/inc
  ${CMAKE_CURRENT_SOURCE_DIR}/dsp/inc
)
target_link_libraries(iqforge_core PUBLIC
  PkgConfig::LIBIIO
  PkgConfig::LIBHACKRF
  PkgConfig::FFTW3F
  Threads::Threads
  samplerate
  nlohmann_json::nlohmann_json
)
if(WIN32)
  # IqForgeDevice (devices/src/iq_forge_device.cpp) uses winsock2 directly.
  target_link_libraries(iqforge_core PUBLIC ws2_32)
endif()

if(WIN32)
  set(IQFORGE_ICON_PATH "${PROJECT_SOURCE_DIR}/assets/iqforge.ico")
  configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/gui/resources/iqforge.rc.in
    ${CMAKE_CURRENT_BINARY_DIR}/iqforge.rc
    @ONLY
  )
  set(IQFORGE_ICON_RC ${CMAKE_CURRENT_BINARY_DIR}/iqforge.rc)
endif()

add_executable(iqforge
  main.cpp
  gui/src/app.cpp
  gui/src/app_state.cpp
  gui/src/app_settings.cpp
  gui/src/panel_settings.cpp
  gui/src/log_panel.cpp
  gui/src/freq_input.cpp
  gui/src/duration_input.cpp
  gui/src/file_browser.cpp
  gui/src/load_progress.cpp
  gui/src/panel_device.cpp
  gui/src/panel_control.cpp
  gui/src/panel_help.cpp
  gui/src/panel_tx.cpp
  gui/src/panel_rx.cpp
  gui/src/panel_signal_viewer.cpp
  gui/src/dock_tab_utils.cpp
  gui/src/panel_visualization.cpp
  gui/src/panel_spectrum_viewer.cpp
  gui/src/plot_time_domain.cpp
  gui/src/plot_spectrum.cpp
  gui/src/measurements.cpp
  gui/src/plot_waterfall.cpp
  gui/src/plot_phase.cpp
  gui/src/plot_instfreq.cpp
  gui/src/plot_constellation.cpp
  ${IQFORGE_ICON_RC}
)
target_include_directories(iqforge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/gui/inc)
target_link_libraries(iqforge PRIVATE iqforge_core implot imgui)

if(WIN32)
  # Without this, iqforge.exe links as a console-subsystem app, so launching
  # it (e.g. via the Start Menu shortcut) pops up a cmd.exe-style console
  # window alongside the GUI for no reason -- nothing in this app prints to
  # a console. WIN32_EXECUTABLE switches the subsystem to GUI; MSVC then
  # additionally needs the entry point pointed back at plain main()
  # (mainCRTStartup) since /SUBSYSTEM:WINDOWS otherwise defaults to expecting
  # WinMain, which main.cpp doesn't define.
  set_target_properties(iqforge PROPERTIES WIN32_EXECUTABLE TRUE)
  if(MSVC)
    target_link_options(iqforge PRIVATE /ENTRY:mainCRTStartup)
  endif()
endif()

install(TARGETS iqforge_core
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

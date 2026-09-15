#include "panel_device.h"

#include <imgui.h>

#include <cstdio>
#include <string>

#include "freq_input.h"
#include "plot_zoom_controls.h"

namespace iqforge {

namespace {
constexpr double kZero = 0.0;
constexpr double kMinAtten = -89.75;
constexpr double kHackrfTxMax = 47.0;
constexpr double kHackrfRxMax = 102.0;
constexpr double kPlutoRxMax = 77.0;

// Powers of two only -- FFTW accepts any size, but these are the sizes that
// actually benefit from its fast paths, and cover everything from a coarse/
// fast display down to fine frequency resolution.
constexpr int kFftSizeOptions[] = {256, 512, 1024, 2048, 4096, 8192, 16384, 32768};
constexpr int kNumFftSizeOptions = static_cast<int>(sizeof(kFftSizeOptions) / sizeof(kFftSizeOptions[0]));

const char* kUriHint(DeviceKind kind) {
  switch (kind) {
    case DeviceKind::PlutoSDR: return "usb: | usb:1.5.5 | ip:192.168.2.1 | ip:pluto.local (empty = usb:)";
    case DeviceKind::HackRF: return "serial number (empty = first device found)";
    case DeviceKind::IqForge: return "host[:port] of the board's iq_forge_app, e.g. 192.168.0.7 (port defaults to 7373)";
  }
  return "";
}
} // namespace

void drawDevicePanel(AppState& state) {
  ImGui::Begin("Device");

  bool connected = state.deviceManager.isConnected();

  ImGui::BeginDisabled(connected);
  int kind = static_cast<int>(state.selectedKind);
  if (ImGui::RadioButton("PlutoSDR", kind == 0) && kind != 0) {
    state.selectedKind = DeviceKind::PlutoSDR;
    state.txGainDb = kMinAtten;
    state.rxGainDb = kZero;
    state.rxGainMode = RxGainMode::AgcSlow;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("HackRF", kind == 1) && kind != 1) {
    state.selectedKind = DeviceKind::HackRF;
    state.txGainDb = kZero;
    state.rxGainDb = kZero;
    state.rxGainMode = RxGainMode::Manual; // no hardware AGC on HackRF
    state.txChannel = 0; // HackRF has only one TX chain
    state.rxChannel = 0; // HackRF has only one RX chain
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("IqForge", kind == 2) && kind != 2) {
    state.selectedKind = DeviceKind::IqForge;
    state.txChannel = 0;
    state.rxChannel = 0;
  }

  ImGui::InputText("URI / Serial", state.uriBuffer, sizeof(state.uriBuffer));
  ImGui::TextDisabled("%s", kUriHint(state.selectedKind));

  if (state.selectedKind == DeviceKind::PlutoSDR) {
    if (ImGui::RadioButton("TX1##txchannel", state.txChannel == 0)) state.txChannel = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("TX2##txchannel", state.txChannel == 1)) state.txChannel = 1;
    ImGui::SameLine();
    HelpMarker(
        "Which physical TX chain to transmit on. TX2 only works on units "
        "running the AD9361 in 2T2R/dual-channel mode -- stock single-"
        "channel Pluto firmware doesn't expose it and Connect will fail "
        "with an error if selected.");

    if (ImGui::RadioButton("RX1##rxchannel", state.rxChannel == 0)) state.rxChannel = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("RX2##rxchannel", state.rxChannel == 1)) state.rxChannel = 1;
    ImGui::SameLine();
    HelpMarker(
        "Which physical RX chain to receive on. RX2 only works on units "
        "running the AD9361 in 2T2R/dual-channel mode -- stock single-"
        "channel Pluto firmware doesn't expose it and Connect will fail "
        "with an error if selected.");
  }
  ImGui::EndDisabled();

  // Connect/Disconnect deliberately sits right here, immediately after the
  // fields that identify which device to connect to -- *before* Scan and
  // the frequency/gain controls below, so a long scan result list (or the
  // dock simply being sized for a shorter idle panel) can never push it
  // out of view (https://github.com/FernandesKA/iq_forge/issues/9). Its
  // logic doesn't depend on the widgets drawn after it: Connect reads
  // state.sampleRateHz/etc. directly, and the "already connected" live
  // push of changed frequency/gain further down runs independently of
  // where this button is drawn.
  if (!connected) {
    if (ImGui::Button("Connect")) {
      DeviceConfig cfg;
      cfg.kind = state.selectedKind;
      cfg.uri = state.uriBuffer;
      cfg.sampleRateHz = state.sampleRateHz;
      cfg.centerFreqHz = state.centerFreqHz;
      cfg.bandwidthHz = state.bandwidthHz;
      cfg.txGainDb = state.txGainDb;
      cfg.rxGainDb = state.rxGainDb;
      cfg.rxGainMode = state.rxGainMode;
      cfg.txChannel = state.txChannel;
      cfg.rxChannel = state.rxChannel;
      if (state.deviceManager.connect(cfg, state.connectError)) {
        state.log("Connected to " + state.deviceManager.device()->name());
        if (!state.connectError.empty()) {
          state.log("Warning: " + state.connectError);
          state.connectError.clear(); // logged, not a connect failure -- don't show as one after disconnect
        }
      } else {
        state.log("Connect failed: " + state.connectError);
      }
    }
  } else {
    ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "Connected: %s", state.deviceManager.device()->name().c_str());
    ImGui::SameLine();
    if (ImGui::Button("Disconnect")) {
      state.deviceManager.disconnect();
      state.log("Disconnected");
    }
  }
  if (!state.connectError.empty() && !connected) {
    ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "%s", state.connectError.c_str());
  }

  ImGui::Separator();

  bool isIqForge = state.selectedKind == DeviceKind::IqForge;

  ImGui::BeginDisabled(connected);
  if (!isIqForge) {
    if (ImGui::Button("Scan")) {
      state.scanResults = state.selectedKind == DeviceKind::PlutoSDR ? scanPlutoDevices() : scanHackrfDevices();
      state.log("Scan found " + std::to_string(state.scanResults.size()) + " device(s)");
    }
    ImGui::SameLine();
    ImGui::TextDisabled(state.selectedKind == DeviceKind::PlutoSDR
                             ? "Probes USB and the network (mDNS), like SDR++'s device list"
                             : "Lists HackRF units currently attached via USB");

    // A dropdown rather than an always-expanded list: with several devices
    // (multiple PlutoSDRs on the network, a handful of HackRF units, etc.) an
    // inline list ate up panel space and made the results hard to scan, so
    // it's collapsed behind a combo -- ImGui scrolls its popup on its own once
    // it has more entries than fit, same effect as the old list's own
    // scrolling but without permanently occupying that space.
    if (!state.scanResults.empty()) {
      // Preview shows the description matching whatever's currently in the
      // URI field, if any -- so the combo reflects Connect's actual target
      // even after picking a result (or typing a URI by hand) rather than
      // resetting to a generic label.
      const char* preview = "Select a scanned device...";
      for (const auto& d : state.scanResults) {
        if (d.uri == state.uriBuffer) {
          preview = d.description.c_str();
          break;
        }
      }
      if (ImGui::BeginCombo("Scan results", preview)) {
        for (const auto& d : state.scanResults) {
          ImGui::PushID(d.uri.c_str());
          bool isSelected = d.uri == state.uriBuffer;
          if (ImGui::Selectable(d.description.c_str(), isSelected)) {
            std::snprintf(state.uriBuffer, sizeof(state.uriBuffer), "%s", d.uri.c_str());
          }
          if (isSelected) ImGui::SetItemDefaultFocus();
          ImGui::PopID();
        }
        ImGui::EndCombo();
      }
    }
  } else {
    ImGui::TextDisabled("No scan for IqForge yet -- type the board's IP above");
  }
  ImGui::EndDisabled();

  ImGui::Separator();

  bool changed = false;
  if (!isIqForge) {
    changed |= FrequencyInputHz("Sample rate", &state.sampleRateHz, &state.sampleRateUnit);
  }
  // For IqForge this is the DDS's own output frequency (there's no separate
  // LO -- see iq_forge_device.h), pushed via the same setFrequency() call as
  // any other device kind below.
  changed |= FrequencyInputHz(isIqForge ? "DDS freq" : "Center freq", &state.centerFreqHz, &state.centerFreqUnit);
  if (!isIqForge) {
    changed |= FrequencyInputHz("Bandwidth", &state.bandwidthHz, &state.bandwidthUnit);
  }

  bool txGainChanged = false;
  bool gainModeChanged = false;
  bool rxGainChanged = false;
  if (!isIqForge) {
    txGainChanged = ImGui::SliderScalar(
        state.selectedKind == DeviceKind::HackRF ? "TX VGA gain (dB)" : "TX attenuation (dB)",
        ImGuiDataType_Double, &state.txGainDb,
        state.selectedKind == DeviceKind::HackRF ? &kZero : &kMinAtten,
        state.selectedKind == DeviceKind::HackRF ? &kHackrfTxMax : &kZero, "%.2f");

    // HackRF has no hardware AGC (see hackrf_device.h), so the mode picker
    // only makes sense for PlutoSDR -- HackRF stays permanently Manual.
    if (state.selectedKind == DeviceKind::PlutoSDR) {
      if (ImGui::RadioButton("Manual##rxgainmode", state.rxGainMode == RxGainMode::Manual)) {
        state.rxGainMode = RxGainMode::Manual;
        gainModeChanged = true;
      }
      ImGui::SameLine();
      if (ImGui::RadioButton("AGC slow", state.rxGainMode == RxGainMode::AgcSlow)) {
        state.rxGainMode = RxGainMode::AgcSlow;
        gainModeChanged = true;
      }
      ImGui::SameLine();
      if (ImGui::RadioButton("AGC fast", state.rxGainMode == RxGainMode::AgcFast)) {
        state.rxGainMode = RxGainMode::AgcFast;
        gainModeChanged = true;
      }
      ImGui::SameLine();
      ImGui::TextDisabled("RX gain mode");
    }

    // Gain is driven by the AD9361 itself under either AGC mode -- the slider
    // would just be misleading (and, per setRxGain(), any drag on it while
    // AGC is active gets silently ignored by the device anyway).
    ImGui::BeginDisabled(state.selectedKind == DeviceKind::PlutoSDR && state.rxGainMode != RxGainMode::Manual);
    rxGainChanged = ImGui::SliderScalar(
        "RX gain (dB)", ImGuiDataType_Double, &state.rxGainDb, &kZero,
        state.selectedKind == DeviceKind::HackRF ? &kHackrfRxMax : &kPlutoRxMax, "%.2f");
    ImGui::EndDisabled();
  } else {
    ImGui::TextDisabled("IqForge: sine-only DDS TX for now -- no RX, no gain controls yet");
  }

  if (connected && changed) {
    IDevice* dev = state.deviceManager.device();
    if (!isIqForge) {
      if (!dev->setSampleRate(state.sampleRateHz)) state.log("Sample rate rejected by device");
    }
    if (!dev->setFrequency(state.centerFreqHz)) state.log("Frequency rejected by device");
    if (!isIqForge) {
      if (!dev->setBandwidth(state.bandwidthHz)) state.log("Bandwidth rejected by device");
    }
  }
  if (connected && txGainChanged) state.deviceManager.device()->setTxGain(state.txGainDb);
  if (connected && gainModeChanged) {
    if (!state.deviceManager.device()->setRxGainMode(state.rxGainMode)) {
      state.log("RX gain mode rejected by device");
    }
  }
  if (connected && rxGainChanged) state.deviceManager.device()->setRxGain(state.rxGainDb);

  ImGui::Separator();

  int fftSizeIdx = 0;
  for (int i = 0; i < kNumFftSizeOptions; ++i) {
    if (kFftSizeOptions[i] == state.fftSize) {
      fftSizeIdx = i;
      break;
    }
  }
  char fftSizePreview[16];
  std::snprintf(fftSizePreview, sizeof fftSizePreview, "%d", kFftSizeOptions[fftSizeIdx]);
  ImGui::SetNextItemWidth(120.0f);
  if (ImGui::BeginCombo("FFT size", fftSizePreview)) {
    for (int i = 0; i < kNumFftSizeOptions; ++i) {
      char label[16];
      std::snprintf(label, sizeof label, "%d", kFftSizeOptions[i]);
      if (ImGui::Selectable(label, i == fftSizeIdx)) state.setFftSize(kFftSizeOptions[i]);
      if (i == fftSizeIdx) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  HelpMarker(
      "Number of points per FFT for the Spectrum and Waterfall views.\n"
      "Higher = finer frequency resolution but slower and more smeared in "
      "time; lower = faster and more time-responsive but coarser frequency "
      "resolution.\n"
      "Applies to both RX and TX and takes effect immediately -- this also "
      "clears the waterfall history since old rows no longer match the new "
      "size.");

  ImGui::End();
}

} // namespace iqforge

#include "panel_rx.h"

#include <imgui.h>

#include "freq_input.h"
#include "i_device.h"
#include "iq_file.h"

namespace iqforge {

namespace {
// "PlutoSDR (usb:1.5.5)" / "HackRF (0000...)" -- SigMF core:hw, empty if no
// device was ever connected this session.
std::string describeHardware(const AppState& state) {
  const IDevice* dev = state.deviceManager.device();
  if (!dev) return "";
  std::string hw = dev->name();
  if (state.uriBuffer[0] != '\0') hw += " (" + std::string(state.uriBuffer) + ")";
  return hw;
}

void drawAnnotationControls(AppState& state) {
  ImGui::SeparatorText("Annotations");
  ImGui::SetNextItemWidth(200.0f);
  ImGui::InputTextWithHint("##annotationLabel", "Label (optional)", state.rxAnnotationLabelBuffer,
                            sizeof(state.rxAnnotationLabelBuffer));
  ImGui::SameLine();
  if (ImGui::Button("Mark now")) {
    state.rxRecordAnnotations.push_back(AppState::RxAnnotation{
        static_cast<uint64_t>(state.rxRecordBuffer.size()), 1, state.rxAnnotationLabelBuffer});
    state.rxAnnotationLabelBuffer[0] = '\0';
  }
  ImGui::TextDisabled("Flags the current recording position as a SigMF annotation on save.");

  if (!state.rxRecordAnnotations.empty()) {
    int removeIndex = -1;
    for (int i = 0; i < static_cast<int>(state.rxRecordAnnotations.size()); ++i) {
      const auto& ann = state.rxRecordAnnotations[i];
      ImGui::PushID(i);
      ImGui::Text("sample %llu%s%s", static_cast<unsigned long long>(ann.sampleStart), ann.label.empty() ? "" : "  ",
                  ann.label.c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton("x")) removeIndex = i;
      ImGui::PopID();
    }
    if (removeIndex >= 0) {
      state.rxRecordAnnotations.erase(state.rxRecordAnnotations.begin() + removeIndex);
    }
  }
}

// Builds the SigMF metadata for the recording currently in rxRecordBuffer,
// capturing the device parameters/annotations as they are right now (i.e.
// at Save & clear time -- if the user retuned mid-recording, only the final
// values are reflected, same simplification as the rest of the app's single
// shared sampleRateHz).
SigmfMeta buildSigmfMeta(const AppState& state) {
  SigmfMeta meta;
  meta.sampleRateHz = state.sampleRateHz;
  meta.hasSampleRate = true;
  meta.recorder = "IQ Forge";
  meta.hw = describeHardware(state);
  meta.description = state.rxRecordDescriptionBuffer;

  SigmfCapture cap;
  cap.sampleStart = 0;
  cap.frequencyHz = state.rxCenterFreqHz;
  cap.hasFrequency = true;
  cap.datetime = isoTimestampNowUtc();
  meta.captures.push_back(cap);

  for (const auto& ann : state.rxRecordAnnotations) {
    SigmfAnnotation a;
    a.sampleStart = ann.sampleStart;
    a.sampleCount = ann.sampleCount;
    a.label = ann.label;
    meta.annotations.push_back(std::move(a));
  }
  return meta;
}
} // namespace

void drawRxControlContents(AppState& state) {
  bool connected = state.deviceManager.isConnected();
  bool active = state.isRxActive();
  bool isIqForge = state.selectedKind == DeviceKind::IqForge;

  if (!isIqForge) {
    // PlutoSDR has an independent RX LO, so this can genuinely differ from
    // the TX panel's frequency; HackRF has only one physical LO shared
    // between RX/TX, so its backend (see hackrf_device.cpp) keeps both in
    // lockstep regardless of which one is set here.
    if (FrequencyInputHz("RX center freq", &state.rxCenterFreqHz, &state.rxCenterFreqUnit)) {
      if (connected && !state.deviceManager.device()->setRxFrequency(state.rxCenterFreqHz)) {
        state.log("RX frequency rejected by device");
      }
      if (state.selectedKind != DeviceKind::PlutoSDR) state.txCenterFreqHz = state.rxCenterFreqHz;
    }
    ImGui::Separator();
  } else {
    ImGui::TextDisabled("IqForge: no RX support yet");
  }

  ImGui::BeginDisabled(!connected);
  if (!active) {
    if (ImGui::Button("Start RX")) {
      state.rxFft.resetAveraging();
      auto callback = [&state](const Sample* data, size_t count) {
        state.rxRing.pushLatest(SampleBuffer(data, data + count));
      };
      if (state.deviceManager.device()->startRx(callback, state.rxError)) {
        state.log("RX started");
      } else {
        state.log("RX start failed: " + state.rxError);
      }
    }
  } else {
    if (ImGui::Button("Stop RX")) {
      state.deviceManager.device()->stopRx();
      state.log("RX stopped");
    }
  }
  ImGui::EndDisabled();

  if (!state.rxError.empty()) {
    ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "%s", state.rxError.c_str());
  }

  ImGui::Separator();
  ImGui::Checkbox("Record to buffer", &state.rxRecording);

  int format = static_cast<int>(state.rxSaveFormat);
  ImGui::RadioButton("SigMF", &format, static_cast<int>(AppState::RxSaveFormat::Sigmf));
  ImGui::SameLine();
  ImGui::RadioButton("Raw CF32", &format, static_cast<int>(AppState::RxSaveFormat::Cf32Raw));
  state.rxSaveFormat = static_cast<AppState::RxSaveFormat>(format);

  ImGui::InputText("Path", state.rxRecordPathBuffer, sizeof(state.rxRecordPathBuffer));
  if (state.rxSaveFormat == AppState::RxSaveFormat::Sigmf) {
    size_t annCount = state.rxRecordAnnotations.size();
    std::string annCountText = annCount == 0 ? "no" : std::to_string(annCount);
    ImGui::TextDisabled("Writes <path>.sigmf-data + <path>.sigmf-meta (sample rate, frequency, %s annotation%s)",
                         annCountText.c_str(), annCount == 1 ? "" : "s");
    ImGui::InputTextWithHint("Description", "optional", state.rxRecordDescriptionBuffer,
                              sizeof(state.rxRecordDescriptionBuffer));
  } else {
    ImGui::TextDisabled("Raw interleaved float32 I/Q -- no sample rate/frequency/annotations stored");
  }

  ImGui::Text("Buffered samples: %zu", state.rxRecordBuffer.size());

  if (state.rxRecording || !state.rxRecordBuffer.empty()) {
    drawAnnotationControls(state);
  }

  ImGui::BeginDisabled(state.rxRecordBuffer.empty());
  if (ImGui::Button("Save & clear")) {
    try {
      std::string path = state.rxRecordPathBuffer;
      if (state.rxSaveFormat == AppState::RxSaveFormat::Sigmf) {
        std::string basePath = sigmfBasePath(path);
        saveSigmf(basePath, state.rxRecordBuffer.data(), state.rxRecordBuffer.size(), buildSigmfMeta(state));
        state.log("Saved SigMF recording to " + basePath + ".sigmf-data/.sigmf-meta");
      } else {
        saveIqFileCf32(path, state.rxRecordBuffer.data(), state.rxRecordBuffer.size());
        state.log("Saved recording to " + path);
      }
      state.rxRecordBuffer.clear();
      state.rxRecordAnnotations.clear();
    } catch (const std::exception& e) {
      state.log(std::string("Save failed: ") + e.what());
    }
  }
  ImGui::EndDisabled();
}

} // namespace iqforge

#include "panel_signal_viewer.h"

#include <imgui.h>

#include <algorithm>
#include <memory>

#include "async_iq_load.h"
#include "file_browser.h"
#include "freq_input.h"
#include "iq_file.h"
#include "load_progress.h"
#include "plot_format.h"

namespace iqforge {

void drawSignalViewerControlContents(AppState& state) {
  ImGui::TextDisabled("Preview, resample, and save IQ files -- no device connection needed.");
  ImGui::Separator();

  ImGui::InputText("File path", state.svFilePathBuffer, sizeof(state.svFilePathBuffer));
  ImGui::SameLine();
  bool browseClicked = ImGui::Button("Browse...");
  ImGui::Checkbox("Loop preview", &state.svLoop);
  ImGui::TextDisabled(
      "Formats: .cf32/.fc32 (float32 IQ), .ci16/.sc16 (int16 IQ), .wav (PCM16), .sigmf-data/.sigmf-meta (SigMF)");

  // Raw formats don't carry their own sample rate -- same manual-entry
  // pattern as TX Control's file source (see fileSourceRateHz there), except
  // there's no device rate to compare against since this mode never
  // transmits/receives anything.
  ImGui::Checkbox("Resample on load", &state.svResampleEnabled);
  FrequencyInputHz("File sample rate", &state.svSourceRateHz, &state.svSourceRateUnit);
  if (state.svResampleEnabled) {
    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputDouble("Resample coefficient", &state.svResampleCoefficient, 0.0, 0.0, "%.6g");
    state.svResampleCoefficient = std::max(state.svResampleCoefficient, 1e-6);
    double resultingRateHz = state.svSourceRateHz * state.svResampleCoefficient;
    ImGui::TextDisabled("Resulting rate: %s", formatHz(resultingRateHz).c_str());
  }

  // Loading (and especially resampling -- a sinc convolution over the whole
  // file, easily seconds for a large file or a large ratio) used to run
  // directly in this button handler and freeze the whole GUI with no
  // feedback until it finished. AsyncIqLoadJob runs it on a background
  // thread instead; Load just starts the job and this frame (and every
  // frame after, via the progress bar below) moves on.
  static AsyncIqLoadJob loadJob;
  bool jobRunning = loadJob.running();

  ImGui::BeginDisabled(jobRunning);
  if (ImGui::Button("Load")) {
    loadJob.start(state.svFilePathBuffer, state.svResampleEnabled, state.svSourceRateHz, state.svResampleCoefficient);
  }
  ImGui::EndDisabled();

  if (loadJob.finished()) {
    AsyncIqLoadJob::Result result = loadJob.take();
    if (result.success) {
      state.svSigmfInfo = result.sigmf;
      state.svCenterFreqHz = 0.0;
      if (result.sigmf && result.sigmf->hasSampleRate) {
        // See panel_tx.cpp's Load handler -- SigMF supplies the rate that
        // raw .cf32/.ci16 has no way to carry, so honor it over whatever was
        // last typed in.
        state.svSourceRateHz = result.sourceRateHz;
        state.log("SigMF: recovered sample rate " + formatHz(result.sourceRateHz));
      }
      if (result.sigmf && !result.sigmf->captures.empty() && result.sigmf->captures.front().hasFrequency) {
        state.svCenterFreqHz = result.sigmf->captures.front().frequencyHz;
      }
      state.svActiveRateHz = result.resultRateHz;
      if (state.svResampleEnabled) {
        state.log("Resampled IQ file from " + formatHz(result.sourceRateHz) + " to " + formatHz(result.resultRateHz) +
                   " (" + std::to_string(result.inputSampleCount) + " -> " + std::to_string(result.buffer.size()) +
                   " samples)");
      }

      state.svSource = std::make_shared<IQFileSource>(std::move(result.buffer), state.svLoop);
      state.svLoadedPath = state.svFilePathBuffer;
      state.svLoadError.clear();
      ++state.svSignalGeneration;
      state.log("Signal Viewer: loaded " + state.svLoadedPath + " (" +
                 std::to_string(state.svSource->totalSamples()) + " samples)");
    } else {
      state.svLoadError = result.errorMessage;
      state.svSource.reset();
      state.svLoadedPath.clear();
      state.svSigmfInfo.reset();
      state.log("Signal Viewer: load failed: " + state.svLoadError);
    }
  }

  bool loaded = state.svSource && state.svLoadedPath == state.svFilePathBuffer;
  if (jobRunning) {
    drawLoadProgressBar(loadJob);
  } else {
    ImGui::PushTextWrapPos(0.0f);
    if (loaded) {
      ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "Loaded: %zu samples @ %s", state.svSource->totalSamples(),
                          formatHz(state.svActiveRateHz).c_str());
      if (state.svSigmfInfo) {
        const SigmfMeta& meta = *state.svSigmfInfo;
        if (!meta.hw.empty()) ImGui::TextDisabled("SigMF hw: %s", meta.hw.c_str());
        if (!meta.description.empty()) ImGui::TextDisabled("SigMF description: %s", meta.description.c_str());
        if (!meta.annotations.empty()) {
          ImGui::TextDisabled("SigMF annotations (%zu):", meta.annotations.size());
          for (const auto& ann : meta.annotations) {
            ImGui::BulletText("sample %llu (%llu samples)%s%s", static_cast<unsigned long long>(ann.sampleStart),
                               static_cast<unsigned long long>(ann.sampleCount), ann.label.empty() ? "" : "  ",
                               ann.label.c_str());
          }
        }
      }
    } else if (!state.svLoadError.empty()) {
      ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "%s", state.svLoadError.c_str());
    } else {
      ImGui::TextDisabled("Not loaded -- click Load to preview the signal/spectrum.");
    }
    ImGui::PopTextWrapPos();
  }

  DrawIqFileBrowserPopup("Select IQ file##sv", browseClicked, state.svFilePathBuffer, sizeof(state.svFilePathBuffer));

  ImGui::Separator();
  ImGui::SeparatorText("Save");

  int format = static_cast<int>(state.svSaveFormat);
  ImGui::RadioButton("SigMF##svformat", &format, static_cast<int>(AppState::SvSaveFormat::Sigmf));
  ImGui::SameLine();
  ImGui::RadioButton("Raw CF32##svformat", &format, static_cast<int>(AppState::SvSaveFormat::Cf32Raw));
  state.svSaveFormat = static_cast<AppState::SvSaveFormat>(format);

  ImGui::InputText("Save path", state.svSavePathBuffer, sizeof(state.svSavePathBuffer));
  if (state.svSaveFormat == AppState::SvSaveFormat::Sigmf) {
    ImGui::TextDisabled("Writes <path>.sigmf-data + <path>.sigmf-meta (sample rate %s)",
                         formatHz(state.svActiveRateHz).c_str());
  } else {
    ImGui::TextDisabled("Raw interleaved float32 I/Q -- no sample rate stored");
  }
  ImGui::TextDisabled("Writes whatever is currently loaded above -- resample on load first if you want a converted rate.");

  ImGui::BeginDisabled(!loaded);
  if (ImGui::Button("Save")) {
    try {
      std::string path = state.svSavePathBuffer;
      const SampleBuffer& data = state.svSource->data();
      if (state.svSaveFormat == AppState::SvSaveFormat::Sigmf) {
        std::string basePath = sigmfBasePath(path);
        SigmfMeta meta;
        meta.sampleRateHz = state.svActiveRateHz;
        meta.hasSampleRate = true;
        meta.recorder = "IQ Forge";
        if (state.svCenterFreqHz != 0.0) {
          SigmfCapture cap;
          cap.frequencyHz = state.svCenterFreqHz;
          cap.hasFrequency = true;
          cap.datetime = isoTimestampNowUtc();
          meta.captures.push_back(cap);
        }
        saveSigmf(basePath, data.data(), data.size(), meta);
        state.log("Signal Viewer: saved SigMF to " + basePath + ".sigmf-data/.sigmf-meta");
      } else {
        saveIqFileCf32(path, data.data(), data.size());
        state.log("Signal Viewer: saved to " + path);
      }
    } catch (const std::exception& e) {
      state.log(std::string("Signal Viewer: save failed: ") + e.what());
    }
  }
  ImGui::EndDisabled();
}

} // namespace iqforge

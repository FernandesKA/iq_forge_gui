#include "panel_tx.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "async_iq_load.h"
#include "iq_file.h"
#include "duration_input.h"
#include "file_browser.h"
#include "freq_input.h"
#include "load_progress.h"
#include "plot_format.h"

namespace iqforge {

namespace {
const char* kWaveformNames[] = {"Tone", "Multi-tone", "Chirp / sweep", "Pulse", "Barker", "Noise", "Ramp", "PRBS"};
const char* kEnvelopeShapeNames[] = {"Rectangular", "Sinc (sin(x)/x)", "Gaussian", "Hann"};
const char* kBarkerCodeNames[] = {
    "B2  [+ -]",       "B2  [+ +]",       "B3  [+ + -]",
    "B4  [+ + - +]",   "B4  [+ + + -]",   "B5  [+ + + - +]",
    "B7  [+ + + - - + -]", "B11 [+ + + - - - + - - + -]",
    "B13 [+ + + + + - - + + - + - +]"};
const char* kPrbsPolynomialNames[] = {"PRBS7", "PRBS9", "PRBS11", "PRBS15", "PRBS23", "PRBS31"};

bool clampGeneratorFrequencies(GeneratorConfig& cfg, double sampleRateHz) {
  const double nyquistHz = std::abs(sampleRateHz) * 0.5;
  const auto clampFrequency = [nyquistHz](double& hz) {
    const double clamped = std::clamp(hz, -nyquistHz, nyquistHz);
    if (clamped == hz) return false;
    hz = clamped;
    return true;
  };

  bool changed = clampFrequency(cfg.toneFreqHz);
  for (double& hz : cfg.multiToneFreqsHz) changed |= clampFrequency(hz);
  // Deviation spans both sides of 0 Hz (+-deviation/2), so its magnitude may
  // be up to a full sample rate before either endpoint exceeds Nyquist.
  const double clampedDeviation = std::clamp(cfg.chirpDeviationHz, -std::abs(sampleRateHz), std::abs(sampleRateHz));
  if (clampedDeviation != cfg.chirpDeviationHz) {
    cfg.chirpDeviationHz = clampedDeviation;
    changed = true;
  }
  const double clampedChipRate = std::clamp(cfg.barkerChipRateHz, 0.0, std::abs(sampleRateHz));
  if (clampedChipRate != cfg.barkerChipRateHz) {
    cfg.barkerChipRateHz = clampedChipRate;
    changed = true;
  }
  // QPSK packs 2 bits/symbol, so its symbol rate (bitRate/2) can go up to a
  // full sample rate before either endpoint exceeds Nyquist.
  const double maxPrbsBitRateHz = cfg.prbsQpskEnabled ? 2.0 * std::abs(sampleRateHz) : std::abs(sampleRateHz);
  const double clampedPrbsBitRate = std::clamp(cfg.prbsBitRateHz, 0.0, maxPrbsBitRateHz);
  if (clampedPrbsBitRate != cfg.prbsBitRateHz) {
    cfg.prbsBitRateHz = clampedPrbsBitRate;
    changed = true;
  }
  const double clampedPeriod = cfg.pulsePeriodSec > 0.0 ? cfg.pulsePeriodSec : 1e-6;
  if (clampedPeriod != cfg.pulsePeriodSec) {
    cfg.pulsePeriodSec = clampedPeriod;
    changed = true;
  }
  const double clampedPulseDuration = std::clamp(cfg.pulseDurationSec, 0.0, cfg.pulsePeriodSec);
  if (clampedPulseDuration != cfg.pulseDurationSec) {
    cfg.pulseDurationSec = clampedPulseDuration;
    changed = true;
  }
  return changed;
}

// Shared by the Pulse waveform (which always gates itself) and the "Pulse
// envelope" option on other waveforms -- both configure the same
// shape/duration/period fields.
bool drawEnvelopeShapeAndTiming(AppState& state) {
  bool changed = false;
  int shape = static_cast<int>(state.genConfig.envelopeShape);
  if (ImGui::Combo("Envelope shape", &shape, kEnvelopeShapeNames, IM_ARRAYSIZE(kEnvelopeShapeNames))) {
    state.genConfig.envelopeShape = static_cast<EnvelopeShape>(shape);
    changed = true;
  }
  changed |= DurationInputSec("Pulse duration", &state.genConfig.pulseDurationSec, &state.pulseDurationUnit);
  changed |= DurationInputSec("Pulse period", &state.genConfig.pulsePeriodSec, &state.pulsePeriodUnit);
  return changed;
}
}

void drawTxControlContents(AppState& state) {
  bool connected = state.deviceManager.isConnected();
  bool active = state.isTxActive();

  int prevSourceMode = state.txSourceMode;
  ImGui::BeginDisabled(active);
  ImGui::RadioButton("Signal generator", &state.txSourceMode, 0);
  ImGui::SameLine();
  ImGui::RadioButton("IQ file", &state.txSourceMode, 1);
  ImGui::EndDisabled();
  if (state.txSourceMode != prevSourceMode) ++state.txSignalGeneration;

  if (state.txSourceMode == 0) {
    bool generatorChanged = clampGeneratorFrequencies(state.genConfig, state.sampleRateHz);
    if (state.genConfig.sampleRateHz != state.sampleRateHz) {
      state.genConfig.sampleRateHz = state.sampleRateHz;
      generatorChanged = true;
    }
    const double nyquistHz = std::abs(state.sampleRateHz) * 0.5;
    int type = static_cast<int>(state.genConfig.type);
    if (ImGui::Combo("Waveform", &type, kWaveformNames, IM_ARRAYSIZE(kWaveformNames))) {
      state.genConfig.type = static_cast<WaveformType>(type);
      generatorChanged = true;
    }
    generatorChanged |= ImGui::SliderFloat("Amplitude", &state.genConfig.amplitude, 0.0f, 1.0f);

    switch (state.genConfig.type) {
      case WaveformType::Tone:
        generatorChanged |= FrequencyInputHz("Tone freq", &state.genConfig.toneFreqHz, &state.toneFreqUnit);
        break;
      case WaveformType::MultiTone: {
        int n = static_cast<int>(state.genConfig.multiToneFreqsHz.size());
        if (ImGui::InputInt("Tone count", &n)) {
          n = std::clamp(n, 1, 8);
          state.genConfig.multiToneFreqsHz.resize(n, 100e3);
          generatorChanged = true;
        }
        for (size_t i = 0; i < state.genConfig.multiToneFreqsHz.size(); ++i) {
          ImGui::PushID(static_cast<int>(i));
          generatorChanged |=
              FrequencyInputHz("Freq", &state.genConfig.multiToneFreqsHz[i], &state.multiToneUnit);
          ImGui::PopID();
        }
        break;
      }
      case WaveformType::Chirp:
        generatorChanged |= FrequencyInputHz(
            "Deviation", &state.genConfig.chirpDeviationHz, &state.chirpDeviationUnit);
        generatorChanged |= DurationInputSec(
            "Sweep duration", &state.genConfig.chirpDurationSec, &state.chirpDurationUnit);
        break;
      case WaveformType::Pulse:
        generatorChanged |= drawEnvelopeShapeAndTiming(state);
        break;
      case WaveformType::Barker: {
        int code = static_cast<int>(state.genConfig.barkerCode);
        if (ImGui::Combo("Barker code", &code, kBarkerCodeNames, IM_ARRAYSIZE(kBarkerCodeNames))) {
          state.genConfig.barkerCode = static_cast<BarkerCode>(code);
          generatorChanged = true;
        }
        generatorChanged |= FrequencyInputHz(
            "Chip rate", &state.genConfig.barkerChipRateHz, &state.barkerChipRateUnit);
        break;
      }
      case WaveformType::Noise:
      case WaveformType::Ramp:
        break;
      case WaveformType::Prbs: {
        int poly = static_cast<int>(state.genConfig.prbsPolynomial);
        if (ImGui::Combo("PRBS polynomial", &poly, kPrbsPolynomialNames, IM_ARRAYSIZE(kPrbsPolynomialNames))) {
          state.genConfig.prbsPolynomial = static_cast<PrbsPolynomial>(poly);
          generatorChanged = true;
        }
        generatorChanged |=
            FrequencyInputHz("Bit rate", &state.genConfig.prbsBitRateHz, &state.prbsBitRateUnit);
        generatorChanged |= ImGui::Checkbox("QPSK + RRC shaping", &state.genConfig.prbsQpskEnabled);
        ImGui::TextDisabled(
            "Off: raw bipolar bit stream (like Barker). On: bit pairs -> QPSK symbols -> "
            "root-raised-cosine pulse shaping -> IQ, e.g. for FPGA/demodulator testing.");
        if (state.genConfig.prbsQpskEnabled) {
          generatorChanged |= ImGui::SliderFloat("RRC roll-off", &state.genConfig.prbsRrcRolloff, 0.0f, 1.0f);
        }
        break;
      }
    }

    // Any non-Pulse waveform can optionally be gated by the same shaped
    // envelope Pulse uses on its own, turning it into a pulsed signal (e.g.
    // a pulsed chirp for radar-style testing).
    if (state.genConfig.type != WaveformType::Pulse) {
      generatorChanged |= ImGui::Checkbox("Pulse envelope", &state.genConfig.envelopeEnabled);
      if (state.genConfig.envelopeEnabled) {
        generatorChanged |= drawEnvelopeShapeAndTiming(state);
      }
    }

    // Mixes noise onto whatever waveform is configured, at the requested
    // SNR relative to Amplitude above -- for testing a receive path under a
    // controlled degraded SNR instead of a clean signal.
    generatorChanged |= ImGui::Checkbox("Add noise", &state.genConfig.noiseEnabled);
    if (state.genConfig.noiseEnabled) {
      generatorChanged |= ImGui::SliderFloat("SNR (dB)", &state.genConfig.noiseSnrDb, -20.0f, 60.0f, "%.1f");
    }

    // Complex IQ can represent baseband offsets only inside the Nyquist
    // interval. Values outside it would wrap to an unexpected RF frequency.
    generatorChanged |= clampGeneratorFrequencies(state.genConfig, state.sampleRateHz);
    if (state.genConfig.type == WaveformType::Barker) {
      ImGui::TextDisabled("Allowed chip rate: 0 .. %.6g MHz", std::abs(state.sampleRateHz) / 1e6);
    } else if (state.genConfig.type == WaveformType::Prbs) {
      double maxBitRateHz = state.genConfig.prbsQpskEnabled ? 2.0 * std::abs(state.sampleRateHz) : std::abs(state.sampleRateHz);
      ImGui::TextDisabled("Allowed bit rate: 0 .. %.6g MHz", maxBitRateHz / 1e6);
    } else if (state.genConfig.type == WaveformType::Chirp) {
      ImGui::TextDisabled("Allowed deviation: %.6g .. +%.6g MHz",
                          -std::abs(state.sampleRateHz) / 1e6, std::abs(state.sampleRateHz) / 1e6);
    } else if (state.genConfig.type != WaveformType::Pulse) {
      ImGui::TextDisabled("Allowed frequency offset: %.6g .. +%.6g MHz",
                          -nyquistHz / 1e6, nyquistHz / 1e6);
    }

    // Keep the generator's live config in sync regardless of whether TX is
    // actually running, not just while active -- AppState::updateDisplays()
    // previews the configured waveform even when idle, so edits need to
    // reach it immediately for that preview to reflect them.
    if (generatorChanged) {
      state.genConfig.sampleRateHz = state.sampleRateHz;
      state.generator->setConfig(state.genConfig);
      ++state.txSignalGeneration;
    }
  } else {
    ImGui::BeginDisabled(active);
    ImGui::InputText("File path", state.filePathBuffer, sizeof(state.filePathBuffer));
    ImGui::SameLine();
    bool browseClicked = ImGui::Button("Browse...");
    ImGui::Checkbox("Loop", &state.fileLoop);
    ImGui::TextDisabled(
        "Formats: .cf32/.fc32 (float32 IQ), .ci16/.sc16 (int16 IQ), .wav (PCM16), .sigmf-data/.sigmf-meta (SigMF)");

    // Raw IQ files don't carry their own sample rate, so resampling needs
    // the user to say what the file was actually recorded at. Each control
    // gets its own row -- FrequencyInputHz alone is already wide (numeric
    // field + 4 unit buttons + label), so chaining more onto the same line
    // via SameLine() overflowed the narrow TX Control dock panel.
    bool resampleToggled = ImGui::Checkbox("Resample on load", &state.fileResampleEnabled);
    // Seed a sensible starting coefficient (the one that lands exactly on
    // the device's configured TX rate) the moment resampling is turned on;
    // left alone after that so the user's own edits stick.
    if (resampleToggled && state.fileResampleEnabled && state.fileSourceRateHz > 0.0) {
      state.fileResampleCoefficient = state.sampleRateHz / state.fileSourceRateHz;
    }
    if (state.fileResampleEnabled) {
      FrequencyInputHz("File sample rate", &state.fileSourceRateHz, &state.fileSourceRateUnit);

      ImGui::SetNextItemWidth(140.0f);
      ImGui::InputDouble("Resample coefficient", &state.fileResampleCoefficient, 0.0, 0.0, "%.6g");
      state.fileResampleCoefficient = std::max(state.fileResampleCoefficient, 1e-6);

      double resultingRateHz = state.fileSourceRateHz * state.fileResampleCoefficient;
      ImGui::TextDisabled("Resulting rate: %s", formatHz(resultingRateHz).c_str());
      if (std::abs(resultingRateHz - state.sampleRateHz) > 1.0) {
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.0f), "Device TX rate is %s -- playback speed/pitch will differ",
                            formatHz(state.sampleRateHz).c_str());
      }
    }

    // Loading (and, worse, resampling -- a sinc convolution over a whole
    // file, easily seconds for a large file or a large ratio) used to run
    // directly in this button handler, freezing the whole GUI with no
    // feedback until it finished. AsyncIqLoadJob runs it on a background
    // thread instead; the Load button just starts the job and this frame
    // (and every frame after, via the progress bar below) moves on.
    static AsyncIqLoadJob loadJob;
    bool jobRunning = loadJob.running();

    ImGui::BeginDisabled(jobRunning);
    if (ImGui::Button("Load")) {
      loadJob.start(state.filePathBuffer, state.fileResampleEnabled, state.fileSourceRateHz,
                    state.fileResampleCoefficient);
    }
    ImGui::EndDisabled();

    if (loadJob.finished()) {
      AsyncIqLoadJob::Result result = loadJob.take();
      if (result.success) {
        state.fileSigmfInfo = result.sigmf;
        if (result.sigmf && result.sigmf->hasSampleRate) {
          // Raw .cf32/.ci16 have no way to carry their own sample rate, so
          // normally the user has to type it in manually (see
          // fileSourceRateHz's declaration); a SigMF sidecar supplies it
          // directly. Center frequency is intentionally *not* auto-applied
          // here -- see the "Apply to device" button below, since silently
          // retuning connected hardware from a file load would be surprising.
          state.fileSourceRateHz = result.sourceRateHz;
          state.log("SigMF: recovered sample rate " + formatHz(result.sourceRateHz));
        }
        if (state.fileResampleEnabled) {
          state.log("Resampled IQ file from " + formatHz(result.sourceRateHz) + " to " +
                     formatHz(result.resultRateHz) + " (x" + std::to_string(state.fileResampleCoefficient) + ", " +
                     std::to_string(result.inputSampleCount) + " -> " + std::to_string(result.buffer.size()) +
                     " samples)");
        }
        // Also used by the idle preview below and by Start TX -- loading
        // immediately makes the file's signal/spectrum visible without
        // having to transmit first, same as the generator preview.
        state.fileSource = std::make_shared<IQFileSource>(std::move(result.buffer), state.fileLoop);
        state.fileLoadedPath = state.filePathBuffer;
        state.fileLoadError.clear();
        ++state.txSignalGeneration;
        state.log("Loaded IQ file: " + state.fileLoadedPath + " (" +
                   std::to_string(state.fileSource->totalSamples()) + " samples)");
      } else {
        state.fileLoadError = result.errorMessage;
        state.fileSource.reset();
        state.fileLoadedPath.clear();
        state.fileSigmfInfo.reset();
        state.log("IQ file load failed: " + state.fileLoadError);
      }
    }

    if (jobRunning) {
      drawLoadProgressBar(loadJob);
    } else {
      // Status text can be an arbitrary (possibly long) file path or error
      // message, so it wraps within the panel's width instead of either
      // overflowing off-screen or being force-fit onto one line.
      bool loaded = state.fileSource && state.fileLoadedPath == state.filePathBuffer;
      ImGui::PushTextWrapPos(0.0f);
      if (loaded) {
        ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "Loaded: %zu samples", state.fileSource->totalSamples());
        if (state.fileSigmfInfo) {
          const SigmfMeta& meta = *state.fileSigmfInfo;
          if (!meta.hw.empty()) ImGui::TextDisabled("SigMF hw: %s", meta.hw.c_str());
          if (!meta.description.empty()) ImGui::TextDisabled("SigMF description: %s", meta.description.c_str());
          if (!meta.captures.empty() && meta.captures.front().hasFrequency) {
            double freqHz = meta.captures.front().frequencyHz;
            ImGui::TextDisabled("SigMF center freq: %s", formatHz(freqHz).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Apply to device")) {
              state.centerFreqHz = freqHz;
              IDevice* dev = state.deviceManager.device();
              if (dev && !dev->setFrequency(freqHz)) {
                state.log("Apply SigMF center freq: rejected by device");
              } else {
                state.log("Apply SigMF center freq: " + formatHz(freqHz) +
                           (dev ? " (tuned)" : " (no device connected)"));
              }
            }
          }
          if (!meta.annotations.empty()) {
            ImGui::TextDisabled("SigMF annotations (%zu):", meta.annotations.size());
            for (const auto& ann : meta.annotations) {
              ImGui::BulletText("sample %llu (%llu samples)%s%s", static_cast<unsigned long long>(ann.sampleStart),
                                 static_cast<unsigned long long>(ann.sampleCount), ann.label.empty() ? "" : "  ",
                                 ann.label.c_str());
            }
          }
        }
      } else if (!state.fileLoadError.empty()) {
        ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "%s", state.fileLoadError.c_str());
      } else {
        ImGui::TextDisabled("Not loaded -- click Load to preview the signal/spectrum before transmitting");
      }
      ImGui::PopTextWrapPos();
    }
    ImGui::EndDisabled();

    DrawIqFileBrowserPopup("Select IQ file", browseClicked, state.filePathBuffer, sizeof(state.filePathBuffer));
  }

  ImGui::Separator();

  ImGui::BeginDisabled(!connected);
  if (!active) {
    if (ImGui::Button("Start TX")) {
      std::shared_ptr<ISampleSource> source;
      bool ok = true;
      if (state.txSourceMode == 0) {
        state.genConfig.sampleRateHz = state.sampleRateHz;
        state.generator->setConfig(state.genConfig);
        source = state.generator;
      } else if (state.fileSource && state.fileLoadedPath == state.filePathBuffer) {
        // Reuses the already-loaded (and already-previewing) source instead
        // of reloading, so playback continues from the preview's current
        // position instead of restarting from sample 0.
        source = state.fileSource;
      } else {
        state.txError = "Load the IQ file first (see Load button above)";
        state.log("TX start failed: " + state.txError);
        ok = false;
      }
      if (ok) {
        auto tee = std::make_shared<TeeSampleSource>(source, &state.txPreviewRing);
        if (state.deviceManager.device()->startTx(tee, state.txError)) {
          state.log("TX started");
        } else {
          state.log("TX start failed: " + state.txError);
        }
      }
    }
  } else {
    if (ImGui::Button("Stop TX")) {
      state.deviceManager.device()->stopTx();
      state.log("TX stopped");
    }
  }
  ImGui::EndDisabled();

  if (!state.txError.empty()) {
    ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "%s", state.txError.c_str());
  }
}

} // namespace iqforge

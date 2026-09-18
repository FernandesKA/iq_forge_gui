#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "app_settings.h"
#include "app_state.h"
#include "test_framework.h"

using namespace iqforge;
namespace fs = std::filesystem;

namespace {
constexpr const char* kTestConfigDir = "iqforge_test_config";

void setConfigDirEnv() {
#if defined(_WIN32)
  _putenv_s("IQFORGE_CONFIG_DIR", kTestConfigDir);
#else
  setenv("IQFORGE_CONFIG_DIR", kTestConfigDir, 1);
#endif
}

// Distinctive, non-default values so a round trip can't accidentally pass
// by comparing against another instance's own defaults.
void fillDistinctive(AppState& s) {
  s.selectedKind = DeviceKind::HackRF;
  std::snprintf(s.uriBuffer, sizeof(s.uriBuffer), "usb:1.2.3");
  s.sampleRateHz = 5.5e6;
  s.sampleRateUnit = FreqUnit::kHz;
  // Distinct RX vs TX values -- this is exactly the independent RX/TX
  // frequency support being round-tripped, so they must not collapse to one.
  s.rxCenterFreqHz = 433.92e6;
  s.rxCenterFreqUnit = FreqUnit::GHz;
  s.txCenterFreqHz = 144.39e6;
  s.txCenterFreqUnit = FreqUnit::MHz;
  s.bandwidthHz = 1.2e6;
  s.bandwidthUnit = FreqUnit::Hz;
  s.txGainDb = -12.5;
  s.rxGainDb = 30.0;
  s.rxGainMode = RxGainMode::AgcFast;
  s.setFftSize(4096);

  s.txSourceMode = 1;
  s.genConfig.type = WaveformType::Chirp;
  s.genConfig.chirpDeviationHz = 500e3;
  s.genConfig.chirpDurationSec = 2e-3;
  s.genConfig.amplitude = 0.42f;
  s.genConfig.noiseEnabled = true;
  s.genConfig.noiseSnrDb = 15.5f;
  s.toneFreqUnit = FreqUnit::Hz;
  s.chirpDeviationUnit = FreqUnit::MHz;
  s.chirpDurationUnit = TimeUnit::Us;

  std::snprintf(s.filePathBuffer, sizeof(s.filePathBuffer), "/tmp/some_capture.sigmf-data");
  s.fileLoop = false;
  s.fileSourceRateHz = 2.4e6;
  s.fileResampleEnabled = true;
  s.fileResampleCoefficient = 1.5;

  s.rxSaveFormat = AppState::RxSaveFormat::Cf32Raw;
  std::snprintf(s.rxRecordPathBuffer, sizeof(s.rxRecordPathBuffer), "/tmp/out.cf32");
  std::snprintf(s.rxRecordDescriptionBuffer, sizeof(s.rxRecordDescriptionBuffer), "test capture");

  std::snprintf(s.svFilePathBuffer, sizeof(s.svFilePathBuffer), "/tmp/some_other_capture.wav");
  s.svLoop = false;
  s.svSourceRateHz = 1.8e6;
  s.svSourceRateUnit = FreqUnit::kHz;
  s.svResampleEnabled = true;
  s.svResampleCoefficient = 0.75;
  s.svSaveFormat = AppState::SvSaveFormat::Cf32Raw;
  std::snprintf(s.svSavePathBuffer, sizeof(s.svSavePathBuffer), "/tmp/sv_out.cf32");

  s.showTxTab = false;
  s.showRxTab = false;
  s.showSignalViewerTab = false;
  s.showSpectrumViewerTab = true;
}

void checkMatchesDistinctive(const AppState& s) {
  CHECK(s.selectedKind == DeviceKind::HackRF);
  CHECK(std::string(s.uriBuffer) == "usb:1.2.3");
  CHECK(s.sampleRateHz == 5.5e6);
  CHECK(s.sampleRateUnit == FreqUnit::kHz);
  CHECK(s.rxCenterFreqHz == 433.92e6);
  CHECK(s.rxCenterFreqUnit == FreqUnit::GHz);
  CHECK(s.txCenterFreqHz == 144.39e6);
  CHECK(s.txCenterFreqUnit == FreqUnit::MHz);
  CHECK(s.bandwidthHz == 1.2e6);
  CHECK(s.bandwidthUnit == FreqUnit::Hz);
  CHECK(s.txGainDb == -12.5);
  CHECK(s.rxGainDb == 30.0);
  CHECK(s.rxGainMode == RxGainMode::AgcFast);
  CHECK(s.fftSize == 4096);
  CHECK(s.txFft.fftSize() == 4096);
  CHECK(s.rxFft.fftSize() == 4096);

  CHECK(s.txSourceMode == 1);
  CHECK(s.genConfig.type == WaveformType::Chirp);
  CHECK(s.genConfig.chirpDeviationHz == 500e3);
  CHECK(s.genConfig.chirpDurationSec == 2e-3);
  CHECK(s.genConfig.amplitude == 0.42f);
  CHECK(s.genConfig.noiseEnabled == true);
  CHECK(s.genConfig.noiseSnrDb == 15.5f);
  CHECK(s.toneFreqUnit == FreqUnit::Hz);
  CHECK(s.chirpDeviationUnit == FreqUnit::MHz);
  CHECK(s.chirpDurationUnit == TimeUnit::Us);

  CHECK(std::string(s.filePathBuffer) == "/tmp/some_capture.sigmf-data");
  CHECK(s.fileLoop == false);
  CHECK(s.fileSourceRateHz == 2.4e6);
  CHECK(s.fileResampleEnabled == true);
  CHECK(s.fileResampleCoefficient == 1.5);

  CHECK(s.rxSaveFormat == AppState::RxSaveFormat::Cf32Raw);
  CHECK(std::string(s.rxRecordPathBuffer) == "/tmp/out.cf32");
  CHECK(std::string(s.rxRecordDescriptionBuffer) == "test capture");

  CHECK(std::string(s.svFilePathBuffer) == "/tmp/some_other_capture.wav");
  CHECK(s.svLoop == false);
  CHECK(s.svSourceRateHz == 1.8e6);
  CHECK(s.svSourceRateUnit == FreqUnit::kHz);
  CHECK(s.svResampleEnabled == true);
  CHECK(s.svResampleCoefficient == 0.75);
  CHECK(s.svSaveFormat == AppState::SvSaveFormat::Cf32Raw);
  CHECK(std::string(s.svSavePathBuffer) == "/tmp/sv_out.cf32");

  CHECK(s.showTxTab == false);
  CHECK(s.showRxTab == false);
  CHECK(s.showSignalViewerTab == false);
  CHECK(s.showSpectrumViewerTab == true);
}
} // namespace

void run_app_settings_tests() {
  std::error_code ec;
  fs::remove_all(kTestConfigDir, ec); // hermetic: start with no leftover config
  setConfigDirEnv();

  // No prefs file yet -- defaults to enabled.
  CHECK(loadAutoSaveEnabled() == true);

  saveAutoSaveEnabled(false);
  CHECK(loadAutoSaveEnabled() == false);
  saveAutoSaveEnabled(true);
  CHECK(loadAutoSaveEnabled() == true);

  // Preset save/load round trip.
  {
    AppState src;
    fillDistinctive(src);
    savePreset(src, "test-preset");

    auto names = listPresets();
    CHECK(std::find(names.begin(), names.end(), "test-preset") != names.end());

    AppState dst;
    CHECK(loadPreset(dst, "test-preset") == true);
    checkMatchesDistinctive(dst);

    CHECK(loadPreset(dst, "no-such-preset") == false);

    deletePreset("test-preset");
    names = listPresets();
    CHECK(std::find(names.begin(), names.end(), "test-preset") == names.end());
    CHECK(loadPreset(dst, "test-preset") == false);
  }

  // Session save/load round trip, gated on the auto-save toggle.
  {
    fs::remove_all(kTestConfigDir + std::string("/session.json"), ec);
    AppState src;
    fillDistinctive(src);

    saveAutoSaveEnabled(true);
    saveSessionSettings(src);

    AppState dst;
    CHECK(loadSessionSettings(dst) == true);
    checkMatchesDistinctive(dst);

    // Disabling auto-save must stop both directions: no write, and any
    // existing session file is ignored on load.
    saveAutoSaveEnabled(false);
    fs::remove(kTestConfigDir + std::string("/session.json"), ec);
    saveSessionSettings(src); // should be a no-op -- file must stay absent
    CHECK(!fs::exists(kTestConfigDir + std::string("/session.json")));

    AppState dst2;
    CHECK(loadSessionSettings(dst2) == false);
  }

  // Loading a pre-existing settings.json written before independent RX/TX
  // frequencies existed (only the old shared "centerFreqHz"/"centerFreqUnit"
  // keys, no "rxCenterFreqHz"/"txCenterFreqHz") must apply that one value to
  // both directions rather than silently resetting either to the default.
  {
    fs::remove_all(kTestConfigDir + std::string("/session.json"), ec);
    saveAutoSaveEnabled(true);
    std::ofstream legacy(kTestConfigDir + std::string("/session.json"));
    legacy << R"({"device": {"centerFreqHz": 868.0e6, "centerFreqUnit": "MHz"}})";
    legacy.close();

    AppState dst;
    CHECK(loadSessionSettings(dst) == true);
    CHECK(dst.rxCenterFreqHz == 868.0e6);
    CHECK(dst.txCenterFreqHz == 868.0e6);
    CHECK(dst.rxCenterFreqUnit == FreqUnit::MHz);
    CHECK(dst.txCenterFreqUnit == FreqUnit::MHz);
  }

  saveAutoSaveEnabled(true);
  fs::remove_all(kTestConfigDir, ec);
}

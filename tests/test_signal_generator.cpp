#include <algorithm>
#include <cmath>
#include <vector>

#include "fft_processor.h"
#include "signal_generator.h"
#include "test_framework.h"

using namespace iqforge;

namespace {
size_t peakBin(const std::vector<float>& db) {
  return static_cast<size_t>(std::max_element(db.begin(), db.end()) - db.begin());
}

double wrapToPi(double phase) {
  constexpr double kPi = 3.14159265358979323846;
  while (phase > kPi) phase -= 2.0 * kPi;
  while (phase < -kPi) phase += 2.0 * kPi;
  return phase;
}

// Estimates instantaneous frequency (Hz) from the phase step between two
// adjacent IQ samples taken sampleRateHz apart.
double instFreqHz(const Sample& a, const Sample& b, double sampleRateHz) {
  constexpr double kTwoPi = 6.283185307179586476925286766559;
  double dPhase = wrapToPi(std::atan2(b.imag(), b.real()) - std::atan2(a.imag(), a.real()));
  return dPhase / kTwoPi * sampleRateHz;
}
} // namespace

void run_signal_generator_tests() {
  constexpr size_t fftSize = 4096;
  constexpr double sampleRate = 1e6;

  // Tone at Fs/8 should show up as a spectral peak near bin (fftSize/2 + fftSize/8),
  // since FftProcessor fftshifts so index fftSize/2 is DC.
  {
    GeneratorConfig cfg;
    cfg.type = WaveformType::Tone;
    cfg.sampleRateHz = sampleRate;
    cfg.toneFreqHz = sampleRate / 8.0;
    cfg.amplitude = 1.0f;
    SignalGenerator gen(cfg);

    std::vector<Sample> buf(fftSize);
    CHECK(gen.generate(buf.data(), buf.size()) == fftSize);

    FftProcessor fft({fftSize, WindowType::Hann, 1.0f});
    std::vector<float> db;
    fft.process(buf.data(), buf.size(), db);

    size_t expected = fftSize / 2 + fftSize / 8;
    size_t got = peakBin(db);
    CHECK(got >= expected - 2 && got <= expected + 2);
  }

  // Ramp output must stay within [-amplitude, amplitude].
  {
    GeneratorConfig cfg;
    cfg.type = WaveformType::Ramp;
    cfg.amplitude = 0.5f;
    SignalGenerator gen(cfg);
    std::vector<Sample> buf(5000);
    gen.generate(buf.data(), buf.size());
    for (const auto& s : buf) {
      CHECK(s.real() >= -0.5001f && s.real() <= 0.5001f);
      CHECK(s.imag() == 0.0f);
    }
  }

  // A running generator must use a newly supplied tone frequency on the
  // next block; the GUI relies on this for live TX tuning.
  {
    GeneratorConfig cfg;
    cfg.type = WaveformType::Tone;
    cfg.sampleRateHz = sampleRate;
    cfg.toneFreqHz = sampleRate / 8.0;
    cfg.amplitude = 1.0f;
    SignalGenerator gen(cfg);

    std::vector<Sample> buf(fftSize);
    gen.generate(buf.data(), buf.size());

    cfg.toneFreqHz = sampleRate / 4.0;
    gen.setConfig(cfg);
    gen.generate(buf.data(), buf.size());

    FftProcessor fft({fftSize, WindowType::Hann, 1.0f});
    std::vector<float> db;
    fft.process(buf.data(), buf.size(), db);

    size_t expected = fftSize / 2 + fftSize / 4;
    size_t got = peakBin(db);
    CHECK(got >= expected - 2 && got <= expected + 2);
  }

  // Frequencies outside the complex-IQ Nyquist interval must be clamped
  // instead of silently aliasing to another RF frequency.
  {
    GeneratorConfig cfg;
    cfg.sampleRateHz = sampleRate;
    cfg.toneFreqHz = 100e6;
    cfg.multiToneFreqsHz = {-100e6, 100e6};
    cfg.chirpDeviationHz = 200e6;
    cfg.barkerChipRateHz = 100e6;

    SignalGenerator gen(cfg);
    GeneratorConfig actual = gen.config();
    CHECK(actual.toneFreqHz == sampleRate / 2.0);
    CHECK(actual.multiToneFreqsHz[0] == -sampleRate / 2.0);
    CHECK(actual.multiToneFreqsHz[1] == sampleRate / 2.0);
    CHECK(actual.chirpDeviationHz == sampleRate);
    CHECK(actual.barkerChipRateHz == sampleRate);

    cfg.toneFreqHz = -100e6;
    gen.setConfig(cfg);
    CHECK(gen.config().toneFreqHz == -sampleRate / 2.0);
  }

  // Every distinct Barker sequence must be emitted as repeating real BPSK
  // chips. At one sample per chip the generated samples equal the code.
  {
    struct BarkerCase {
      BarkerCode code;
      std::vector<int> chips;
    };
    const std::vector<BarkerCase> cases = {
        {BarkerCode::B2PlusMinus, {1, -1}},
        {BarkerCode::B2PlusPlus, {1, 1}},
        {BarkerCode::B3, {1, 1, -1}},
        {BarkerCode::B4PlusPlusMinusPlus, {1, 1, -1, 1}},
        {BarkerCode::B4PlusPlusPlusMinus, {1, 1, 1, -1}},
        {BarkerCode::B5, {1, 1, 1, -1, 1}},
        {BarkerCode::B7, {1, 1, 1, -1, -1, 1, -1}},
        {BarkerCode::B11, {1, 1, 1, -1, -1, -1, 1, -1, -1, 1, -1}},
        {BarkerCode::B13, {1, 1, 1, 1, 1, -1, -1, 1, 1, -1, 1, -1, 1}},
    };

    for (const BarkerCase& test : cases) {
      GeneratorConfig cfg;
      cfg.type = WaveformType::Barker;
      cfg.sampleRateHz = sampleRate;
      cfg.barkerChipRateHz = sampleRate;
      cfg.barkerCode = test.code;
      cfg.amplitude = 1.0f;
      SignalGenerator gen(cfg);

      std::vector<Sample> buf(test.chips.size() * 2);
      gen.generate(buf.data(), buf.size());
      for (size_t i = 0; i < buf.size(); ++i) {
        CHECK(buf[i].real() == static_cast<float>(test.chips[i % test.chips.size()]));
        CHECK(buf[i].imag() == 0.0f);
      }

      for (size_t lag = 1; lag < test.chips.size(); ++lag) {
        int correlation = 0;
        for (size_t i = 0; i + lag < test.chips.size(); ++i) {
          correlation += test.chips[i] * test.chips[i + lag];
        }
        CHECK(std::abs(correlation) <= 1);
      }
    }
  }

  // Noise should not be constant (basic sanity check it's actually random).
  {
    GeneratorConfig cfg;
    cfg.type = WaveformType::Noise;
    cfg.amplitude = 1.0f;
    SignalGenerator gen(cfg);
    std::vector<Sample> buf(1000);
    gen.generate(buf.data(), buf.size());
    bool allSame = std::all_of(buf.begin(), buf.end(), [&](const Sample& s) { return s == buf.front(); });
    CHECK(!allSame);
  }

  // MultiTone with N tones should keep combined amplitude bounded near cfg.amplitude.
  {
    GeneratorConfig cfg;
    cfg.type = WaveformType::MultiTone;
    cfg.sampleRateHz = sampleRate;
    cfg.multiToneFreqsHz = {50e3, 150e3};
    cfg.amplitude = 1.0f;
    SignalGenerator gen(cfg);
    std::vector<Sample> buf(2000);
    gen.generate(buf.data(), buf.size());
    for (const auto& s : buf) {
      CHECK(std::abs(s) <= 1.01f);
    }
  }

  // Chirp (LFM) instantaneous frequency must ramp linearly across the
  // configured deviation (centered on 0 Hz) over one sweep, hold constant
  // amplitude, and restart (sawtooth) after chirpDurationSec elapses.
  {
    GeneratorConfig cfg;
    cfg.type = WaveformType::Chirp;
    cfg.sampleRateHz = sampleRate;
    cfg.chirpDeviationHz = 400e3;
    const size_t sweepSamples = 4096;
    cfg.chirpDurationSec = static_cast<double>(sweepSamples) / sampleRate;
    cfg.amplitude = 1.0f;
    SignalGenerator gen(cfg);

    std::vector<Sample> buf(sweepSamples);
    gen.generate(buf.data(), buf.size());

    for (const auto& s : buf) {
      CHECK(std::abs(s) >= 0.99f && std::abs(s) <= 1.01f);
    }

    const double dt = 1.0 / sampleRate;
    const double startFreq = -cfg.chirpDeviationHz / 2.0;
    const double k = cfg.chirpDeviationHz / cfg.chirpDurationSec;
    const size_t checkIdx[] = {5, sweepSamples / 4, sweepSamples / 2, sweepSamples * 3 / 4 - 1};
    for (size_t i : checkIdx) {
      double expected = startFreq + k * (static_cast<double>(i) * dt);
      double actual = instFreqHz(buf[i], buf[i + 1], sampleRate);
      CHECK(std::abs(actual - expected) < 2000.0);
    }

    // Sweep repeats: the next buffer should again start near -deviation/2.
    std::vector<Sample> buf2(10);
    gen.generate(buf2.data(), buf2.size());
    double restartFreq = instFreqHz(buf2[0], buf2[1], sampleRate);
    CHECK(std::abs(restartFreq - startFreq) < 2000.0);
  }

  // A negative deviation must produce a down-chirp (frequency decreasing).
  {
    GeneratorConfig cfg;
    cfg.type = WaveformType::Chirp;
    cfg.sampleRateHz = sampleRate;
    cfg.chirpDeviationHz = -400e3;
    cfg.chirpDurationSec = 4096.0 / sampleRate;
    cfg.amplitude = 1.0f;
    SignalGenerator gen(cfg);

    std::vector<Sample> buf(4096);
    gen.generate(buf.data(), buf.size());

    double freqStart = instFreqHz(buf[5], buf[6], sampleRate);
    double freqEnd = instFreqHz(buf[3000], buf[3001], sampleRate);
    CHECK(freqStart > freqEnd);
    CHECK(std::abs(freqStart - 200e3) < 2000.0);
  }

  // Pulse: a rectangular constant-amplitude carrier gated on for
  // pulseDurationSec out of every pulsePeriodSec, then repeats.
  {
    GeneratorConfig cfg;
    cfg.type = WaveformType::Pulse;
    cfg.sampleRateHz = sampleRate; // 1e6 -> 1 sample = 1 us
    cfg.pulseDurationSec = 10e-6;  // 10 samples on
    cfg.pulsePeriodSec = 30e-6;    // 30 samples per cycle (10 on, 20 off)
    cfg.amplitude = 1.0f;
    SignalGenerator gen(cfg);

    std::vector<Sample> buf(90); // 3 full periods
    gen.generate(buf.data(), buf.size());

    for (size_t cycle = 0; cycle < 3; ++cycle) {
      size_t base = cycle * 30;
      // Avoid the exact wrap boundary at relative index 0, where
      // accumulated floating-point drift in the running time base can tip
      // the comparison either way; index 3..7 is safely mid-window.
      for (size_t i = 3; i < 8; ++i) {
        CHECK(buf[base + i].real() == 1.0f);
        CHECK(buf[base + i].imag() == 0.0f);
      }
      for (size_t i = 15; i < 25; ++i) {
        CHECK(buf[base + i].real() == 0.0f);
        CHECK(buf[base + i].imag() == 0.0f);
      }
    }
  }

  // Rectangular envelope gates any other waveform type on/off using its own
  // (separate from the Pulse waveform's) duration/period timing, e.g.
  // turning a tone into a pulsed carrier.
  {
    GeneratorConfig cfg;
    cfg.type = WaveformType::Tone;
    cfg.sampleRateHz = sampleRate;
    cfg.toneFreqHz = 100e3;
    cfg.envelopeEnabled = true;
    cfg.envelopeShape = EnvelopeShape::Rectangular;
    cfg.envelopeRectDurationSec = 10e-6;
    cfg.envelopeRectPeriodSec = 30e-6;
    cfg.amplitude = 1.0f;
    SignalGenerator gen(cfg);

    std::vector<Sample> buf(90);
    gen.generate(buf.data(), buf.size());

    for (size_t cycle = 0; cycle < 3; ++cycle) {
      size_t base = cycle * 30;
      for (size_t i = 3; i < 8; ++i) {
        CHECK(std::abs(buf[base + i]) > 0.99f); // tone present during "on"
      }
      for (size_t i = 15; i < 25; ++i) {
        CHECK(buf[base + i].real() == 0.0f);
        CHECK(buf[base + i].imag() == 0.0f);
      }
    }
  }

  // PRBS (raw bit stream): every bit must be a hard +-amplitude chip (no
  // QPSK/RRC shaping), and short-order polynomials must reproduce their
  // exact 2^order - 1 maximal-length sequence -- every nonzero lag
  // autocorrelation near 0 (an m-sequence property), not just "looks random".
  {
    struct PrbsCase {
      PrbsPolynomial poly;
      int order;
    };
    const std::vector<PrbsCase> cases = {
        {PrbsPolynomial::Prbs7, 7},
        {PrbsPolynomial::Prbs9, 9},
        {PrbsPolynomial::Prbs11, 11},
    };
    for (const PrbsCase& test : cases) {
      GeneratorConfig cfg;
      cfg.type = WaveformType::Prbs;
      cfg.sampleRateHz = sampleRate;
      cfg.prbsBitRateHz = sampleRate; // 1 sample/bit
      cfg.prbsPolynomial = test.poly;
      cfg.amplitude = 1.0f;
      SignalGenerator gen(cfg);

      const size_t period = (1u << test.order) - 1u;
      std::vector<Sample> buf(period * 2);
      gen.generate(buf.data(), buf.size());

      std::vector<int> chips(period);
      for (size_t i = 0; i < period; ++i) {
        CHECK(buf[i].real() == 1.0f || buf[i].real() == -1.0f);
        CHECK(buf[i].imag() == 0.0f);
        chips[i] = buf[i].real() > 0.0f ? 1 : -1;
      }
      // Sequence must repeat exactly with the polynomial's period.
      for (size_t i = 0; i < period; ++i) {
        CHECK(buf[period + i].real() == buf[i].real());
      }
      // Maximal-length autocorrelation: near-zero at every nonzero lag.
      for (size_t lag = 1; lag < period; lag += std::max<size_t>(1, period / 20)) {
        long correlation = 0;
        for (size_t i = 0; i < period; ++i) correlation += chips[i] * chips[(i + lag) % period];
        CHECK(std::abs(correlation) <= 1);
      }
    }
  }

  // PRBS is deterministic from its fixed seed: reconstructing a generator
  // with the same config must reproduce the same bit sequence, which is the
  // whole point of using it to test a signal path (a known, repeatable
  // pattern the far end can verify against).
  {
    GeneratorConfig cfg;
    cfg.type = WaveformType::Prbs;
    cfg.sampleRateHz = sampleRate;
    cfg.prbsBitRateHz = sampleRate;
    cfg.prbsPolynomial = PrbsPolynomial::Prbs31;
    cfg.amplitude = 1.0f;

    SignalGenerator genA(cfg);
    SignalGenerator genB(cfg);
    std::vector<Sample> bufA(5000), bufB(5000);
    genA.generate(bufA.data(), bufA.size());
    genB.generate(bufB.data(), bufB.size());
    CHECK(bufA == bufB);
    bool allSame = std::all_of(bufA.begin(), bufA.end(), [&](const Sample& s) { return s == bufA.front(); });
    CHECK(!allSame);
  }

  // PRBS + QPSK/RRC shaping: bit pairs become QPSK symbols pulse-shaped by a
  // root-raised-cosine filter, so the output should be complex (both I and Q
  // active, unlike the raw real bit stream above), stay reasonably bounded,
  // and its bandwidth should scale up with a wider roll-off (more excess
  // bandwidth spreads the same symbol rate over more spectrum).
  {
    auto occupiedBandwidthBins = [&](float rolloff) {
      GeneratorConfig cfg;
      cfg.type = WaveformType::Prbs;
      cfg.sampleRateHz = sampleRate;
      cfg.prbsPolynomial = PrbsPolynomial::Prbs15;
      cfg.prbsQpskEnabled = true;
      cfg.prbsBitRateHz = sampleRate / 8.0; // symbol rate = bitRate/2 = Fs/16
      cfg.prbsRrcRolloff = rolloff;
      cfg.amplitude = 1.0f;
      SignalGenerator gen(cfg);

      std::vector<Sample> buf(fftSize);
      gen.generate(buf.data(), buf.size());

      bool haveQ = std::any_of(buf.begin(), buf.end(), [](const Sample& s) { return s.imag() != 0.0f; });
      CHECK(haveQ);
      for (const auto& s : buf) CHECK(std::abs(s) < 2.0f); // bounded despite RRC overshoot

      FftProcessor fft({fftSize, WindowType::Hann, 1.0f});
      std::vector<float> db;
      fft.process(buf.data(), buf.size(), db);
      float peakDb = *std::max_element(db.begin(), db.end());
      size_t count = 0;
      for (float v : db) {
        if (v >= peakDb - 20.0f) ++count;
      }
      return count;
    };

    size_t narrowBins = occupiedBandwidthBins(0.05f);
    size_t wideBins = occupiedBandwidthBins(0.9f);
    CHECK(wideBins > narrowBins);
  }

  // Sinc envelope: one full window per period (envelopeSincFreqHz), peaking
  // at the center and tapering to (exactly, by construction) zero at the
  // period's edges instead of switching abruptly like Rectangular.
  {
    GeneratorConfig cfg;
    cfg.type = WaveformType::Tone;
    cfg.sampleRateHz = sampleRate;
    cfg.toneFreqHz = 0.0; // constant real carrier, easier to inspect the envelope alone
    cfg.envelopeEnabled = true;
    cfg.envelopeShape = EnvelopeShape::Sinc;
    cfg.envelopeSincFreqHz = 25e3; // period = 40 samples at 1 MHz
    cfg.amplitude = 1.0f;
    SignalGenerator gen(cfg);

    std::vector<Sample> buf(40);
    gen.generate(buf.data(), buf.size());

    CHECK(std::abs(buf[20].real()) > 0.9f);                      // window center: near full amplitude
    CHECK(std::abs(buf[0].real()) < std::abs(buf[20].real()));   // tapered at the period's start edge
    CHECK(std::abs(buf[0].real()) < 0.01f);                      // edge is (near) zero, not abruptly cut
  }

  // Gaussian envelope: a bump of width envelopeGaussianSigmaSec repeating at
  // envelopeGaussianFreqHz, peaking at the center of each period and decaying
  // smoothly (never a hard cutoff, unlike Rectangular) towards the edges.
  {
    GeneratorConfig cfg;
    cfg.type = WaveformType::Tone;
    cfg.sampleRateHz = sampleRate;
    cfg.toneFreqHz = 0.0;
    cfg.envelopeEnabled = true;
    cfg.envelopeShape = EnvelopeShape::Gaussian;
    cfg.envelopeGaussianFreqHz = 25e3;         // period = 40 samples
    cfg.envelopeGaussianSigmaSec = 40e-6 / 6.0; // edges sit at ~+-3 sigma
    cfg.amplitude = 1.0f;
    SignalGenerator gen(cfg);

    std::vector<Sample> buf(40);
    gen.generate(buf.data(), buf.size());

    CHECK(std::abs(buf[20].real()) > 0.9f);                     // window center: near full amplitude
    CHECK(std::abs(buf[0].real()) < std::abs(buf[20].real()));  // decayed at the period's start edge
    CHECK(std::abs(buf[0].real()) < 0.02f);                     // ~3 sigma out: small but not necessarily exact 0
  }

  // Sine envelope: a classic AM-style sinusoid at envelopeSineFreqHz.
  // envelopeModDepth == 1.0 (full depth) swings the gain all the way from 0
  // at the trough to 1 at the peak, matching 100% AM modulation depth.
  {
    GeneratorConfig cfg;
    cfg.type = WaveformType::Tone;
    cfg.sampleRateHz = sampleRate;
    cfg.toneFreqHz = 0.0;
    cfg.envelopeEnabled = true;
    cfg.envelopeShape = EnvelopeShape::Sine;
    cfg.envelopeSineFreqHz = 25e3; // period = 40 samples
    cfg.envelopeModDepth = 1.0f;
    cfg.amplitude = 1.0f;
    SignalGenerator gen(cfg);

    std::vector<Sample> buf(40);
    gen.generate(buf.data(), buf.size());

    CHECK(std::abs(buf[10].real()) > 0.99f); // quarter period: sin(pi/2) == 1 -> full gain
    CHECK(std::abs(buf[30].real()) < 0.01f); // three-quarter period: sin(3pi/2) == -1 -> ~0 gain
  }

  // A shallower modulation depth should leave a nonzero floor at the trough
  // instead of reaching all the way to 0.
  {
    GeneratorConfig cfg;
    cfg.type = WaveformType::Tone;
    cfg.sampleRateHz = sampleRate;
    cfg.toneFreqHz = 0.0;
    cfg.envelopeEnabled = true;
    cfg.envelopeShape = EnvelopeShape::Sine;
    cfg.envelopeSineFreqHz = 25e3;
    cfg.envelopeModDepth = 0.5f;
    cfg.amplitude = 1.0f;
    SignalGenerator gen(cfg);

    std::vector<Sample> buf(40);
    gen.generate(buf.data(), buf.size());

    CHECK(std::abs(buf[30].real()) > 0.49f && std::abs(buf[30].real()) < 0.51f); // trough floored at 1-depth
  }

  // Add noise: average total power E[|signal + noise|^2] should land near
  // signalPower + noisePower (cross term averages to ~0 over enough
  // samples, since the added noise is zero-mean and independent of the
  // deterministic tone) -- i.e. the requested SNR is actually honored, not
  // just "some noise gets added somewhere".
  {
    GeneratorConfig cfg;
    cfg.type = WaveformType::Tone;
    cfg.sampleRateHz = sampleRate;
    cfg.toneFreqHz = sampleRate / 8.0;
    cfg.amplitude = 1.0f;
    cfg.noiseEnabled = true;
    cfg.noiseSnrDb = 10.0f;
    SignalGenerator gen(cfg);

    std::vector<Sample> buf(200000);
    gen.generate(buf.data(), buf.size());

    double sumPower = 0.0;
    for (const auto& s : buf) sumPower += static_cast<double>(std::norm(s));
    double avgPower = sumPower / static_cast<double>(buf.size());

    double signalPower = 1.0; // amplitude^2, amplitude == 1
    double expectedNoisePower = signalPower / std::pow(10.0, cfg.noiseSnrDb / 10.0);
    double expectedTotal = signalPower + expectedNoisePower;
    CHECK(std::abs(avgPower - expectedTotal) < expectedTotal * 0.15);
  }

  // Noise is mixed in after envelope/pulse gating, so it fills in the
  // "silent" gaps between pulses too (a real RF noise floor doesn't turn
  // off just because the signal did) -- unlike the noiseEnabled==false case
  // above, where those same samples are exactly 0.
  {
    GeneratorConfig cfg;
    cfg.type = WaveformType::Pulse;
    cfg.sampleRateHz = sampleRate;
    cfg.pulseDurationSec = 20e-6;
    cfg.pulsePeriodSec = 40e-6;
    cfg.amplitude = 1.0f;
    cfg.noiseEnabled = true;
    cfg.noiseSnrDb = 0.0f; // noise power == signal power -- easily visible
    SignalGenerator gen(cfg);

    std::vector<Sample> buf(40);
    gen.generate(buf.data(), buf.size());

    bool anyNonzeroInGap = false;
    for (size_t i = 20; i < 40; ++i) {
      if (buf[i].real() != 0.0f || buf[i].imag() != 0.0f) anyNonzeroInGap = true;
    }
    CHECK(anyNonzeroInGap);
  }
}

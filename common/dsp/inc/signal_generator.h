#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <random>
#include <vector>

#include "sample_source.h"

namespace iqforge {

enum class WaveformType {
  Tone,
  MultiTone,
  Chirp,
  Pulse,
  Barker,
  Noise,
  Ramp,
  Prbs,
};

// Shape of the envelope applied to a non-Pulse waveform when envelopeEnabled
// is set (see GeneratorConfig below). Each shape has its own dedicated
// timing/width fields, independent of the Pulse waveform's own ДИ/ППИ.
enum class EnvelopeShape {
  Rectangular, // hard on/off gate, own ДИ/ППИ (envelopeRect*)
  Sine,        // continuous AM-style sinusoid at envelopeSineFreqHz
  Sinc,        // sin(x)/x main lobe plus a few sidelobes, repeats at envelopeSincFreqHz
  Gaussian,    // Gaussian bump of width envelopeGaussianSigmaSec, repeats at envelopeGaussianFreqHz
};

// All Barker sequences, excluding variants obtainable only by negation or
// reversal. Lengths 2 and 4 each have two distinct sequences.
enum class BarkerCode {
  B2PlusMinus,
  B2PlusPlus,
  B3,
  B4PlusPlusMinusPlus,
  B4PlusPlusPlusMinus,
  B5,
  B7,
  B11,
  B13,
};

// Standard ITU-T/test-equipment maximal-length PRBS polynomials, named by
// their LFSR order (sequence period is 2^order - 1 bits).
enum class PrbsPolynomial {
  Prbs7,
  Prbs9,
  Prbs11,
  Prbs15,
  Prbs23,
  Prbs31,
};

struct GeneratorConfig {
  WaveformType type = WaveformType::Tone;
  double sampleRateHz = 1e6;

  // Tone
  double toneFreqHz = 0.0;

  // MultiTone
  std::vector<double> multiToneFreqsHz = {50e3, 150e3, 300e3};

  // Chirp: linear sweep of chirpDeviationHz total bandwidth, centered on 0 Hz
  // baseband (i.e. from -deviation/2 to +deviation/2), over durationSec, then
  // repeats (sawtooth sweep). A negative deviation produces a down-chirp.
  double chirpDeviationHz = 800e3;
  double chirpDurationSec = 1e-3;

  // Barker: a continuously repeated BPSK chip sequence.
  BarkerCode barkerCode = BarkerCode::B13;
  double barkerChipRateHz = 100e3;

  // Pulse: a constant-amplitude (real) carrier gated on for pulseDurationSec
  // (ДИ) out of every pulsePeriodSec (ППИ), then repeats. Always a hard
  // rectangular on/off gate; for a shaped envelope on top of a *continuous*
  // waveform instead, see envelopeEnabled below.
  double pulseDurationSec = 10e-6;  // ДИ
  double pulsePeriodSec = 100e-6;   // ППИ

  // Envelope: multiplies any *other* (non-Pulse) waveform by a repeating
  // shape, e.g. turning a tone into an AM-modulated or pulsed carrier.
  // Ignored for WaveformType::Pulse, which always applies its own gating
  // (above) instead. envelopeModDepth (0..1) controls how far the shape
  // pulls the gain down from 1.0 at its deepest point: 1.0 (default) reaches
  // all the way to the shape's own floor (0 gain for Rectangular/Sinc/
  // Gaussian, full 100%-AM for Sine); lower values leave a partial floor
  // instead of the full swing.
  bool envelopeEnabled = false;
  EnvelopeShape envelopeShape = EnvelopeShape::Rectangular;
  float envelopeModDepth = 1.0f; // 0..1

  // Rectangular envelope: its own ДИ/ППИ, independent of the Pulse
  // waveform's fields above.
  double envelopeRectDurationSec = 10e-6; // ДИ
  double envelopeRectPeriodSec = 100e-6;  // ППИ

  // Sine envelope: a classic AM-style sinusoid at this rate; envelopeModDepth
  // above plays the role of modulation depth (amplitude of the sinusoid).
  double envelopeSineFreqHz = 10e3;

  // Sinc envelope: sin(x)/x main lobe (3 sidelobes each side), one full
  // window per period, repeating at this rate.
  double envelopeSincFreqHz = 10e3;

  // Gaussian envelope: a Gaussian bump of width envelopeGaussianSigmaSec
  // (standard deviation), repeating at this rate.
  double envelopeGaussianFreqHz = 10e3;
  double envelopeGaussianSigmaSec = 5e-6;

  // PRBS: a continuously repeated pseudorandom bit sequence from an LFSR of
  // the selected standard polynomial -- useful for testing signal paths
  // because the bit pattern and its period (2^order - 1 bits) are exactly
  // known ahead of time. With prbsQpskEnabled false, bits directly BPSK the
  // carrier (one +-amplitude chip per bit, like Barker above). With it true,
  // bit pairs map to QPSK symbols which are pulse-shaped by a root-raised-
  // cosine filter, e.g. for feeding a known bit pattern into an FPGA/
  // demodulator under test: PRBS -> QPSK -> RRC -> IQ.
  PrbsPolynomial prbsPolynomial = PrbsPolynomial::Prbs15;
  double prbsBitRateHz = 100e3;
  bool prbsQpskEnabled = false;
  float prbsRrcRolloff = 0.35f; // 0..1, root-raised-cosine excess bandwidth

  float amplitude = 0.7f; // 0..1, leaves headroom to avoid clipping downstream

  // Add noise: mixes complex Gaussian noise onto the generated waveform
  // (any type, applied after envelope/pulse gating so the noise floor stays
  // continuous even between pulses, like a real RF channel's does) at
  // noiseSnrDb relative to `amplitude`. amplitude is used as the nominal
  // signal power reference rather than a measured running average, since a
  // single-pass generator has no lookahead to average over -- accurate for
  // constant-envelope waveforms (Tone, Barker, PRBS BPSK/QPSK, ...), an
  // approximation for the rest (MultiTone, Pulse's on-time, ...). Useful for
  // testing a receive path under a controlled degraded SNR instead of a
  // clean signal.
  bool noiseEnabled = false;
  float noiseSnrDb = 20.0f;
};

// Produces synthetic IQ test signals. Safe to reconfigure from another
// thread while generate() is being called from the TX thread.
class SignalGenerator : public ISampleSource {
 public:
  explicit SignalGenerator(GeneratorConfig cfg = {});

  void setConfig(const GeneratorConfig& cfg);
  GeneratorConfig config() const;

  size_t generate(Sample* out, size_t count) override;

 private:
  void generateTone(Sample* out, size_t count, const GeneratorConfig& cfg);
  void generateMultiTone(Sample* out, size_t count, const GeneratorConfig& cfg);
  void generateChirp(Sample* out, size_t count, const GeneratorConfig& cfg);
  void generatePulse(Sample* out, size_t count, const GeneratorConfig& cfg);
  void generateBarker(Sample* out, size_t count, const GeneratorConfig& cfg);
  void generateNoise(Sample* out, size_t count, const GeneratorConfig& cfg);
  void generateRamp(Sample* out, size_t count, const GeneratorConfig& cfg);
  void generatePrbs(Sample* out, size_t count, const GeneratorConfig& cfg);
  void generatePrbsBpsk(Sample* out, size_t count, const GeneratorConfig& cfg);
  void generatePrbsQpsk(Sample* out, size_t count, const GeneratorConfig& cfg);
  void applyPulseGate(Sample* out, size_t count, const GeneratorConfig& cfg);
  void applyEnvelope(Sample* out, size_t count, const GeneratorConfig& cfg);
  void applyNoise(Sample* out, size_t count, const GeneratorConfig& cfg);

  // (Re)seeds the LFSR and, for the QPSK path, refills the RRC symbol
  // window -- called whenever the PRBS waveform is (re)selected or its
  // polynomial/mode changes, since the two paths consume bits at different
  // rates (1 bit/chip vs. 2 bits/symbol) and can't share mid-sequence state.
  void resetPrbsState(const GeneratorConfig& cfg);
  int nextPrbsBit();
  Sample nextPrbsQpskSymbol();

  mutable std::mutex cfgMutex_;
  GeneratorConfig cfg_;

  double tonePhase_ = 0.0;
  std::vector<double> multiTonePhases_;
  double chirpTime_ = 0.0;
  double pulseGateTime_ = 0.0;
  double envelopeTime_ = 0.0;
  size_t barkerChipIndex_ = 0;
  double barkerChipPhase_ = 0.0;
  float rampValue_ = -1.0f;

  uint32_t prbsReg_ = 0;
  int prbsOrder_ = 0;
  int prbsTap_ = 0;
  bool prbsInitialized_ = false;
  PrbsPolynomial prbsActivePolynomial_ = PrbsPolynomial::Prbs15;
  bool prbsQpskActive_ = false;
  double prbsBitPhase_ = 0.0;
  int prbsCurrentBit_ = 0;
  double prbsSymbolPhase_ = 0.0;
  std::deque<Sample> prbsSymbolWindow_;

  std::mt19937 rng_{std::random_device{}()};
  std::normal_distribution<float> noiseDist_{0.0f, 1.0f};
};

} // namespace iqforge

#pragma once

#include <atomic>
#include <complex>
#include <cstdint>
#include <vector>

namespace slopsmith {

// Virtual drop tuning: a real-time "drop pedal" for the instrument input.
//
// Shifts the input down (or up) by a number of semitones so a bass/guitar in
// standard tuning sounds like a lower tuning (e.g. -6 → Bb standard). Applied
// to the mono input source before the detectors and the tone chain, so the
// tuner, note detection and amp sims all hear the shifted signal.
//
// Algorithm: pitch-synchronous delay line. A read head runs at rate
// r = 2^(semitones/12) against the write head; once it has drifted one splice
// span away it crossfades to a second head exactly one span closer. The span is
// a whole number of periods of the note being played (YIN-tracked), so the
// splice lands on the same point of the waveform and the pitch stays clean.
// Added latency averages about crossfade + half a span: ~14 ms on a bass at
// -6 semitones in the offline prototype (93 % of notes within 35 cents).
//
// Monophonic by design: the tracker follows one pitch, so chords still shift
// but splice less cleanly. JUCE-free so it can be unit-tested standalone.
//
// Threading: prepare() allocates (call before audio starts); process() is
// real-time safe (no allocation, no locks); setSemitones() may be called from
// any thread.
class DropTuner
{
public:
    static constexpr float kMaxSemitones = 12.0f;

    void prepare(double sampleRate);
    void reset() noexcept;

    // 0 = bypass. Clamped to [-12, 12]; non-finite → 0.
    void setSemitones(float semitones) noexcept;
    float getSemitones() const noexcept { return requested.load(std::memory_order_relaxed); }

    // In-place processing of one mono block. Bypass leaves samples untouched.
    void process(float* samples, int numSamples) noexcept;

    // Diagnostics / tests (audio-thread values; read after process()).
    double getDelaySamples() const noexcept { return delay; }
    double getTrackedPeriod() const noexcept { return period; }

private:
    double estimatePeriod() noexcept;              // YIN on recent input; 0 = unvoiced
    float readAt(double delaySamples) const noexcept;
    double spanFor(double p) const noexcept;
    void fft(std::complex<double>* a) const noexcept;

    std::atomic<float> requested { 0.0f };
    float active = 0.0f;                           // semitones in effect on the audio thread
    double rate = 1.0;

    double sr = 48000.0;
    std::vector<float> ring;                       // input history
    uint64_t mask = 0;
    uint64_t written = 0;

    double delay = 0.0, newDelay = 0.0, minDelay = 0.0;
    int xfadeLeft = 0, xfadeLen = 1;
    double defaultSpan = 0.0, minSpan = 0.0, period = 0.0;
    int hop = 256, hopCounter = 0;

    // YIN workspace (sized in prepare)
    int tauMin = 2, tauMax = 2, frameLen = 4, fftSize = 8;
    std::vector<double> frame, cumSq, cmnd;
    std::vector<std::complex<double>> spec, twiddles;
    std::vector<int> bitrev;
};

} // namespace slopsmith

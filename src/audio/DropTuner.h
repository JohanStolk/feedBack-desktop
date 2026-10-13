#pragma once

#include <atomic>
#include <complex>
#include <cstdint>
#include <vector>

namespace slopsmith {

// Virtual tuning: a real-time "drop pedal" for the instrument input.
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
// Changing the amount crossfades (~4 ms) from the current head to a fresh one,
// so it can change note by note without clicks.
//
// Chart (optional, per song): the renderer sends the song's shift change
// points ("match each song": a per-string shift for Drop D and other mixed
// tunings) and the expected input pitch of every note, plus the song clock.
// Then:
//  - the shift follows the change points at sample-exact song times;
//  - priming: 10 ms before each note its expected period is loaded, so the
//    splice is right from the pluck instead of after the tracker settles;
//    the tracker then searches near the expected note and falls back to a
//    free search when the player plays something else;
//  - on a detected pluck near a chart note the head restarts right behind
//    the input (crossfaded), cutting the delay at the attack.
// The offline prototype measured 6 ms instead of 22 ms to the shifted pitch,
// and 96 % instead of 93 % of notes in tune, on a real bass take.
//
// Monophonic by design: the tracker follows one pitch, so chords still shift
// but splice less cleanly. JUCE-free so it can be unit-tested standalone.
//
// Threading: prepare() allocates (call before audio starts); process() is
// real-time safe (no allocation, no locks). setSemitones() and setClock() may
// be called from any thread; setChart()/clearChart() from one control thread
// (they allocate/free; the audio thread swaps charts lock-free).
class DropTuner
{
public:
    static constexpr float kMaxSemitones = 12.0f;

    DropTuner() = default;
    ~DropTuner();
    DropTuner(const DropTuner&) = delete;
    DropTuner& operator=(const DropTuner&) = delete;

    void prepare(double sampleRate);
    void reset() noexcept;

    // 0 = bypass. Clamped to [-12, 12]; non-finite → 0. Fractional values are
    // fine (e.g. a song tuned a few cents off A440). Ignored while a chart
    // with shift change points is loaded.
    void setSemitones(float semitones) noexcept;
    float getSemitones() const noexcept { return requested.load(std::memory_order_relaxed); }

    // One song's chart. Times are song seconds. `shiftTimes/shiftSemis`: the
    // amount in effect from each time on (empty = use setSemitones). `noteTimes/
    // noteHz`: each note's expected INPUT pitch, i.e. what the instrument
    // sounds before the shift (empty = no priming). Pairs are sorted here;
    // non-finite or mismatched entries are dropped.
    void setChart(const std::vector<double>& shiftTimes, const std::vector<float>& shiftSemis,
                  const std::vector<double>& noteTimes, const std::vector<float>& noteHz);
    void clearChart();
    // The song position now, pushed by the renderer (~every frame); the audio
    // thread extrapolates from it while `playing`.
    void setClock(double songTime, bool playing) noexcept;

    // In-place processing of one mono block. Bypass leaves samples untouched.
    // Returns true if any sample was changed (shifted or crossfading).
    bool process(float* samples, int numSamples) noexcept;

    // Diagnostics / tests (audio-thread values; read after process()).
    double getDelaySamples() const noexcept { return delay; }
    double getTrackedPeriod() const noexcept { return period; }
    double getHintPeriod() const noexcept { return hint; }
    float getActiveSemitones() const noexcept { return active; }
    uint32_t getOnsetCount() const noexcept { return onsetCount; }

private:
    struct Chart
    {
        std::vector<double> shiftT;
        std::vector<float> shiftSemis;
        std::vector<double> noteT;
        std::vector<float> noteHz;
    };

    // YIN on the newest `len` samples; best period in [lo, hi] samples, 0 = none.
    double estimatePeriod(int len, int lo, int hi) noexcept;
    void track() noexcept;                         // one tracker hop (uses `hint`)
    bool onsetStep(float v) noexcept;
    void adoptPendingChart() noexcept;
    void syncChartTo(double songTime) noexcept;   // reposition after a seek
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

    // Chart handoff: the control thread publishes `pendingChart`; the audio
    // thread adopts it and hands the old one back through `retiredChart`,
    // which the control thread frees on its next call.
    std::atomic<Chart*> pendingChart { nullptr };
    std::atomic<Chart*> retiredChart { nullptr };
    Chart* chart = nullptr;                        // audio thread only
    size_t shiftIdx = 0, nextNote = 0;
    double lastSongTime = -1e300;

    std::atomic<double> clockTime { 0.0 };
    std::atomic<bool> clockPlaying { false };
    std::atomic<uint32_t> clockGen { 0 };
    uint32_t seenClockGen = 0;
    bool haveClock = false, basePlaying = false;
    double baseTime = 0.0;
    uint64_t baseWritten = 0;

    double hint = 0.0;                             // expected period (samples), 0 = none
    static constexpr uint64_t kNoPluck = UINT64_MAX / 2;
    uint64_t sincePluck = kNoPluck;                // samples since a pluck matched a chart note
    bool restartPending = false;                   // head restart waiting for a crossfade to end
    // Onset detector: a fast envelope jumping well above the slow one.
    double envFast = 0.0, envSlow = 0.0, aFast = 0.0, aSlow = 0.0;
    int sinceOnset = 0, onsetHoldoff = 1;
    uint32_t onsetCount = 0;

    // YIN workspace (sized in prepare)
    int tauMin = 2, tauMax = 2, frameLen = 4, fftSize = 8;
    std::vector<double> frame, cumSq, cmnd;
    std::vector<std::complex<double>> spec, twiddles;
    std::vector<int> bitrev;
};

} // namespace slopsmith

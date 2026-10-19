#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace slopsmith {

// How much of an audio callback's time budget the processing uses.
//
// load = time spent in the callback / duration of the block it produced
// (numSamples / sampleRate). 1.0 means the callback took as long as the audio
// it delivered, so any more and the device underruns (heard as crackles).
// The audio thread records each block; any thread reads a smoothed average
// (~0.5 s time constant) and the worst block since its previous read.
//
// Real-time safe: no locks, no allocation; steady_clock is a vDSO /
// QueryPerformanceCounter read. JUCE-free so it can be unit-tested standalone.
class CallbackLoad
{
public:
    using Clock = std::chrono::steady_clock;

    // Times one callback body: construct at the top, destroyed on any return.
    class Scope
    {
    public:
        Scope(CallbackLoad& l, int numSamples, double sampleRate) noexcept
            : load(l), n(numSamples), sr(sampleRate), t0(Clock::now()) {}
        ~Scope() { load.add(std::chrono::duration<double>(Clock::now() - t0).count(), n, sr); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        CallbackLoad& load;
        int n;
        double sr;
        Clock::time_point t0;
    };

    // Audio thread: one block took `seconds` to produce `numSamples` at
    // `sampleRate`. Ignored for an empty block or an unknown rate.
    void add(double seconds, int numSamples, double sampleRate) noexcept
    {
        if (numSamples <= 0 || ! (sampleRate > 0.0) || ! std::isfinite(seconds) || seconds < 0.0) return;
        const double budget = numSamples / sampleRate;
        const float l = (float) (seconds / budget);
        // Exponential average over ~kTimeConstant seconds of audio, so the
        // value means the same at every buffer size.
        const float a = (float) (1.0 - std::exp(-budget / kTimeConstant));
        avg = blocks == 0 ? l : avg + a * (l - avg);
        ++blocks;
        average.store(avg, std::memory_order_relaxed);
        float p = peak.load(std::memory_order_relaxed);
        while (l > p && ! peak.compare_exchange_weak(p, l, std::memory_order_relaxed)) {}
        count.fetch_add(1, std::memory_order_relaxed);
    }

    // Any thread. Smoothed load (0 = idle, 1 = the whole budget).
    float getAverage() const noexcept { return average.load(std::memory_order_relaxed); }
    // Any thread. Worst block since the previous call (0 if none since).
    float takePeak() noexcept { return peak.exchange(0.0f, std::memory_order_relaxed); }
    // Blocks recorded so far (to tell "idle" from "no callbacks running").
    uint64_t getBlockCount() const noexcept { return count.load(std::memory_order_relaxed); }

    // Control thread, while no callback runs (device restart): start over.
    void reset() noexcept
    {
        avg = 0.0f;
        blocks = 0;
        average.store(0.0f, std::memory_order_relaxed);
        peak.store(0.0f, std::memory_order_relaxed);
    }

private:
    static constexpr double kTimeConstant = 0.5;   // seconds of audio

    float avg = 0.0f;                              // audio thread only
    uint64_t blocks = 0;                           // audio thread only
    std::atomic<float> average { 0.0f };
    std::atomic<float> peak { 0.0f };
    std::atomic<uint64_t> count { 0 };
};

} // namespace slopsmith

// Real-time cost and added delay of DropTuner per audio callback size.
//   drop_tuner_bench [sampleRate]
// Renders 30 s of synthetic bass notes (E1–G2, plucked and muted, with a
// chart for priming) at several callback sizes.
// 1. Cost: the median and worst (99.9th percentile) time per callback as a
//    share of its real-time budget. The pitch analysis runs every ~5.3 ms in
//    one go, so the worst callback, not the average, decides whether a buffer
//    size is safe. Depends on the machine.
// 2. Delay: what the shift adds (mean while shifting, and over the first 20 ms
//    after each matched pluck: the attack), next to the buffers' own share of
//    the device latency (one input + one output buffer). Machine-independent.
#include "DropTuner.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {
constexpr double kPi = 3.14159265358979323846;

// Plucked, muted notes. `minLen`/`muteSec`: the delay table uses longer notes
// with a clear mute, so every pluck is detected (the cost table keeps the
// denser, faster run).
std::vector<float> synthBass(double sr, double seconds, std::vector<double>& noteT, std::vector<float>& noteHz,
                             double minLen = 0.12, double muteSec = 0.03)
{
    std::vector<float> x((size_t) (seconds * sr), 0.0f);
    unsigned seed = 7;
    auto rnd = [&seed]() { seed = seed * 1103515245u + 12345u; return (seed >> 8) / 16777216.0; };
    double t0 = 0.0;
    while (t0 < seconds - 0.2)
    {
        const int midi = 28 + (int) (rnd() * 16);            // E1..G2
        const double hz = 440.0 * std::pow(2.0, (midi - 69) / 12.0);
        const double len = minLen + rnd() * 0.5;              // fast and long notes
        noteT.push_back(t0);
        noteHz.push_back((float) hz);
        const size_t a = (size_t) (t0 * sr), n = (size_t) (len * sr), mute = (size_t) (muteSec * sr);
        for (size_t i = 0; i < n && a + i < x.size(); ++i)
        {
            const double t = i / sr;
            double s = 0.0;
            for (int k = 1; k * hz < 5000.0 && k < 40; ++k)
                s += std::exp(-0.12 * k) / k * std::sin(2 * kPi * k * hz * t + 0.7 * k);
            double g = 0.5 * std::min(1.0, t / 0.003) * std::exp(-t / 2.0);
            if (i + mute > n) g *= (double) (n - i) / mute;
            x[a + i] = (float) (g * s);
        }
        t0 += len;
    }
    return x;
}

double pct(std::vector<double> v, double q)
{
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, (size_t) (q * v.size()))];
}
} // namespace

int main(int argc, char** argv)
{
    const double sr = argc > 1 ? std::atof(argv[1]) : 48000.0;
    std::vector<double> noteT;
    std::vector<float> noteHz;
    const auto input = synthBass(sr, 30.0, noteT, noteHz);
    std::printf("DropTuner cost per callback, %.0f Hz, 30 s of synthetic bass, shift -6\n", sr);
    std::printf("%-7s %-7s %10s %10s %10s  %s\n", "block", "mode", "median", "p99.9", "max", "worst / budget");
    for (int block : { 32, 64, 128, 256, 512 })
    {
        for (int primed = 0; primed < 2; ++primed)
        {
            slopsmith::DropTuner dt;
            dt.prepare(sr);
            dt.setSemitones(-6.0f);
            if (primed) dt.setChart({}, {}, noteT, noteHz);
            auto x = input;
            std::vector<double> us;
            us.reserve(x.size() / (size_t) block);
            for (size_t i = 0; i + (size_t) block <= x.size(); i += (size_t) block)
            {
                if (primed) dt.setClock((double) i / sr, true);
                const auto t0 = std::chrono::steady_clock::now();
                dt.process(x.data() + i, block);
                us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
            }
            const double budget = block / sr * 1e6, worst = pct(us, 0.999);
            std::printf("%-7d %-7s %8.1fus %8.1fus %8.1fus  %5.1f%% of %.2f ms%s\n", block, primed ? "primed" : "plain",
                        pct(us, 0.5), worst, pct(us, 1.0), worst / budget * 100.0, budget / 1000.0,
                        worst / budget > 0.5 ? "  <- too tight with an amp sim" : "");
        }
    }
    std::printf("Rule of thumb: keep 'worst / budget' under ~30%% so amp sims and the rest of the\n"
                "callback still fit; pick the smallest buffer that stays there.\n");

    // Added delay per callback size. The mean is over every block while
    // shifting (DelayStats::nowMs, equal weight per sample); the attack is the
    // mean of the per-pluck figures (DelayStats::attackMs, each new pluck).
    std::vector<double> dNoteT;
    std::vector<float> dNoteHz;
    const auto dInput = synthBass(sr, 30.0, dNoteT, dNoteHz, 0.3, 0.1);
    std::printf("\nAdded delay, %.0f Hz, 30 s of synthetic bass (0.3-0.8 s notes, 100 ms mutes), "
                "chart-primed; plain = no chart (mean only)\n", sr);
    std::printf("%-7s %-14s %-6s %12s %12s %12s %7s\n", "block", "buffers in+out", "shift",
                "plain mean", "primed mean", "attack", "plucks");
    for (int block : { 32, 64, 128, 256, 512 })
    {
        for (float semis : { -1.0f, -6.0f })
        {
            double mean[2] = { 0.0, 0.0 }, attackSum = 0.0;
            uint32_t plucks = 0;
            for (int primed = 0; primed < 2; ++primed)
            {
                slopsmith::DropTuner dt;
                dt.prepare(sr);
                dt.setSemitones(semis);
                if (primed) dt.setChart({}, {}, dNoteT, dNoteHz);
                auto x = dInput;
                double sum = 0.0;
                size_t n = 0;
                for (size_t i = 0; i + (size_t) block <= x.size(); i += (size_t) block)
                {
                    if (primed) dt.setClock((double) i / sr, true);
                    dt.process(x.data() + i, block);
                    const auto st = dt.getDelayStats();
                    if (st.shifting) { sum += st.nowMs; ++n; }
                    if (primed && st.plucks > plucks) { attackSum += st.attackMs; plucks = st.plucks; }
                }
                mean[primed] = n ? sum / (double) n : 0.0;
            }
            std::printf("%-7d %11.1f ms  %-6.0f %9.1f ms %9.1f ms %9.1f ms %4u/%zu\n", block, 2.0 * block / sr * 1000.0,
                        (double) semis, mean[0], mean[1], plucks ? attackSum / plucks : 0.0, plucks, dNoteT.size());
        }
    }
    std::printf("The shift's delay should not depend on the block size; the buffers' share grows\n"
                "with it (plus the driver's own latency, which only the device can report).\n");
    return 0;
}

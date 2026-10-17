// Unit tests for slopsmith::DropTuner (virtual drop tuning). JUCE-free,
// platform-independent — exit 0 = pass. Uses its own CHECK (assert() is
// compiled out in Release builds).
#include "DropTuner.h"

#include <cmath>
#include <cstdio>
#include <vector>

using slopsmith::DropTuner;

static int failures = 0;
#define CHECK(cond, ...)                                                           \
    do {                                                                           \
        if (!(cond)) {                                                             \
            ++failures;                                                            \
            std::fprintf(stderr, "FAIL %s:%d: %s — ", __FILE__, __LINE__, #cond);  \
            std::fprintf(stderr, __VA_ARGS__);                                     \
            std::fprintf(stderr, "\n");                                            \
        }                                                                          \
    } while (0)

static const double kPi = 3.14159265358979323846;

// Bass-like note: decaying harmonics, so it has a real fundamental + overtones.
static std::vector<float> bassNote(double hz, double seconds, double sr)
{
    std::vector<float> x((size_t) (seconds * sr));
    for (size_t n = 0; n < x.size(); ++n)
    {
        const double t = n / sr;
        double s = 0.0;
        for (int k = 1; k * hz < 5000.0 && k < 40; ++k)
            s += std::exp(-0.12 * k) / k * std::sin(2 * kPi * k * hz * t + 0.7 * k);
        x[n] = (float) (0.5 * s * std::min(1.0, t / 0.003) * std::exp(-t / 2.0));
    }
    return x;
}

// Independent pitch estimate (brute-force YIN) of x[a, b) — not DropTuner's code.
static double measureHz(const std::vector<float>& x, size_t a, size_t b, double sr, double fmin, double fmax)
{
    const int tauMax = (int) (sr / fmin), tauMin = (int) (sr / fmax);
    const int w = (int) (b - a) - tauMax;
    if (w <= tauMax) return 0.0;
    std::vector<double> d((size_t) tauMax + 1, 0.0), cm((size_t) tauMax + 1, 1.0);
    for (int tau = 1; tau <= tauMax; ++tau)
    {
        double s = 0.0;
        for (int j = 0; j < w; ++j)
        {
            const double diff = x[a + (size_t) j] - x[a + (size_t) (j + tau)];
            s += diff * diff;
        }
        d[(size_t) tau] = s;
    }
    double run = 0.0;
    for (int tau = 1; tau <= tauMax; ++tau)
    {
        run += d[(size_t) tau];
        cm[(size_t) tau] = run > 0 ? d[(size_t) tau] * tau / run : 1.0;
    }
    int best = -1;
    for (int t = tauMin; t < tauMax; ++t)
        if (cm[(size_t) t] < 0.15)
        {
            while (t + 1 < tauMax && cm[(size_t) t + 1] < cm[(size_t) t]) ++t;
            best = t;
            break;
        }
    if (best < 1) return 0.0;
    const double y0 = cm[(size_t) best - 1], y1 = cm[(size_t) best], y2 = cm[(size_t) best + 1];
    const double den = y0 - 2 * y1 + y2;
    const double tau = std::abs(den) > 1e-12 ? best + 0.5 * (y0 - y2) / den : best;
    return sr / tau;
}

static double cents(double f, double ref) { return 1200.0 * std::log2(f / ref); }

static std::vector<float> run(DropTuner& dt, std::vector<float> x, int block, double* maxDelayMs = nullptr,
                              double sr = 48000.0, size_t settleFrom = 0)
{
    double maxDelay = 0.0;
    for (size_t i = 0; i < x.size(); i += (size_t) block)
    {
        const int n = (int) std::min((size_t) block, x.size() - i);
        dt.process(x.data() + i, n);
        if (i >= settleFrom) maxDelay = std::max(maxDelay, dt.getDelaySamples());
    }
    if (maxDelayMs) *maxDelayMs = maxDelay / sr * 1000.0;
    return x;
}

static void test_bypass_is_identity()
{
    DropTuner dt;
    dt.prepare(48000.0);
    const auto in = bassNote(41.2, 0.5, 48000.0);
    const auto out = run(dt, in, 256);
    bool same = true;
    for (size_t i = 0; i < in.size(); ++i) same = same && in[i] == out[i];
    CHECK(same, "semitones=0 must leave the signal untouched");
}

static void test_shift_down(double sr, double inHz, float semis)
{
    DropTuner dt;
    dt.prepare(sr);
    dt.setSemitones(semis);
    const auto in = bassNote(inHz, 1.6, sr);
    double maxDelayMs = 0.0;
    const auto out = run(dt, in, 256, &maxDelayMs, sr, (size_t) (0.2 * sr));
    const double expected = inHz * std::pow(2.0, semis / 12.0);
    const double got = measureHz(out, (size_t) (0.6 * sr), (size_t) (1.0 * sr), sr, expected / 1.5, expected * 1.5);
    CHECK(got > 0.0 && std::abs(cents(got, expected)) < 10.0,
          "%.0f Hz, %.1f Hz %+g st: expected %.2f Hz, got %.2f Hz (%.1f cents)",
          sr, inHz, semis, expected, got, got > 0 ? cents(got, expected) : 0.0);
    // Latency bound: min delay (~4 ms) + one splice span (≤ ~2 periods of the
    // input) — the prototype measured a 32 ms max on a real bass at -6.
    CHECK(maxDelayMs < 40.0, "max delay %.1f ms at %.0f Hz %+g st", maxDelayMs, sr, semis);
    for (float v : out) CHECK(std::isfinite(v), "non-finite output sample");
}

static void test_shift_up()
{
    const double sr = 48000.0;
    DropTuner dt;
    dt.prepare(sr);
    dt.setSemitones(5.0f);
    const auto in = bassNote(55.0, 1.6, sr);
    const auto out = run(dt, in, 128);
    const double expected = 55.0 * std::pow(2.0, 5.0 / 12.0);
    const double got = measureHz(out, (size_t) (0.6 * sr), (size_t) (1.0 * sr), sr, expected / 1.5, expected * 1.5);
    CHECK(got > 0.0 && std::abs(cents(got, expected)) < 10.0, "up +5: expected %.2f Hz, got %.2f Hz", expected, got);
}

static void test_block_size_independent()
{
    // The algorithm runs per sample, so the device buffer size must not change
    // the output.
    const auto in = bassNote(41.2, 0.8, 48000.0);
    DropTuner a, b;
    a.prepare(48000.0); b.prepare(48000.0);
    a.setSemitones(-6.0f); b.setSemitones(-6.0f);
    const auto oa = run(a, in, 64);
    const auto ob = run(b, in, 1024);
    double maxDiff = 0.0;
    for (size_t i = 0; i < in.size(); ++i) maxDiff = std::max(maxDiff, (double) std::abs(oa[i] - ob[i]));
    CHECK(maxDiff == 0.0, "64- vs 1024-sample blocks differ by %g", maxDiff);
}

static void test_clamping()
{
    DropTuner dt;
    dt.prepare(48000.0);
    dt.setSemitones(-40.0f);
    CHECK(dt.getSemitones() == -12.0f, "clamp low: %g", dt.getSemitones());
    dt.setSemitones(99.0f);
    CHECK(dt.getSemitones() == 12.0f, "clamp high: %g", dt.getSemitones());
    dt.setSemitones(std::nanf(""));
    CHECK(dt.getSemitones() == 0.0f, "NaN → 0: %g", dt.getSemitones());
}

static void test_silence_stays_silent()
{
    DropTuner dt;
    dt.prepare(48000.0);
    dt.setSemitones(-6.0f);
    std::vector<float> z(48000, 0.0f);
    const auto out = run(dt, z, 256);
    double peak = 0.0;
    for (float v : out) peak = std::max(peak, (double) std::abs(v));
    CHECK(peak == 0.0, "silence in → %g out", peak);
}

// Magical tuning switches the amount per note (e.g. Drop D: -2 on the low
// string, 0 elsewhere). A switch must crossfade, never jump.
static double maxStep(const std::vector<float>& x, size_t from)
{
    double m = 0.0;
    for (size_t i = from + 1; i < x.size(); ++i) m = std::max(m, (double) std::abs(x[i] - x[i - 1]));
    return m;
}

static void test_switching_is_click_free()
{
    const double sr = 48000.0;
    DropTuner dt;
    dt.prepare(sr);
    // Steady tone (no decay), so any click stands out against the waveform.
    std::vector<float> in((size_t) (2.0 * sr));
    for (size_t n = 0; n < in.size(); ++n)
    {
        double s = 0.0;
        for (int k = 1; k <= 12; ++k) s += std::exp(-0.2 * k) / k * std::sin(2 * kPi * k * 55.0 * n / sr + 0.7 * k);
        in[n] = (float) (0.5 * s);
    }
    std::vector<float> out = in;
    // Starts dry: the tone begins at full amplitude, so a delayed head at t=0
    // would read silence from before the stream and "click" by construction.
    // 0 -> +3 covers an up-shift starting from the dry head.
    const float pattern[] = { 0.0f, -2.0f, 0.0f, 3.0f, -6.0f, -2.0f, 3.0f, 0.0f, -1.0f, -7.0f };
    const size_t patternLen = sizeof(pattern) / sizeof(pattern[0]);
    const int block = 128;
    const size_t switchEvery = (size_t) (0.1 * sr) / (size_t) block * (size_t) block;
    size_t k = 0;
    for (size_t i = 0; i < out.size(); i += (size_t) block)
    {
        if (i % switchEvery == 0) dt.setSemitones(pattern[k++ % patternLen]);
        dt.process(out.data() + i, (int) std::min((size_t) block, out.size() - i));
    }
    // A hard jump between two heads would step by up to the peak-to-peak
    // amplitude (~1.0); the waveform's own steepest step is far smaller.
    const double limit = 1.5 * maxStep(in, 0);
    const double got = maxStep(out, 0);
    if (got > limit)
        for (size_t i = 1; i < out.size(); ++i)
            if (std::abs(out[i] - out[i - 1]) > limit)
            {
                std::fprintf(stderr, "  first big step at %.4f s (segment %zu)\n", i / sr, i / switchEvery);
                break;
            }
    CHECK(got <= limit, "largest step %.4f exceeds %.4f (input's own max x1.5)", got, limit);
}

static void test_back_to_zero_is_dry()
{
    const double sr = 48000.0;
    DropTuner dt;
    dt.prepare(sr);
    const auto in = bassNote(41.2, 1.2, sr);
    std::vector<float> out = in;
    const size_t half = (size_t) (0.5 * sr) / 256 * 256;
    dt.setSemitones(-6.0f);
    for (size_t i = 0; i < half; i += 256) dt.process(out.data() + i, 256);
    dt.setSemitones(0.0f);
    for (size_t i = half; i < out.size(); i += 256)
        dt.process(out.data() + i, (int) std::min((size_t) 256, out.size() - i));
    // After the 4 ms crossfade (plus at most one splice crossfade it had to
    // finish first) the signal is the untouched input again.
    const size_t from = half + (size_t) (0.02 * sr);
    bool same = true;
    for (size_t i = from; i < in.size(); ++i) same = same && in[i] == out[i];
    CHECK(same, "switching back to 0 must end in the dry signal");
    CHECK(dt.getDelaySamples() == 0.0, "delay after switching off: %g", dt.getDelaySamples());
}

static void test_switch_follows_new_amount()
{
    const double sr = 48000.0;
    DropTuner dt;
    dt.prepare(sr);
    const auto in = bassNote(55.0, 1.6, sr);
    std::vector<float> out = in;
    const size_t half = (size_t) (0.4 * sr) / 256 * 256;
    dt.setSemitones(-6.0f);
    for (size_t i = 0; i < out.size(); i += 256)
    {
        if (i == half) dt.setSemitones(-2.0f);
        dt.process(out.data() + i, (int) std::min((size_t) 256, out.size() - i));
    }
    const double expected = 55.0 * std::pow(2.0, -2.0 / 12.0);
    const double got = measureHz(out, (size_t) (0.6 * sr), (size_t) (1.0 * sr), sr, expected / 1.5, expected * 1.5);
    CHECK(got > 0.0 && std::abs(cents(got, expected)) < 10.0,
          "after -6 -> -2: expected %.2f Hz, got %.2f Hz", expected, got);
}

// ── Chart: shift points, priming, plucks ────────────────────────────────────

// Process `x` in blocks, pushing the song clock (= sample time + offset) before
// each block, as the renderer does once per frame.
static void runClocked(DropTuner& dt, std::vector<float>& x, size_t from, size_t to, double sr,
                       double clockOffset = 0.0, int block = 256)
{
    for (size_t i = from; i < to; i += (size_t) block)
    {
        dt.setClock(clockOffset + (double) i / sr, true);
        dt.process(x.data() + i, (int) std::min((size_t) block, to - i));
    }
}

// A run of plucked notes, each `noteSec` long, back to back; each is muted
// over its last 100 ms, like a player stopping the string before the next pluck.
static std::vector<float> noteRun(const std::vector<double>& hz, double noteSec, double sr)
{
    std::vector<float> x;
    const size_t mute = (size_t) (0.100 * sr);
    for (double f : hz)
    {
        auto n = bassNote(f, noteSec, sr);
        for (size_t i = 0; i < mute && i < n.size(); ++i) n[n.size() - 1 - i] *= (float) i / mute;
        x.insert(x.end(), n.begin(), n.end());
    }
    return x;
}

static void test_chart_shift_points_follow_the_clock()
{
    const double sr = 48000.0;
    DropTuner dt;
    dt.prepare(sr);
    dt.setSemitones(-5.0f);                        // ignored while the chart has shift points
    dt.setChart({ -1e9, 1.0 }, { -2.0f, 0.0f }, {}, {});
    std::vector<float> in(2 * (size_t) sr);
    for (size_t n = 0; n < in.size(); ++n) in[n] = (float) (0.4 * std::sin(2 * kPi * 55.0 * n / sr));
    auto out = in;
    runClocked(dt, out, 0, (size_t) (0.9 * sr) / 256 * 256, sr);
    CHECK(dt.getActiveSemitones() == -2.0f, "before 1.0 s: %g", dt.getActiveSemitones());
    const double expected = 55.0 * std::pow(2.0, -2.0 / 12.0);
    const double got = measureHz(out, (size_t) (0.4 * sr), (size_t) (0.85 * sr), sr, expected / 1.5, expected * 1.5);
    CHECK(got > 0.0 && std::abs(cents(got, expected)) < 10.0, "shift point -2: expected %.2f Hz, got %.2f", expected, got);
    runClocked(dt, out, (size_t) (0.9 * sr) / 256 * 256, out.size(), sr);
    CHECK(dt.getActiveSemitones() == 0.0f, "after 1.0 s: %g", dt.getActiveSemitones());
    bool dry = true;
    for (size_t i = (size_t) (1.05 * sr); i < out.size(); ++i) dry = dry && out[i] == in[i];
    CHECK(dry, "a 0 shift point must end in the dry signal");

    // Seek back before the change point: the earlier amount applies again.
    std::vector<float> more(in.begin(), in.begin() + (size_t) (0.2 * sr));
    runClocked(dt, more, 0, more.size(), sr, 0.3);
    CHECK(dt.getActiveSemitones() == -2.0f, "after seeking back to 0.3 s: %g", dt.getActiveSemitones());

    // Clearing the chart hands control back to setSemitones().
    dt.clearChart();
    std::vector<float> tail(in.begin(), in.begin() + (size_t) (0.1 * sr));
    runClocked(dt, tail, 0, tail.size(), sr, 0.5);
    CHECK(dt.getActiveSemitones() == -5.0f, "after clearChart: %g", dt.getActiveSemitones());
}

static void test_priming_loads_the_expected_period()
{
    // E1 then A1. Primed, the A1 period is in place from its pluck on; the
    // tracker alone would still report E1 until its window fills with A1.
    const double sr = 48000.0, noteSec = 0.5;
    const auto in = noteRun({ 41.2, 55.0 }, noteSec, sr);
    const size_t pluck = (size_t) (noteSec * sr);
    // A low note's pluck is detected at its first big peak (~13 ms for A1-E1),
    // so probe 25 ms in: the tracker's 57 ms window is still mostly E1 then.
    const size_t probe = pluck + (size_t) (0.025 * sr) / 256 * 256;

    DropTuner primed, plain;
    for (DropTuner* dt : { &primed, &plain })
    {
        dt->prepare(sr);
        dt->setSemitones(-6.0f);
    }
    primed.setChart({}, {}, { 0.0, noteSec }, { 41.2f, 55.0f });
    auto a = in, b = in;
    runClocked(primed, a, 0, probe, sr);
    runClocked(plain, b, 0, probe, sr);
    const double want = sr / 55.0;
    CHECK(std::abs(primed.getTrackedPeriod() - want) / want < 0.02,
          "primed period %.1f, expected %.1f (A1)", primed.getTrackedPeriod(), want);
    CHECK(std::abs(plain.getTrackedPeriod() - want) / want > 0.05,
          "unprimed tracker already on A1 (%.1f) — the test no longer shows priming", plain.getTrackedPeriod());
    CHECK(primed.getOnsetCount() >= 2, "plucks detected: %u", primed.getOnsetCount());
}

static void test_pluck_restarts_the_head()
{
    // At a pluck matching a chart note the delay drops to the minimum
    // (crossfaded), instead of wherever the splice cycle happened to be.
    const double sr = 48000.0, noteSec = 0.5;
    const auto in = noteRun({ 41.2, 41.2, 41.2, 41.2 }, noteSec, sr);
    DropTuner primed, plain;
    for (DropTuner* dt : { &primed, &plain })
    {
        dt->prepare(sr);
        dt->setSemitones(-6.0f);
    }
    primed.setChart({}, {}, { 0.0, 0.5, 1.0, 1.5 }, { 41.2f, 41.2f, 41.2f, 41.2f });
    double sumPrimed = 0.0, sumPlain = 0.0;
    auto a = in, b = in;
    size_t done = 0;
    for (int k = 1; k < 4; ++k)
    {
        // After the pluck is detected (~18 ms on E1: its first big peak) and
        // the 4 ms crossfade.
        const size_t at = (size_t) ((k * noteSec + 0.030) * sr) / 256 * 256;
        runClocked(primed, a, done, at, sr);
        runClocked(plain, b, done, at, sr);
        done = at;
        sumPrimed += primed.getDelaySamples();
        sumPlain += plain.getDelaySamples();
    }
    // Restarted: the minimum (crossfade + 2 samples) plus the drift since the
    // restart (rate 0.71 → 0.29 ms per ms, ~8 ms) — well under one E1 span
    // (24 ms), where the unprimed head can be anywhere.
    const double minMs = 4.0 + 2.0 / 48.0;
    const double primedMs = sumPrimed / 3.0 / sr * 1000.0, plainMs = sumPlain / 3.0 / sr * 1000.0;
    CHECK(primedMs < minMs + 4.0, "delay after a primed pluck: %.1f ms (min %.1f; unprimed %.1f)",
          primedMs, minMs, plainMs);
    CHECK(primed.getOnsetCount() >= 4, "plucks detected: %u", primed.getOnsetCount());
}

static void test_wrong_chart_falls_back_to_the_player()
{
    // The chart expects A1 but the player plays D2: the guided search fails,
    // the free search follows the player, and the pitch is still right.
    const double sr = 48000.0;
    DropTuner dt;
    dt.prepare(sr);
    dt.setSemitones(-6.0f);
    dt.setChart({}, {}, { 0.0 }, { 55.0f });
    auto x = bassNote(73.42, 1.2, sr);
    runClocked(dt, x, 0, x.size(), sr);
    const double expected = 73.42 * std::pow(2.0, -6.0 / 12.0);
    const double got = measureHz(x, (size_t) (0.5 * sr), (size_t) (0.9 * sr), sr, expected / 1.5, expected * 1.5);
    CHECK(got > 0.0 && std::abs(cents(got, expected)) < 10.0, "wrong chart note: expected %.2f Hz, got %.2f", expected, got);
}

static void test_chart_input_is_sanitised()
{
    const double sr = 48000.0;
    DropTuner dt;
    dt.prepare(sr);
    // Unsorted, non-finite, out-of-range pitches and mismatched lengths: no crash,
    // the bad entries are dropped and the rest still applies.
    dt.setChart({ 0.5, -1e9, std::nan("") }, { 0.0f, -3.0f, -2.0f }, { 1.0, 0.2, 0.3 },
                { std::nanf(""), 5000.0f });
    dt.setClock(std::nan(""), true);                // ignored
    std::vector<float> x((size_t) (0.3 * sr), 0.0f);
    runClocked(dt, x, 0, x.size(), sr);
    CHECK(dt.getActiveSemitones() == -3.0f, "sorted shift points: %g", dt.getActiveSemitones());
    CHECK(dt.getHintPeriod() == 0.0, "no valid note → no hint: %g", dt.getHintPeriod());
    for (float v : x) CHECK(std::isfinite(v) && v == 0.0f, "silence stays silent");
}

int main()
{
    test_bypass_is_identity();
    test_shift_down(48000.0, 41.2, -6.0f);   // 4-string bass low E → Bb
    test_shift_down(48000.0, 55.0, -1.0f);   // A string → Ab (Eb-standard style)
    test_shift_down(48000.0, 98.0, -6.0f);   // G string
    test_shift_down(44100.0, 41.2, -6.0f);
    test_shift_down(96000.0, 41.2, -6.0f);
    test_shift_up();
    test_shift_down(48000.0, 55.0, -12.0f);  // octave down: A1 → A0 (27.5 Hz)
    test_shift_down(48000.0, 55.0, 12.0f);   // octave up:   A1 → A2
    test_block_size_independent();
    test_clamping();
    test_silence_stays_silent();
    test_switching_is_click_free();
    test_back_to_zero_is_dry();
    test_switch_follows_new_amount();
    test_chart_shift_points_follow_the_clock();
    test_priming_loads_the_expected_period();
    test_pluck_restarts_the_head();
    test_wrong_chart_falls_back_to_the_player();
    test_chart_input_is_sanitised();
    if (failures == 0) std::printf("drop_tuner_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}

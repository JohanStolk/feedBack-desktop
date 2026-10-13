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

int main()
{
    test_bypass_is_identity();
    test_shift_down(48000.0, 41.2, -6.0f);   // 4-string bass low E → Bb
    test_shift_down(48000.0, 55.0, -1.0f);   // A string → Ab (Eb-standard style)
    test_shift_down(48000.0, 98.0, -6.0f);   // G string
    test_shift_down(44100.0, 41.2, -6.0f);
    test_shift_down(96000.0, 41.2, -6.0f);
    test_shift_up();
    test_block_size_independent();
    test_clamping();
    test_silence_stays_silent();
    test_switching_is_click_free();
    test_back_to_zero_is_dry();
    test_switch_follows_new_amount();
    if (failures == 0) std::printf("drop_tuner_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}

#include "DropTuner.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace slopsmith {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kFmin = 35.0;         // lowest tracked pitch: below a 4-string bass's low E (41 Hz)
constexpr double kFmax = 400.0;
constexpr double kYinThreshold = 0.15;
constexpr double kRmsFloor = 1e-3;     // quieter than this = unvoiced (keeps the default span)
constexpr double kXfadeMs = 4.0;
constexpr double kDefaultSpanMs = 20.0;
constexpr double kMinSpanMs = 8.0;
constexpr double kHopMs = 5.3;         // tracker update interval (~256 samples @ 48 kHz)
// Pitch analysis runs on a lowpassed, decimated copy at about this rate: bass
// fundamentals stop at kFmax, so ~11-12 kHz keeps every harmonic YIN needs
// while its frames (and FFT) shrink 4x at 44.1/48 kHz, 8x at 88.2/96 kHz.
constexpr double kAnalysisRateHz = 11000.0;
constexpr int kDecimTapsPerFactor = 8;  // FIR length = 8 * factor + 1
constexpr double kPrimeLeadSec = 0.010; // load a note's expected period this early
constexpr double kOnsetToleranceSec = 0.080; // a pluck this close to a chart note is that note
constexpr double kHintRangeSemitones = 1.5;  // primed search: expected note ± this
constexpr double kSeekBackSec = 0.05, kSeekForwardSec = 0.5; // clock jumps that mean a seek
// Onset detector (prototype values): fast (1 ms) envelope above 3x the slow
// (50 ms) one and above a floor (~-46 dBFS), at most one per 60 ms.
constexpr double kOnsetRatio = 3.0, kOnsetFloor = 0.005, kOnsetHoldoffMs = 60.0;

size_t nextPow2(size_t v)
{
    size_t p = 1;
    while (p < v) p <<= 1;
    return p;
}
} // namespace

void DropTuner::prepare(double sampleRate)
{
    sr = (sampleRate > 0.0 && std::isfinite(sampleRate)) ? sampleRate : 48000.0;
    xfadeLen = std::max(1, (int) std::lround(kXfadeMs * sr / 1000.0));
    minDelay = (double) xfadeLen + 2.0;
    defaultSpan = kDefaultSpanMs * sr / 1000.0;
    minSpan = std::max(kMinSpanMs * sr / 1000.0, 2.0 * xfadeLen);
    hop = std::max(32, (int) std::lround(kHopMs * sr / 1000.0));
    aFast = std::exp(-1.0 / (0.001 * sr));
    aSlow = std::exp(-1.0 / (0.050 * sr));
    onsetHoldoff = std::max(1, (int) std::lround(kOnsetHoldoffMs * sr / 1000.0));
    attackLen = std::max(1, (int) std::lround(kAttackMs * sr / 1000.0));

    // Analysis path: windowed-sinc lowpass (Blackman) at 0.45 x the analysis
    // rate, evaluated once per `decim` input samples straight from `ring`.
    decim = std::max(1, (int) std::floor(sr / kAnalysisRateHz));
    asr = sr / decim;
    decimTaps.clear();
    if (decim > 1)
    {
        const int taps = kDecimTapsPerFactor * decim + 1;
        const double fc = 0.45 * asr / sr;          // cutoff, cycles per input sample
        double sum = 0.0;
        for (int i = 0; i < taps; ++i)
        {
            const double m = i - (taps - 1) / 2.0;
            const double sinc = m == 0.0 ? 2.0 * fc : std::sin(2.0 * kPi * fc * m) / (kPi * m);
            const double w = 0.42 - 0.5 * std::cos(2.0 * kPi * i / (taps - 1)) + 0.08 * std::cos(4.0 * kPi * i / (taps - 1));
            decimTaps.push_back((float) (sinc * w));
            sum += sinc * w;
        }
        for (auto& t : decimTaps) t = (float) (t / sum);   // unity gain at DC
    }

    // YIN sizes, in analysis samples.
    tauMax = (int) (asr / kFmin);
    tauMin = std::max(2, (int) (asr / kFmax));
    frameLen = 2 * tauMax;                          // YIN needs two periods of fmin
    // The packed correlation in estimatePeriod() has no wrap-around as long as
    // the FFT covers the frame (j + tau < len <= fftSize).
    fftSize = (int) nextPow2((size_t) frameLen);

    // Input history must cover the largest read delay (min delay + one span +
    // crossfade drift; spans are at most a few periods) and the decimation FIR.
    const double tauMaxIn = (double) tauMax * decim;
    const size_t needed = (size_t) (minDelay + std::max(defaultSpan, minSpan + tauMaxIn) * 2.0)
                        + decimTaps.size() + 4096;
    ring.assign(nextPow2(needed), 0.0f);
    mask = (uint64_t) ring.size() - 1;
    aring.assign(nextPow2((size_t) frameLen + 64), 0.0f);
    amask = (uint64_t) aring.size() - 1;

    frame.assign((size_t) frameLen, 0.0);
    cumSq.assign((size_t) frameLen + 1, 0.0);
    cmnd.assign((size_t) tauMax + 2, 1.0);
    spec.assign((size_t) fftSize, {});
    twiddles.resize((size_t) fftSize / 2);
    for (int k = 0; k < fftSize / 2; ++k)
        twiddles[(size_t) k] = std::polar(1.0, -2.0 * kPi * k / fftSize);
    bitrev.resize((size_t) fftSize);
    int bits = 0;
    while ((1 << bits) < fftSize) ++bits;
    for (int i = 0; i < fftSize; ++i)
    {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        bitrev[(size_t) i] = r;
    }
    reset();
}

void DropTuner::reset() noexcept
{
    std::fill(ring.begin(), ring.end(), 0.0f);
    written = 0;
    std::fill(aring.begin(), aring.end(), 0.0f);
    aWritten = 0;
    decimPhase = 0;
    // Start as a dry passthrough (head at 0, rate 1); a requested shift then
    // crossfades in like any other change.
    active = 0.0f;
    rate = 1.0;
    delay = newDelay = 0.0;
    xfadeLeft = 0;
    period = 0.0;
    hopCounter = 0;
    hint = 0.0;
    sincePluck = kNoPluck;
    restartPending = false;
    envFast = envSlow = 0.0;
    sinceOnset = onsetHoldoff;
    lastSongTime = -1e300;                         // re-sync the chart position
    haveClock = false;
    seenClockGen = clockGen.load(std::memory_order_acquire) - 1;  // re-read the clock
    avgDelay = 0.0;
    avgSeeded = false;
    attackSum = 0.0;
    attackN = 0;
    statShifting.store(false, std::memory_order_relaxed);
    statNowMs.store(0.0f, std::memory_order_relaxed);
    statAvgMs.store(0.0f, std::memory_order_relaxed);
    statAttackMs.store(0.0f, std::memory_order_relaxed);
    statPlucks.store(0, std::memory_order_relaxed);
}

DropTuner::DelayStats DropTuner::getDelayStats() const noexcept
{
    DelayStats d;
    d.shifting = statShifting.load(std::memory_order_relaxed);
    d.nowMs = statNowMs.load(std::memory_order_relaxed);
    d.averageMs = statAvgMs.load(std::memory_order_relaxed);
    d.attackMs = statAttackMs.load(std::memory_order_relaxed);
    d.plucks = statPlucks.load(std::memory_order_relaxed);
    return d;
}

DropTuner::~DropTuner()
{
    delete chart;
    delete pendingChart.exchange(nullptr);
    delete retiredChart.exchange(nullptr);
}

void DropTuner::setChart(const std::vector<double>& shiftTimes, const std::vector<float>& shiftSemis,
                         const std::vector<double>& noteTimes, const std::vector<float>& noteHz)
{
    auto* c = new Chart();
    std::vector<std::pair<double, float>> pairs;
    pairs.reserve(std::min(shiftTimes.size(), shiftSemis.size()));
    for (size_t i = 0; i < shiftTimes.size() && i < shiftSemis.size(); ++i)
        if (std::isfinite(shiftTimes[i]) && std::isfinite(shiftSemis[i]))
            pairs.emplace_back(shiftTimes[i], std::clamp(shiftSemis[i], -kMaxSemitones, kMaxSemitones));
    std::stable_sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& p : pairs) { c->shiftT.push_back(p.first); c->shiftSemis.push_back(p.second); }

    pairs.clear();
    for (size_t i = 0; i < noteTimes.size() && i < noteHz.size(); ++i)
        if (std::isfinite(noteTimes[i]) && std::isfinite(noteHz[i]) && noteHz[i] >= kFmin && noteHz[i] <= kFmax)
            pairs.emplace_back(noteTimes[i], noteHz[i]);
    std::stable_sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& p : pairs) { c->noteT.push_back(p.first); c->noteHz.push_back(p.second); }

    delete retiredChart.exchange(nullptr, std::memory_order_acq_rel);   // freed off the audio thread
    delete pendingChart.exchange(c, std::memory_order_acq_rel);        // never adopted: drop it
}

void DropTuner::clearChart()
{
    setChart({}, {}, {}, {});
}

void DropTuner::setClock(double songTime, bool playing) noexcept
{
    if (!std::isfinite(songTime)) return;
    clockTime.store(songTime, std::memory_order_relaxed);
    clockPlaying.store(playing, std::memory_order_relaxed);
    clockGen.fetch_add(1, std::memory_order_release);
}

void DropTuner::adoptPendingChart() noexcept
{
    // Only while the previous retired chart has been collected, so nothing leaks.
    if (retiredChart.load(std::memory_order_acquire) != nullptr) return;
    Chart* next = pendingChart.exchange(nullptr, std::memory_order_acq_rel);
    if (next == nullptr) return;
    retiredChart.store(chart, std::memory_order_release);
    chart = next;
    hint = 0.0;
    sincePluck = kNoPluck;
    lastSongTime = -1e300;
}

void DropTuner::syncChartTo(double songTime) noexcept
{
    const auto& st = chart->shiftT;
    shiftIdx = 0;
    if (! st.empty())
    {
        const auto it = std::upper_bound(st.begin(), st.end(), songTime);
        shiftIdx = it == st.begin() ? 0 : (size_t) (it - st.begin()) - 1;
    }
    const auto& nt = chart->noteT;
    nextNote = (size_t) (std::upper_bound(nt.begin(), nt.end(), songTime + kPrimeLeadSec) - nt.begin());
}

bool DropTuner::onsetStep(float v) noexcept
{
    const double a = std::abs((double) v);
    envFast = a > envFast ? a + aFast * (envFast - a) : aFast * envFast;
    envSlow = aSlow * envSlow + (1.0 - aSlow) * a;
    if (++sinceOnset >= onsetHoldoff && envFast > kOnsetFloor && envFast > kOnsetRatio * envSlow)
    {
        sinceOnset = 0;
        ++onsetCount;
        return true;
    }
    return false;
}

void DropTuner::setSemitones(float semitones) noexcept
{
    if (!std::isfinite(semitones)) semitones = 0.0f;
    requested.store(std::clamp(semitones, -kMaxSemitones, kMaxSemitones), std::memory_order_relaxed);
}

float DropTuner::readAt(double d) const noexcept
{
    // The current sample is index written - 1; read d samples behind it. An
    // up-shift fading out the dry head drifts below 0 for a few ms: hold it at
    // the newest sample rather than read ring slots not yet written.
    if (d < 0.0) d = 0.0;
    const double pos = (double) (written - 1) - d;
    if (pos < 0.0) return 0.0f;
    const uint64_t i = (uint64_t) pos;
    const double f = pos - (double) i;
    const float a = ring[(size_t) (i & mask)];
    const float b = ring[(size_t) ((i + 1) & mask)];
    return (float) (a * (1.0 - f) + b * f);
}

double DropTuner::spanFor(double p) const noexcept
{
    if (p <= 0.0) return defaultSpan;
    const double k = std::max(1.0, std::ceil(minSpan / p));
    return k * p;
}

void DropTuner::fft(std::complex<double>* a) const noexcept
{
    const int n = fftSize;
    for (int i = 0; i < n; ++i)
        if (i < bitrev[(size_t) i]) std::swap(a[i], a[bitrev[(size_t) i]]);
    for (int len = 2; len <= n; len <<= 1)
    {
        const int half = len >> 1, step = n / len;
        for (int i = 0; i < n; i += len)
            for (int j = 0; j < half; ++j)
            {
                const std::complex<double> u = a[i + j];
                const std::complex<double> v = a[i + j + half] * twiddles[(size_t) (j * step)];
                a[i + j] = u + v;
                a[i + j + half] = u - v;
            }
    }
}

double DropTuner::estimatePeriod(int len, int lo, int hi) noexcept
{
    // Newest `len` analysis samples, mean removed. Searches periods in
    // [lo, hi), all in analysis samples.
    const uint64_t start = aWritten - (uint64_t) len;
    double mean = 0.0;
    for (int j = 0; j < len; ++j)
    {
        frame[(size_t) j] = aring[(size_t) ((start + (uint64_t) j) & amask)];
        mean += frame[(size_t) j];
    }
    mean /= len;
    double energy = 0.0;
    for (int j = 0; j < len; ++j)
    {
        frame[(size_t) j] -= mean;
        energy += frame[(size_t) j] * frame[(size_t) j];
    }
    if (std::sqrt(energy / len) < kRmsFloor) return 0.0;

    // r(tau) = sum_{j<w} x[j] x[j+tau], via one complex FFT: pack a = x[0:w] in
    // the real part and b = x in the imaginary part, split the spectra, form
    // conj(A)·B, inverse-transform. No wrap-around: j + tau < len <= fftSize.
    const int w = len - hi;
    for (int k = 0; k < fftSize; ++k)
    {
        const double re = (k < w) ? frame[(size_t) k] : 0.0;
        const double im = (k < len) ? frame[(size_t) k] : 0.0;
        spec[(size_t) k] = { re, im };
    }
    fft(spec.data());
    // Z[k] = A[k] + i B[k]; A[k] = (Z[k] + conj(Z[-k]))/2, B[k] = (Z[k] - conj(Z[-k]))/(2i).
    // Compute C[k] = conj(A[k]) B[k] for all k in place, pairing k with n-k.
    const int n = fftSize;
    for (int k = 0; k <= n / 2; ++k)
    {
        const int nk = (n - k) & (n - 1);
        const std::complex<double> zk = spec[(size_t) k], znk = spec[(size_t) nk];
        const std::complex<double> ak = 0.5 * (zk + std::conj(znk));
        const std::complex<double> bk = std::complex<double>(0.0, -0.5) * (zk - std::conj(znk));
        const std::complex<double> ank = 0.5 * (znk + std::conj(zk));
        const std::complex<double> bnk = std::complex<double>(0.0, -0.5) * (znk - std::conj(zk));
        spec[(size_t) k] = std::conj(ak) * bk;
        spec[(size_t) nk] = std::conj(ank) * bnk;
    }
    // Inverse FFT via conjugation: ifft(X) = conj(fft(conj(X))) / n.
    for (auto& v : spec) v = std::conj(v);
    fft(spec.data());
    // spec[tau].real / n == r(tau) (the conj back is irrelevant for the real part).

    for (int j = 0; j < len; ++j)
        cumSq[(size_t) j + 1] = cumSq[(size_t) j] + frame[(size_t) j] * frame[(size_t) j];
    const double e0 = cumSq[(size_t) w];

    // Cumulative-mean-normalised difference function.
    double running = 0.0;
    cmnd[0] = 1.0;
    for (int tau = 1; tau <= hi; ++tau)
    {
        const double r = spec[(size_t) tau].real() / n;
        const double eTau = cumSq[(size_t) (tau + w)] - cumSq[(size_t) tau];
        const double d = std::max(0.0, e0 + eTau - 2.0 * r);
        running += d;
        cmnd[(size_t) tau] = running > 1e-12 ? d * tau / running : 1.0;
    }

    int tau = -1;
    for (int t = lo; t < hi; ++t)
    {
        if (cmnd[(size_t) t] < kYinThreshold)
        {
            while (t + 1 < hi && cmnd[(size_t) t + 1] < cmnd[(size_t) t]) ++t;
            tau = t;
            break;
        }
    }
    if (tau < 1) return 0.0;
    const double y0 = cmnd[(size_t) tau - 1], y1 = cmnd[(size_t) tau], y2 = cmnd[(size_t) tau + 1];
    const double den = y0 - 2.0 * y1 + y2;
    return std::abs(den) > 1e-12 ? tau + 0.5 * (y0 - y2) / den : (double) tau;
}

void DropTuner::track() noexcept
{
    // Primed: search near the expected note first, on a window just long
    // enough for it (less of the previous note in it). Otherwise, or if the
    // player isn't on that note, a free search; with no pitch at all keep the
    // expected period (0 without a chart = unvoiced, default span).
    // `hint` and `period` are in input samples; the search runs in analysis
    // samples (decim x fewer).
    if (hint > 0.0)
    {
        const double k = std::pow(2.0, kHintRangeSemitones / 12.0);
        const double hintA = hint / decim;
        const int lo = std::max(tauMin, (int) std::floor(hintA / k));
        const int hi = std::min(tauMax, (int) std::ceil(hintA * k));
        const int len = std::min(frameLen, 2 * hi);
        // Right after a pluck that matched a chart note the window still holds
        // the previous note, which the searches would lock onto: trust the
        // chart until the window is mostly the new note.
        if (sincePluck < (uint64_t) len * (uint64_t) decim) { period = hint; return; }
        if (lo < hi && aWritten >= (uint64_t) len)
        {
            const double p = estimatePeriod(len, lo, hi);
            if (p > 0.0) { period = p * decim; return; }
        }
    }
    if (aWritten >= (uint64_t) frameLen)
    {
        const double p = estimatePeriod(frameLen, tauMin, tauMax);
        if (p > 0.0) { period = p * decim; return; }
    }
    period = hint;
}

bool DropTuner::process(float* samples, int numSamples) noexcept
{
    if (samples == nullptr || numSamples <= 0 || ring.empty()) return false;
    bool touched = false;

    adoptPendingChart();
    const uint32_t gen = clockGen.load(std::memory_order_acquire);
    if (gen != seenClockGen)
    {
        seenClockGen = gen;
        baseTime = clockTime.load(std::memory_order_relaxed);
        basePlaying = clockPlaying.load(std::memory_order_relaxed);
        baseWritten = written;
        haveClock = true;
    }
    const float req = requested.load(std::memory_order_relaxed);
    const bool followShifts = chart != nullptr && ! chart->shiftT.empty();
    const bool primed = chart != nullptr && ! chart->noteT.empty();
    double blockDelaySum = 0.0;
    int blockDelayN = 0;

    for (int i = 0; i < numSamples; ++i)
    {
        const float in = samples[i];
        ring[(size_t) (written & mask)] = in;
        ++written;
        ++sincePluck;
        // Analysis copy: one lowpassed sample per `decim` input samples. Only
        // needed while shifting (the tracker is idle in passthrough), but kept
        // current anyway so a shift that starts mid-note has history.
        if (++decimPhase >= decim)
        {
            decimPhase = 0;
            float a = in;
            if (! decimTaps.empty())
            {
                const size_t taps = decimTaps.size();
                double acc = 0.0;
                if (written >= taps)
                    for (size_t t = 0; t < taps; ++t)
                        acc += decimTaps[t] * ring[(size_t) ((written - 1 - t) & mask)];
                a = (float) acc;
            }
            aring[(size_t) (aWritten & amask)] = a;
            ++aWritten;
        }

        float target = req;
        if (followShifts || primed)
        {
            const double now = ! haveClock ? -1e300
                : basePlaying ? baseTime + (double) (written - baseWritten) / sr : baseTime;
            if (now < lastSongTime - kSeekBackSec || now > lastSongTime + kSeekForwardSec)
                syncChartTo(now);
            lastSongTime = now;
            if (followShifts)
            {
                const auto& st = chart->shiftT;
                while (shiftIdx + 1 < st.size() && st[shiftIdx + 1] <= now) ++shiftIdx;
                target = chart->shiftSemis[shiftIdx];
            }
            if (primed)
            {
                const auto& nt = chart->noteT;
                // A note is coming: splice at its expected period from the start.
                while (nextNote < nt.size() && nt[nextNote] - kPrimeLeadSec <= now)
                    hint = period = sr / chart->noteHz[nextNote++];
                // A pluck near a chart note: that note, with the head right
                // behind the input (crossfaded) so the attack isn't delayed.
                if (onsetStep(in) && haveClock && basePlaying)
                {
                    const auto it = std::lower_bound(nt.begin(), nt.end(), now);
                    size_t best = nt.size();
                    double bestDist = kOnsetToleranceSec;
                    if (it != nt.end() && *it - now <= bestDist) { best = (size_t) (it - nt.begin()); bestDist = *it - now; }
                    if (it != nt.begin() && now - *(it - 1) <= bestDist) best = (size_t) (it - nt.begin()) - 1;
                    if (best < nt.size())
                    {
                        hint = period = sr / chart->noteHz[best];
                        sincePluck = 0;
                        restartPending = true;
                        attackSum = 0.0;
                        attackN = 0;
                    }
                }
            }
        }

        // A new amount takes effect between crossfades: crossfade from the
        // current head to a fresh one right behind the input (or to the dry
        // signal for 0). No click, so the amount can change per note.
        if (target != active && xfadeLeft == 0)
        {
            active = target;
            rate = std::pow(2.0, active / 12.0);
            const double head = (active == 0.0f) ? 0.0 : minDelay;
            if (delay != head)
            {
                newDelay = head;
                xfadeLeft = xfadeLen;
            }
        }

        // Restart for a matched pluck, once any running crossfade is done.
        if (restartPending && xfadeLeft == 0)
        {
            restartPending = false;
            if (active != 0.0f && delay > minDelay + 1.0)
            {
                newDelay = minDelay;
                xfadeLeft = xfadeLen;
            }
        }

        // Dry passthrough: off and fully faded out. History is still recorded
        // so the next shift starts clean.
        if (active == 0.0f && xfadeLeft == 0 && delay == 0.0) continue;

        if (active != 0.0f && ++hopCounter >= hop)
        {
            hopCounter = 0;
            track();
        }

        const double span = spanFor(period);
        if (xfadeLeft == 0)
        {
            if (rate < 1.0 && delay >= minDelay + span)
            {
                newDelay = delay - span;                // jump to the newer head
                xfadeLeft = xfadeLen;
            }
            else if (rate > 1.0 && delay <= minDelay)
            {
                newDelay = delay + span;                // upward: jump to the older head
                xfadeLeft = xfadeLen;
            }
        }

        float y;
        double effDelay;                           // what this output sample lags the input by
        if (xfadeLeft > 0)
        {
            const double g = 1.0 - (double) xfadeLeft / xfadeLen;
            y = (float) ((1.0 - g) * readAt(delay) + g * readAt(newDelay));
            effDelay = (1.0 - g) * delay + g * newDelay;
        }
        else
        {
            y = readAt(delay);
            effDelay = delay;
        }
        blockDelaySum += effDelay;
        ++blockDelayN;
        if (sincePluck < (uint64_t) attackLen)
        {
            attackSum += effDelay;
            ++attackN;
        }
        else if (attackN > 0)
        {
            statAttackMs.store((float) (attackSum / attackN * 1000.0 / sr), std::memory_order_relaxed);
            statPlucks.fetch_add(1, std::memory_order_relaxed);
            attackSum = 0.0;
            attackN = 0;
        }
        delay += 1.0 - rate;
        if (xfadeLeft > 0)
        {
            newDelay += 1.0 - rate;
            if (--xfadeLeft == 0) delay = newDelay;
        }
        samples[i] = y;
        touched = true;
    }

    if (blockDelayN > 0)
    {
        const double mean = blockDelaySum / blockDelayN;
        // ~1 s average over the time spent shifting.
        const double a = 1.0 - std::exp(-(double) blockDelayN / sr / 1.0);
        avgDelay = avgSeeded ? avgDelay + a * (mean - avgDelay) : mean;
        avgSeeded = true;
        statNowMs.store((float) (mean * 1000.0 / sr), std::memory_order_relaxed);
        statAvgMs.store((float) (avgDelay * 1000.0 / sr), std::memory_order_relaxed);
        statShifting.store(true, std::memory_order_relaxed);
    }
    else
    {
        statNowMs.store(0.0f, std::memory_order_relaxed);
        statShifting.store(false, std::memory_order_relaxed);
    }
    return touched;
}

} // namespace slopsmith

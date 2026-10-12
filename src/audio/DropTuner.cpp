#include "DropTuner.h"

#include <algorithm>
#include <cmath>

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

    tauMax = (int) (sr / kFmin);
    tauMin = std::max(2, (int) (sr / kFmax));
    frameLen = 2 * tauMax;                          // YIN needs two periods of fmin
    // Linear cross-correlation of the first (frameLen - tauMax) samples against
    // the whole frame needs frameLen + tauMax points without wrap-around.
    fftSize = (int) nextPow2((size_t) (frameLen + tauMax));

    // History must cover the analysis frame and the largest read delay
    // (min delay + one span + crossfade drift); spans are at most a few periods.
    const size_t needed = (size_t) frameLen + (size_t) (minDelay + std::max(defaultSpan, minSpan + tauMax) * 2.0) + 4096;
    ring.assign(nextPow2(needed), 0.0f);
    mask = (uint64_t) ring.size() - 1;

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
    delay = newDelay = minDelay;
    xfadeLeft = 0;
    period = 0.0;
    hopCounter = 0;
}

void DropTuner::setSemitones(float semitones) noexcept
{
    if (!std::isfinite(semitones)) semitones = 0.0f;
    requested.store(std::clamp(semitones, -kMaxSemitones, kMaxSemitones), std::memory_order_relaxed);
}

float DropTuner::readAt(double d) const noexcept
{
    // The current sample is index written - 1; read d samples behind it.
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

double DropTuner::estimatePeriod() noexcept
{
    // Most recent frameLen samples, mean removed.
    const uint64_t start = written - (uint64_t) frameLen;
    double mean = 0.0;
    for (int j = 0; j < frameLen; ++j)
    {
        frame[(size_t) j] = ring[(size_t) ((start + (uint64_t) j) & mask)];
        mean += frame[(size_t) j];
    }
    mean /= frameLen;
    double energy = 0.0;
    for (int j = 0; j < frameLen; ++j)
    {
        frame[(size_t) j] -= mean;
        energy += frame[(size_t) j] * frame[(size_t) j];
    }
    if (std::sqrt(energy / frameLen) < kRmsFloor) return 0.0;

    // r(tau) = sum_{j<w} x[j] x[j+tau], via one complex FFT: pack a = x[0:w] in
    // the real part and b = x in the imaginary part, split the spectra, form
    // conj(A)·B, inverse-transform.
    const int w = frameLen - tauMax;
    for (int k = 0; k < fftSize; ++k)
    {
        const double re = (k < w) ? frame[(size_t) k] : 0.0;
        const double im = (k < frameLen) ? frame[(size_t) k] : 0.0;
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

    for (int j = 0; j < frameLen; ++j)
        cumSq[(size_t) j + 1] = cumSq[(size_t) j] + frame[(size_t) j] * frame[(size_t) j];
    const double e0 = cumSq[(size_t) w];

    // Cumulative-mean-normalised difference function.
    double running = 0.0;
    cmnd[0] = 1.0;
    for (int tau = 1; tau <= tauMax; ++tau)
    {
        const double r = spec[(size_t) tau].real() / n;
        const double eTau = cumSq[(size_t) (tau + w)] - cumSq[(size_t) tau];
        const double d = std::max(0.0, e0 + eTau - 2.0 * r);
        running += d;
        cmnd[(size_t) tau] = running > 1e-12 ? d * tau / running : 1.0;
    }

    int tau = -1;
    for (int t = tauMin; t < tauMax; ++t)
    {
        if (cmnd[(size_t) t] < kYinThreshold)
        {
            while (t + 1 < tauMax && cmnd[(size_t) t + 1] < cmnd[(size_t) t]) ++t;
            tau = t;
            break;
        }
    }
    if (tau < 1) return 0.0;
    const double y0 = cmnd[(size_t) tau - 1], y1 = cmnd[(size_t) tau], y2 = cmnd[(size_t) tau + 1];
    const double den = y0 - 2.0 * y1 + y2;
    return std::abs(den) > 1e-12 ? tau + 0.5 * (y0 - y2) / den : (double) tau;
}

void DropTuner::process(float* samples, int numSamples) noexcept
{
    if (samples == nullptr || numSamples <= 0 || ring.empty()) return;

    const float target = requested.load(std::memory_order_relaxed);
    if (target != active)
    {
        // New amount: restart the head right behind the input. A one-off glitch
        // on a settings change is fine; no stale delay is carried over.
        active = target;
        rate = std::pow(2.0, active / 12.0);
        delay = newDelay = minDelay;
        xfadeLeft = 0;
        period = 0.0;
    }
    const bool bypass = (active == 0.0f);

    for (int i = 0; i < numSamples; ++i)
    {
        ring[(size_t) (written & mask)] = samples[i];
        ++written;
        if (bypass) continue;                       // history still recorded, for a clean start

        if (++hopCounter >= hop)
        {
            hopCounter = 0;
            if (written >= (uint64_t) frameLen) period = estimatePeriod();
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
        if (xfadeLeft > 0)
        {
            const double g = 1.0 - (double) xfadeLeft / xfadeLen;
            y = (float) ((1.0 - g) * readAt(delay) + g * readAt(newDelay));
        }
        else
        {
            y = readAt(delay);
        }
        delay += 1.0 - rate;
        if (xfadeLeft > 0)
        {
            newDelay += 1.0 - rate;
            if (--xfadeLeft == 0) delay = newDelay;
        }
        samples[i] = y;
    }
}

} // namespace slopsmith

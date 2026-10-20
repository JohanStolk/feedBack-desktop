// Unit test for slopsmith::CallbackLoad (the Audio screen's CPU figure).
// JUCE-free; exit 0 = pass.
#include "CallbackLoad.h"

#include <cmath>
#include <cstdio>
#include <thread>

using slopsmith::CallbackLoad;

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

static void test_average_and_peak()
{
    CallbackLoad l;
    CHECK(l.getAverage() == 0.0f && l.takePeak() == 0.0f, "starts idle");
    // 256 samples at 48 kHz = 5.333 ms budget; 1.333 ms of work = 25 %.
    const double budget = 256.0 / 48000.0;
    for (int i = 0; i < 400; ++i) l.add(0.25 * budget, 256, 48000.0);   // ~2 s of audio
    CHECK(std::abs(l.getAverage() - 0.25f) < 0.005f, "steady 25%%: %.3f", l.getAverage());
    l.add(0.9 * budget, 256, 48000.0);                                  // one slow block
    CHECK(l.getAverage() < 0.30f, "one spike barely moves the average: %.3f", l.getAverage());
    CHECK(std::abs(l.takePeak() - 0.9f) < 0.001f, "peak = the slow block");
    CHECK(l.takePeak() == 0.0f, "peak resets on read");
    CHECK(l.getBlockCount() == 401, "blocks counted: %llu", (unsigned long long) l.getBlockCount());
}

static void test_same_meaning_at_any_buffer_size()
{
    // The time constant is in seconds of audio: after 0.5 s at a new level the
    // average has covered the same share of the step at 64 and at 1024 samples.
    for (int bs : { 64, 1024 })
    {
        CallbackLoad l;
        const double budget = bs / 48000.0;
        const int perHalfSecond = (int) std::lround(0.5 / budget);
        l.add(0.0, bs, 48000.0);
        for (int i = 0; i < perHalfSecond; ++i) l.add(0.5 * budget, bs, 48000.0);
        CHECK(std::abs(l.getAverage() - 0.5f * (1.0f - std::exp(-1.0f))) < 0.02f,
              "bs %d: %.3f after 0.5 s", bs, l.getAverage());
    }
}

static void test_bad_input_is_ignored()
{
    CallbackLoad l;
    l.add(0.001, 0, 48000.0);
    l.add(0.001, 256, 0.0);
    l.add(std::nan(""), 256, 48000.0);
    l.add(-1.0, 256, 48000.0);
    CHECK(l.getBlockCount() == 0 && l.getAverage() == 0.0f, "ignored");
}

static void test_scope_times_the_body()
{
    CallbackLoad l;
    {
        // 10 ms budget (480 samples at 48 kHz); sleep ~2 ms → roughly 20 %.
        const CallbackLoad::Scope s(l, 480, 48000.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const float p = l.takePeak();
    CHECK(p > 0.15f && p < 2.0f, "scoped 2 ms of a 10 ms budget: %.2f", p);
    l.reset();
    CHECK(l.getAverage() == 0.0f && l.takePeak() == 0.0f, "reset");
}

int main()
{
    test_average_and_peak();
    test_same_meaning_at_any_buffer_size();
    test_bad_input_is_ignored();
    test_scope_times_the_body();
    if (failures == 0) std::printf("callback_load_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}

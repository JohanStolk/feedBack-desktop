// Offline render tool for DropTuner: raw float32 mono in → raw float32 mono out.
//   drop_tuner_render <in.f32> <out.f32> <sampleRate> <semitones> [blockSize] [schedule.txt]
// Used to score the C++ port against the Python prototype on a recorded take.
// schedule.txt ("<seconds> <semitones>" per line, sorted) switches the amount
// over time like magical tuning does per note; it is applied at block starts.
#include "DropTuner.h"

#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 5)
    {
        std::fprintf(stderr, "usage: %s in.f32 out.f32 sampleRate semitones [blockSize]\n", argv[0]);
        return 2;
    }
    const double sr = std::atof(argv[3]);
    const float semis = (float) std::atof(argv[4]);
    const int block = argc > 5 ? std::atoi(argv[5]) : 256;
    std::vector<std::pair<double, float>> schedule;
    if (argc > 6)
    {
        std::FILE* sf = std::fopen(argv[6], "r");
        if (!sf) { std::perror(argv[6]); return 1; }
        double t; float v;
        while (std::fscanf(sf, "%lf %f", &t, &v) == 2) schedule.emplace_back(t, v);
        std::fclose(sf);
    }

    std::FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::perror(argv[1]); return 1; }
    std::vector<float> x;
    float buf[4096];
    size_t n;
    while ((n = std::fread(buf, sizeof(float), 4096, f)) > 0) x.insert(x.end(), buf, buf + n);
    std::fclose(f);

    slopsmith::DropTuner dt;
    dt.prepare(sr);
    dt.setSemitones(semis);
    size_t next = 0;
    std::vector<float> delays;
    delays.reserve(x.size() / (size_t) block + 1);
    for (size_t i = 0; i < x.size(); i += (size_t) block)
    {
        const int len = (int) std::min((size_t) block, x.size() - i);
        while (next < schedule.size() && schedule[next].first * sr <= (double) i)
            dt.setSemitones(schedule[next++].second);
        dt.process(x.data() + i, len);
        delays.push_back((float) dt.getDelaySamples());
    }
    std::FILE* o = std::fopen(argv[2], "wb");
    if (!o) { std::perror(argv[2]); return 1; }
    std::fwrite(x.data(), sizeof(float), x.size(), o);
    std::fclose(o);
    double sum = 0.0, mx = 0.0;
    for (float d : delays) { sum += d; if (d > mx) mx = d; }
    std::printf("rendered %zu samples; delay mean %.1f ms, max %.1f ms\n", x.size(),
                sum / delays.size() / sr * 1000.0, mx / sr * 1000.0);
    return 0;
}

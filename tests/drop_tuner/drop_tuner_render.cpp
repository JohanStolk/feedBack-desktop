// Offline render tool for DropTuner: raw float32 mono in → raw float32 mono out.
//   drop_tuner_render <in.f32> <out.f32> <sampleRate> <semitones> [blockSize]
// Used to score the C++ port against the Python prototype on a recorded take.
#include "DropTuner.h"

#include <cstdio>
#include <cstdlib>
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
    std::vector<float> delays;
    delays.reserve(x.size() / (size_t) block + 1);
    for (size_t i = 0; i < x.size(); i += (size_t) block)
    {
        const int len = (int) std::min((size_t) block, x.size() - i);
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

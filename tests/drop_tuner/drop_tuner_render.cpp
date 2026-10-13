// Offline render tool for DropTuner: raw float32 mono in → raw float32 mono out.
//   drop_tuner_render <in.f32> <out.f32> <sampleRate> <semitones> [blockSize] [shifts.txt|-] [notes.txt]
// Used to score the C++ port against the Python prototype on a recorded take.
// shifts.txt ("<seconds> <semitones>" per line) and notes.txt ("<seconds> <hz>":
// each note's expected input pitch) are loaded as a chart, like the app does
// per song ("match each song" + priming); the song clock is the sample clock,
// pushed once per block like the renderer's per-frame push. With DT_DELAY_LOG
// set to a path, the head delay of every sample is written there (float32).
#include "DropTuner.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static bool loadPairs(const char* path, std::vector<double>& t, std::vector<float>& v)
{
    std::FILE* f = std::fopen(path, "r");
    if (!f) { std::perror(path); return false; }
    double a; float b;
    while (std::fscanf(f, "%lf %f", &a, &b) == 2) { t.push_back(a); v.push_back(b); }
    std::fclose(f);
    return true;
}

int main(int argc, char** argv)
{
    if (argc < 5)
    {
        std::fprintf(stderr, "usage: %s in.f32 out.f32 sampleRate semitones [blockSize] [shifts.txt|-] [notes.txt]\n", argv[0]);
        return 2;
    }
    const double sr = std::atof(argv[3]);
    const float semis = (float) std::atof(argv[4]);
    const int block = argc > 5 ? std::atoi(argv[5]) : 256;
    std::vector<double> shiftT, noteT;
    std::vector<float> shiftV, noteHz;
    if (argc > 6 && std::strcmp(argv[6], "-") != 0 && ! loadPairs(argv[6], shiftT, shiftV)) return 1;
    if (argc > 7 && ! loadPairs(argv[7], noteT, noteHz)) return 1;
    const bool useChart = ! shiftT.empty() || ! noteT.empty();

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
    if (useChart) dt.setChart(shiftT, shiftV, noteT, noteHz);
    const char* logPath = std::getenv("DT_DELAY_LOG");
    std::vector<float> perSample;
    if (logPath) perSample.reserve(x.size());
    std::vector<float> delays;
    delays.reserve(x.size() / (size_t) block + 1);
    for (size_t i = 0; i < x.size(); i += (size_t) block)
    {
        const int len = (int) std::min((size_t) block, x.size() - i);
        if (useChart) dt.setClock((double) i / sr, true);
        if (logPath)
            for (int k = 0; k < len; ++k)
            {
                dt.process(x.data() + i + (size_t) k, 1);   // same result: block-size independent
                perSample.push_back((float) dt.getDelaySamples());
            }
        else
            dt.process(x.data() + i, len);
        delays.push_back((float) dt.getDelaySamples());
    }
    if (logPath)
    {
        std::FILE* lf = std::fopen(logPath, "wb");
        if (!lf) { std::perror(logPath); return 1; }
        std::fwrite(perSample.data(), sizeof(float), perSample.size(), lf);
        std::fclose(lf);
    }
    std::FILE* o = std::fopen(argv[2], "wb");
    if (!o) { std::perror(argv[2]); return 1; }
    std::fwrite(x.data(), sizeof(float), x.size(), o);
    std::fclose(o);
    double sum = 0.0, mx = 0.0;
    for (float d : delays) { sum += d; if (d > mx) mx = d; }
    std::printf("rendered %zu samples; delay mean %.1f ms, max %.1f ms; onsets %u\n", x.size(),
                sum / delays.size() / sr * 1000.0, mx / sr * 1000.0, dt.getOnsetCount());
    return 0;
}

// Offline checks for the native pipeline (not shipped):
//   1. HtDemucs C++ STFT/iSTFT + ONNX Runtime matches the NumPy/CPU reference on a real music window.
//   2. Spatial invariants: HRTF left/right, head rotation keeps sources room-fixed, upmix centre extraction,
//      room RT60.
//   3. StreamingSeparator produces continuous output with the expected channel layout.
// Usage: spatiall_selftest <model.onnx> <dir with mix.f32 + stems.f32 from make_selftest_data.py> [cpu|dml|auto]

#include "sep/HtDemucs.h"
#include "sep/StreamingSeparator.h"
#include "spatial/Binaural.h"
#include "spatial/Room.h"
#include "spatial/Rotation.h"
#include "spatial/Upmix.h"
#include "util/Log.h"

#include <windows.h>
#include <objbase.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

static std::vector<float> readF32(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    f.seekg(0, std::ios::end);
    std::vector<float> v(static_cast<size_t>(f.tellg()) / sizeof(float));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(v.size() * sizeof(float)));
    return v;
}

static int fail(const char* msg) {
    std::printf("FAIL: %s\n", msg);
    return 1;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return fail("usage: spatiall_selftest <model.onnx> <datadir> [cpu|dml|auto]");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    logx::init();
    const std::wstring prov = argc > 3 ? argv[3] : L"auto";
    const auto provider = prov == L"cpu" ? HtDemucs::Provider::Cpu
                          : prov == L"dml" ? HtDemucs::Provider::DirectML
                                           : HtDemucs::Provider::Auto;

    // 1) Window separation parity.
    HtDemucs model;
    std::string err;
    auto t0 = std::chrono::steady_clock::now();
    if (!model.load(argv[1], provider, &err)) return fail(err.c_str());
    std::printf("loaded in %.1f s, provider %s, window %d, sources %d\n",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
                model.providerName().c_str(), model.segment(), model.numSources());
    const std::filesystem::path dir(argv[2]);
    const auto mix = readF32(dir / "mix.f32");
    const auto want = readF32(dir / "stems.f32");
    const size_t N = static_cast<size_t>(model.segment());
    if (mix.size() != 2 * N || want.size() != 2 * N * model.numSources()) return fail("test data does not match model");
    std::vector<float> got(want.size());
    model.run(mix.data(), got.data());  // warm-up
    t0 = std::chrono::steady_clock::now();
    if (!model.run(mix.data(), got.data())) return fail("inference failed");
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    for (int s = 0; s < model.numSources(); ++s) {
        double num = 0, den = 0, maxd = 0;
        for (size_t i = 0; i < 2 * N; ++i) {
            const double w = want[s * 2 * N + i], g = got[s * 2 * N + i];
            num += w * w;
            den += (w - g) * (w - g);
            maxd = std::max(maxd, std::fabs(w - g));
        }
        const double snr = 10 * std::log10(num / (den + 1e-20));
        std::printf("  %-8s parity SNR %6.1f dB, max abs diff %.2e\n", model.sources()[s].c_str(), snr, maxd);
        if (snr < 60) return fail("C++ separation does not match the reference");
    }
    std::printf("separate window: %.0f ms\n", ms);

    // 2) Spatial invariants on white noise.
    std::vector<float> noise(512 * 64);
    uint32_t seed = 7;
    for (auto& v : noise) {
        seed = seed * 1664525u + 1013904223u;
        v = (static_cast<float>(seed >> 8) / 16777216.0f - 0.5f) * 0.2f;
    }
    // ILD (right minus left, dB) of noise from `dir`, as heard by a head at `rot`.
    auto ild = [&](float azimuth, const HeadRotation& rot) {
        Binaural bin;
        std::string e;
        if (!bin.init(44100, 512, 1, &e)) return 1e9;
        std::vector<float> out(1024);
        double eL = 0, eR = 0;
        for (int b = 0; b < 64; ++b) {
            std::fill(out.begin(), out.end(), 0.0f);
            const float* ins[1] = {&noise[static_cast<size_t>(b) * 512]};
            SourcePose pose{rot.toListener(directionFromAngles(azimuth, 0.0f)), 1.0f};
            bin.process(ins, &pose, out.data());
            if (b < 2) continue;  // let the gain ramp settle
            for (int i = 0; i < 512; ++i) {
                eL += double(out[2 * i]) * out[2 * i];
                eR += double(out[2 * i + 1]) * out[2 * i + 1];
            }
        }
        return 10 * std::log10(eR / (eL + 1e-20));
    };
    const HeadRotation still;
    const double right = ild(90.0f, still), ahead = ild(0.0f, still);
    const double turned = ild(30.0f, HeadRotation::fromYawPitchRoll(30.0f, 0.0f, 0.0f));
    std::printf("binaural ILD: +90 deg %.1f dB, ahead %.1f dB, +30 deg with head turned +30 %.1f dB\n", right, ahead,
                turned);
    if (right < 6.0) return fail("HRTF panning is wrong (right ear not louder)");
    if (std::fabs(turned - ahead) > 1.0) return fail("head rotation does not keep sources fixed in the room");

    // Upmix: identical L/R must land in the centre source; one-sided content must stay in its residual.
    auto upmixShares = [&](float gainL, float gainR, double& centre, double& left, double& right) {
        StereoUpmix up;
        up.init();
        std::vector<float> c(512), l(512), r(512);
        centre = left = right = 0;
        for (int b = 0; b < 64; ++b) {
            std::vector<float> inL(512), inR(512);
            for (int i = 0; i < 512; ++i) {
                inL[i] = noise[static_cast<size_t>(b) * 512 + i] * gainL;
                inR[i] = noise[static_cast<size_t>(b) * 512 + i] * gainR;
            }
            up.process(inL.data(), inR.data(), c.data(), l.data(), r.data());
            if (b < 8) continue;  // smoothing settles
            for (int i = 0; i < 512; ++i) {
                centre += double(c[i]) * c[i];
                left += double(l[i]) * l[i];
                right += double(r[i]) * r[i];
            }
        }
        const double total = centre + left + right + 1e-20;
        centre /= total;
        left /= total;
        right /= total;
    };
    double c1, l1, r1, c2, l2, r2;
    upmixShares(1.0f, 1.0f, c1, l1, r1);
    upmixShares(1.0f, 0.0f, c2, l2, r2);
    std::printf("upmix energy share: centred -> C %.2f L %.2f R %.2f; left-only -> C %.2f L %.2f R %.2f\n", c1, l1,
                r1, c2, l2, r2);
    if (c1 < 0.95) return fail("centred content is not extracted to the centre source");
    if (l2 < 0.95) return fail("one-sided content leaks into the centre");

    // Room: the tail must decay at roughly the preset's RT60 (Studio 0.35 s).
    {
        Room room;
        if (!room.init(44100, 512, &err)) return fail(err.c_str());
        room.setPreset(Room::Preset::Studio);
        std::vector<float> send(512, 0.0f), out(1024);
        std::vector<double> blockEnergy;
        for (int b = 0; b < 120; ++b) {
            std::fill(send.begin(), send.end(), 0.0f);
            if (b == 0) send[0] = 1.0f;  // impulse
            std::fill(out.begin(), out.end(), 0.0f);
            room.process(send.data(), still, 1.0f, out.data());
            double e = 0;
            for (float v : out) e += double(v) * v;
            blockEnergy.push_back(e);
        }
        // Schroeder backward integration; RT60 from the -5..-25 dB slope.
        std::vector<double> edc(blockEnergy.size());
        double acc = 0;
        for (size_t i = blockEnergy.size(); i-- > 0;) edc[i] = (acc += blockEnergy[i]);
        double t5 = -1, t25 = -1;
        for (size_t i = 0; i < edc.size(); ++i) {
            const double db = 10 * std::log10(edc[i] / edc[0]);
            if (t5 < 0 && db <= -5) t5 = i * 512.0 / 44100;
            if (t25 < 0 && db <= -25) t25 = i * 512.0 / 44100;
        }
        const double rt60 = t25 > t5 && t5 >= 0 ? (t25 - t5) * 3.0 : -1;
        std::printf("room: Studio RT60 measured %.2f s (preset 0.35 s)\n", rt60);
        if (rt60 < 0.2 || rt60 > 0.55) return fail("room decay does not match its preset");
    }

    // 3) Streaming continuity: feed 3 s of the mix in odd-sized chunks.
    StreamingSeparator sep;
    sep.init(&model, 11025, 13230);
    std::vector<float> stream;
    std::vector<float> chunk;
    size_t pos = 0, steps = 0;
    double worst = 0;
    while (pos + 441 <= N && pos < 3 * 44100) {
        chunk.resize(441 * 2);
        for (size_t i = 0; i < 441; ++i) {
            chunk[2 * i] = mix[pos + i];
            chunk[2 * i + 1] = mix[N + pos + i];
        }
        sep.push(chunk.data(), 441);
        pos += 441;
        while (sep.ready()) {
            if (!sep.step(stream)) return fail("streaming step failed");
            worst = std::max(worst, sep.lastInferenceMs());
            ++steps;
        }
    }
    const size_t frames = stream.size() / sep.channels();
    std::printf("streaming: %zu steps, %zu frames out (%d ch), worst inference %.0f ms (hop %d ms)\n", steps, frames,
                sep.channels(), worst, sep.hop() * 1000 / 44100);
    if (frames != steps * static_cast<size_t>(sep.hop())) return fail("streaming output length mismatch");
    std::printf("PASS\n");
    return 0;
}

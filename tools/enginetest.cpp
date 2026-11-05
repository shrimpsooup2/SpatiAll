// Headless end-to-end check on real devices without making sound you hear (use unconnected outputs).
//
//   spatiall_enginetest pipeline <sink-substr> <out-substr> <mix.f32> <model.onnx> [stems|speakers|adaptive]
//                      [lookahead] [hop]
//       Plays music into <sink>, runs the Engine (SelectedApps routing of this pid) rendering to <out>,
//       records both via loopback, reports engine status, output level and measured latency (bypass).

#include "audio/Routing.h"
#include "audio/Wasapi.h"
#include "engine/Engine.h"
#include "util/Log.h"

#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace {

std::vector<float> readF32(const std::wstring& p) {
    std::ifstream f(p, std::ios::binary);
    f.seekg(0, std::ios::end);
    std::vector<float> v(static_cast<size_t>(f.tellg()) / sizeof(float));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(v.size() * sizeof(float)));
    return v;
}

std::wstring findDevice(const std::wstring& substr) {
    for (const auto& d : audio::listRenderDevices())
        if (logx::widen(d.name).find(substr) != std::wstring::npos) return d.id;
    return {};
}

double nowSec() {
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

// Planar 44.1 kHz stereo music, played with silence gaps: [gap][music] repeated, at the device rate (nearest).
struct Player {
    std::vector<float> music;  // planar [2][n]
    size_t n = 0;
    double pos = 0.0;
    double step = 1.0;
    double gapSec = 0.0, musicSec = 0.0;
    std::atomic<double> startTime{0.0};
    void fill(float* out, size_t frames) {
        if (startTime == 0.0) startTime = nowSec();
        const double cycle = gapSec + musicSec;
        for (size_t i = 0; i < frames; ++i) {
            const double t = pos / 44100.0;
            const double inCycle = std::fmod(t, cycle);
            float l = 0.0f, r = 0.0f;
            if (inCycle >= gapSec) {
                const size_t k = static_cast<size_t>((inCycle - gapSec) * 44100.0) % n;
                l = music[k];
                r = music[n + k];
            }
            out[2 * i] = l;
            out[2 * i + 1] = r;
            pos += step;
        }
    }
};

// Loopback recorder with arrival timestamps, to find signal onsets.
struct Recorder {
    audio::CaptureStream cap;
    std::mutex mu;
    std::vector<float> mono;
    double firstTime = 0.0;
    uint32_t rate = 0;
    bool start(const audio::CaptureStream::Config& cc) {
        // The stream's format is known before its first callback, so read the rate there.
        return cap.start(cc, [this](const float* s, size_t frames) {
            std::lock_guard lk(mu);
            if (mono.empty()) {
                rate = cap.sampleRate();
                firstTime = nowSec() - static_cast<double>(frames) / rate;
            }
            for (size_t i = 0; i < frames; ++i) mono.push_back(0.5f * (s[2 * i] + s[2 * i + 1]));
        });
    }
    double rms(double fromSec, double toSec) {
        std::lock_guard lk(mu);
        const size_t a = static_cast<size_t>(fromSec * rate), b = std::min(mono.size(), static_cast<size_t>(toSec * rate));
        double acc = 0;
        for (size_t i = a; i < b; ++i) acc += double(mono[i]) * mono[i];
        return b > a ? std::sqrt(acc / double(b - a)) : 0.0;
    }
    // Wall-clock times where the 10 ms RMS envelope crosses from silence to above `thresh`.
    std::vector<double> onsets(float thresh) {
        std::lock_guard lk(mu);
        std::vector<double> out;
        const size_t win = rate / 100;
        bool quiet = false;
        int quietRun = 0;
        for (size_t i = 0; i + win <= mono.size(); i += win) {
            double acc = 0;
            for (size_t k = i; k < i + win; ++k) acc += double(mono[k]) * mono[k];
            const double r = std::sqrt(acc / win);
            if (r < thresh * 0.1) {
                if (++quietRun >= 50) quiet = true;  // >= 0.5 s of silence arms the detector
            } else {
                quietRun = 0;
                if (quiet && r > thresh) {
                    out.push_back(firstTime + static_cast<double>(i) / rate);
                    quiet = false;
                }
            }
        }
        return out;
    }
};

int pipelineTest(const std::wstring& sinkSub, const std::wstring& outSub, const std::wstring& mixPath,
                 const std::wstring& model, engine::Mode mode, float lookahead, float hop) {
    const std::wstring sink = findDevice(sinkSub), out = findDevice(outSub);
    if (sink.empty() || out.empty() || sink == out) return std::printf("devices not found / identical\n"), 1;
    std::printf("sink: %s\nout:  %s\n", audio::deviceName(sink).c_str(), audio::deviceName(out).c_str());

    Player player;
    player.music = readF32(mixPath);
    player.n = player.music.size() / 2;
    player.gapSec = 1.5;
    player.musicSec = 2.5;

    engine::Engine eng;
    engine::Config cfg;
    cfg.headphoneDeviceId = out;
    cfg.sinkDeviceId = sink;
    cfg.capture = engine::CaptureMode::SelectedApps;
    cfg.appPids = {GetCurrentProcessId()};
    cfg.mode = mode;
    cfg.modelPath = model;
    if (lookahead > 0.0f) cfg.lookaheadSec = lookahead;
    if (hop > 0.0f) cfg.hopSec = hop;
    double t0 = nowSec();
    if (!eng.start(cfg)) return std::printf("engine start failed: %s\n", eng.status().error.c_str()), 1;
    std::printf("engine started in %.1f s\n", nowSec() - t0);

    Recorder inRec, outRec;
    audio::CaptureStream::Config ci;
    ci.deviceId = sink;
    audio::CaptureStream::Config co;
    co.deviceId = out;
    if (!inRec.start(ci) || !outRec.start(co)) return std::printf("loopback recorders failed\n"), 1;

    audio::RenderStream rs;
    if (!rs.start(sink, [&](float* o, size_t f) { player.fill(o, f); })) return std::printf("play failed\n"), 1;
    player.step = 44100.0 / rs.sampleRate();

    // Phase 1: spatial on. Phase 2: bypass (dry, same latency) for an exact latency measurement.
    // Status is polled throughout so underruns and adaptive mode/hop changes are reported with their time.
    uint64_t seenUnderruns = 0;
    engine::Mode seenMode = eng.status().activeMode;
    double seenHop = 0.0;
    auto watch = [&](int ms) {
        for (int waited = 0; waited < ms; waited += 100) {
            Sleep(100);
            const auto s = eng.status();
            if (s.underruns != seenUnderruns) {
                std::printf("  underrun at %.1f s (total %llu)\n", nowSec() - t0,
                            static_cast<unsigned long long>(s.underruns));
                seenUnderruns = s.underruns;
            }
            if (s.activeMode != seenMode) {
                std::printf("  %.1f s: now %s%s%s\n", nowSec() - t0,
                            s.activeMode == engine::Mode::Stems ? "stems" : "virtual speakers",
                            s.adaptiveReason.empty() ? "" : " - ", s.adaptiveReason.c_str());
                seenMode = s.activeMode;
            }
            if (s.activeMode == engine::Mode::Stems && std::fabs(s.hopSec - seenHop) > 0.02) {
                std::printf("  %.1f s: hop %.2f s (inference %.0f ms)\n", nowSec() - t0, s.hopSec, s.inferenceMs);
                seenHop = s.hopSec;
            }
        }
    };
    if (mode == engine::Mode::Adaptive) {
        // A video starts playing for 5 s, then stops.
        watch(6000);
        std::printf("  %.1f s: video ON\n", nowSec() - t0);
        eng.setLowLatencyHint(true, "video: YouTube in chrome.exe");
        watch(5000);
        std::printf("  %.1f s: video OFF\n", nowSec() - t0);
        eng.setLowLatencyHint(false);
        watch(8000);
    } else {
        watch(14000);
    }
    const auto st = eng.status();
    eng.setEnabled(false);
    const double bypassFrom = nowSec();
    watch(9000);
    rs.stop();
    inRec.cap.stop();
    outRec.cap.stop();
    const auto st2 = eng.status();
    eng.stop();

    std::printf("status: stage=%s provider=%s latency(est)=%.0f ms inference=%.0f ms gpu=%.0f%% underruns=%llu "
                "render load avg %.0f%% peak %.0f%% capture=%u Hz render=%u Hz\n",
                st.stage.c_str(), st.provider.c_str(), st.latencySec * 1000, st.inferenceMs, st.gpuLoad * 100,
                static_cast<unsigned long long>(st2.underruns), st.renderLoad * 100, st.renderLoadPeak * 100,
                st.captureRate, st.renderRate);
    for (size_t s = 0; s < st.stemNames.size(); ++s)
        std::printf("  stem %-7s level %.4f\n", st.stemNames[s].c_str(), st.stemLevels[s]);
    // Equal 8 s windows (two full play/gap cycles); the spatial one starts after level matching settles.
    const double b = bypassFrom - outRec.firstTime;
    const double spatialRms = outRec.rms(5.9, 13.9), bypassRms = outRec.rms(b + 0.9, b + 8.9);
    std::printf("input RMS %.4f, output RMS spatial %.4f vs bypass %.4f (%+.1f dB)\n", inRec.rms(1, 9), spatialRms,
                bypassRms, 20 * std::log10(spatialRms / (bypassRms + 1e-12)));

    const auto ins = inRec.onsets(0.01f), outs = outRec.onsets(0.01f);
    std::vector<double> lat;
    for (double o : outs) {
        double best = -1;
        for (double i : ins)
            if (i < o && o - i < 3.0) best = o - i;  // latest input onset before this output onset
        if (best <= 0) continue;
        // The timeline shows latency per phase (e.g. low latency while a fullscreen app is in front).
        std::printf("  onset at %.1f s: latency %.0f ms\n", o - t0, best * 1000.0);
        if (o >= bypassFrom + 1.0) lat.push_back(best);
    }
    if (lat.empty()) {
        std::printf("latency: no onsets matched (in %zu, out %zu)\n", ins.size(), outs.size());
    } else {
        double sum = 0;
        for (double v : lat) sum += v;
        std::printf("measured latency (bypass, onset-to-onset): %.0f ms over %zu onsets\n", 1000 * sum / lat.size(),
                    lat.size());
    }
    const bool ok = outRec.rms(2, 9) > 0.005 && st2.error.empty();
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    logx::init();
    // Own journal: the app may be running, and its journal describes its live routing, not a crash.
    audio::RoutingJournal::instance().useFile(L"routing-journal-enginetest.txt");
    audio::RoutingJournal::instance().recoverFromCrash();
    const std::wstring cmd = argc > 1 ? argv[1] : L"";
    if (cmd == L"pipeline" && argc >= 6) {
        const std::wstring m = argc > 6 ? argv[6] : L"stems";
        const engine::Mode mode = m == L"speakers" ? engine::Mode::VirtualSpeakers
                                  : m == L"adaptive" ? engine::Mode::Adaptive
                                                     : engine::Mode::Stems;
        return pipelineTest(argv[2], argv[3], argv[4], argv[5], mode, argc > 7 ? std::stof(argv[7]) : 0.0f,
                            argc > 8 ? std::stof(argv[8]) : 0.0f);
    }
    std::printf("usage: see header of tools/enginetest.cpp\n");
    return 2;
}

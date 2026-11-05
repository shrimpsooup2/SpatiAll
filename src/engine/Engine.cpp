#include "engine/Engine.h"

#include "audio/Routing.h"
#include "audio/Wasapi.h"
#include "dsp/Resampler.h"
#include "dsp/SpscRing.h"
#include "sep/HtDemucs.h"
#include "sep/StreamingSeparator.h"
#include "spatial/Binaural.h"
#include "spatial/Room.h"
#include "spatial/Rotation.h"
#include "spatial/Upmix.h"
#include "track/HeadTracker.h"
#include "util/Log.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <cmath>

namespace engine {
namespace {

constexpr int kBlock = 512;                   // spatializer frame size at 44.1 kHz
constexpr size_t kInputRingFrames = 1 << 20;  // ~5 s even at 192 kHz
constexpr float kFadeSec = 0.15f;             // spatial on/off crossfade
constexpr float kModeFadeSec = 0.04f;         // fade out/in around a live stems <-> speakers switch

// Adaptive hop: keep inference below this share of the hop, so chunks arrive with room to spare.
constexpr double kTargetGpuLoad = 0.6;
// Adaptive mode: give up on separation when even the longest hop leaves the GPU this busy...
constexpr double kOverloadGpuLoad = 0.9;
constexpr int kOverloadSteps = 3;
constexpr ULONGLONG kGpuRetryMs = 30000;       // ...and try again after this long.
constexpr ULONGLONG kHintHoldMs = 3000;  // stay low-latency briefly after the game/video goes away

float dbToLin(float db) { return std::pow(10.0f, db / 20.0f); }

// Direct-sound distance law referenced to 1 m, capped so very close sources don't explode. The room send stays
// constant, so the direct-to-room ratio falls with distance like in a real room.
float distanceGain(float d) { return std::min(1.0f / std::max(d, 0.3f), 2.0f); }

Room::Preset toRoom(RoomPreset p) {
    switch (p) {
        case RoomPreset::SmallRoom: return Room::Preset::SmallRoom;
        case RoomPreset::Studio: return Room::Preset::Studio;
        case RoomPreset::LivingRoom: return Room::Preset::LivingRoom;
        case RoomPreset::Hall: return Room::Preset::Hall;
        case RoomPreset::Off: break;
    }
    return Room::Preset::Off;
}

// Transparent below 0.9, smoothly saturating above: keeps summed stems from hard-clipping.
inline float softLimit(float x) {
    const float a = std::fabs(x);
    if (a <= 0.9f) return x;
    return std::copysign(0.9f + 0.1f * std::tanh((a - 0.9f) / 0.1f), x);
}

struct ComInit {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComInit() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

}  // namespace

struct Engine::Impl {
    Config cfg;
    bool stemsCapable = false;  // a separation model is loaded (Stems / Adaptive)

    HtDemucs model;
    std::wstring loadedModel;
    StreamingSeparator sep;
    int numStems = 0;
    int maxGroups = 1;  // render-side allocation: stems, or the single speaker pair
    int sceneCh = 2;    // [dry L/R | stems L/R...]; speakers frames carry silent stem channels

    audio::CaptureStream capture;
    SpscRing inRing;
    HANDLE inputEvent = nullptr;
    audio::RenderStream render;
    audio::RenderStream sinkKeepAlive;  // plays silence into the sink so its loopback keeps a steady clock
    SpscRing sceneRing;
    Resampler inRs;
    Binaural binaural;

    std::thread worker;
    std::atomic<bool> stopWorker{false};
    std::atomic<bool> running{false};
    std::atomic<bool> faulted{false};

    // Live stems <-> speakers switching. The worker requests, the render thread fades out, drops the queued
    // audio of the old kind and acknowledges; only then does the worker produce the new kind.
    std::atomic<uint32_t> switchReq{0}, switchAck{0};
    std::atomic<bool> switchToStems{false};
    std::atomic<bool> activeStems{false};  // what the render thread is rendering (for status)
    std::atomic<bool> lowLatencyHint{false};

    // Worker-thread state.
    bool workerStems = false;
    double hopMs = 0.0;
    ULONGLONG lastHintMs = 0;
    ULONGLONG gpuFallbackUntil = 0;
    int overloadSteps = 0;
    uint64_t loggedUnderruns = 0;
    ULONGLONG lastUnderrunLog = 0;

    // Render-thread state.
    Resampler outRs;
    bool outRsReady = false;
    double outRatio = 1.0;
    double driftAdj = 1.0;
    std::vector<float> block, dry, dryDelayed, spatial, outFifo, send;
    std::vector<std::vector<float>> stemIn;   // [2 * groups] deinterleaved stereo per stem (or the mix)
    std::vector<std::vector<float>> mono;     // [3 * groups] centre / left / right per group
    std::vector<const float*> monoPtrs;
    std::vector<SourcePose> poses;
    std::vector<float> sendGains;             // per group: room send (distance-independent)
    std::vector<StereoUpmix> upmix;           // per group
    Room room;
    Room::Preset roomPreset = Room::Preset::Off;
    bool renderStems = false;
    float modeGain = 1.0f;
    size_t outRead = 0;
    bool primed = false;
    size_t padSilence = 0;
    size_t targetMin = 0, periodLen = 0;  // targetMin: adaptive playback cushion (frames)
    size_t periodMin = SIZE_MAX, periodCount = 0;
    float enableGain = 1.0f;
    float levelMatch = 1.0f;             // spatial -> plain-stereo loudness correction
    double spatialEnergy = 0.0, dryEnergy = 0.0;
    SceneParams renderScene;
    uint64_t renderSceneVer = 0;
    track::Orientation head;             // smoothed orientation applied to the scene

    // Shared.
    std::atomic<bool> enabled{true};
    mutable std::mutex sceneMu;
    SceneParams scene;
    std::atomic<uint64_t> sceneVer{1};
    mutable std::mutex statusMu;
    std::string error, stage, adaptiveReason;
    std::string hintReason, lastHintReason;  // why low latency was requested (guarded by statusMu)
    std::vector<std::string> stemNames;
    std::string provider;
    std::atomic<double> inferenceMs{0.0}, avgFill{0.0};
    std::atomic<float> inputPeak{0.0f};
    std::array<std::atomic<float>, kMaxStems> levels{};
    std::atomic<uint64_t> underruns{0};
    std::atomic<double> renderLoad{0.0}, renderLoadPeak{0.0};
    std::atomic<double> inferencePeakMs{0.0}, cushionMs{0.0}, currentHopSec{0.0};
    uint32_t captureRate = 0;
    std::atomic<uint32_t> renderRate{0};
    std::atomic<const track::HeadTracker*> tracker{nullptr};
    std::atomic<float> headYaw{0.0f}, headPitch{0.0f}, headRoll{0.0f};
    std::atomic<bool> headTracked{false};

    void setStage(const std::string& s) {
        std::lock_guard lk(statusMu);
        stage = s;
    }
    void setError(const std::string& e) {
        logx::error(e);
        std::lock_guard lk(statusMu);
        error = e;
    }
    void setReason(const std::string& r) {
        std::lock_guard lk(statusMu);
        if (adaptiveReason != r) logx::info("adaptive: " + (r.empty() ? std::string("stems") : r));
        adaptiveReason = r;
    }

    int groups() const { return renderStems ? numStems : 1; }
    size_t cushionBase() const { return static_cast<size_t>((renderStems ? 0.08 : 0.02) * kInternalRate); }
    size_t cushionMax() const { return static_cast<size_t>((renderStems ? 0.6 : 0.15) * kInternalRate); }

    void workerMain();
    bool decideStems(ULONGLONG now);
    void adaptHop();
    void renderCallback(float* out, size_t frames);
    void produceBlock();
    void handleModeSwitch();
    void updateHead();
    void buildPoses(const HeadRotation& rot);
};

// ------------------------------------------------------------------------------------------------
// Worker: sink capture -> 44.1 kHz -> separator (or straight through) -> scene ring.

// Which kind of audio the worker should produce now. Fixed modes are fixed; Adaptive trades separation for
// latency while the app says latency matters (game, video) and for headroom while the GPU can't keep up.
bool Engine::Impl::decideStems(ULONGLONG now) {
    if (!stemsCapable) return false;
    if (cfg.mode != Mode::Adaptive) return true;
    if (lowLatencyHint) {
        lastHintMs = now;
        std::lock_guard lk(statusMu);
        lastHintReason = hintReason;
    }
    if (lastHintMs != 0 && now - lastHintMs < kHintHoldMs) {
        std::string reason;
        {
            std::lock_guard lk(statusMu);
            reason = lastHintReason;
        }
        setReason(reason.empty() ? "low latency" : reason);
        return false;
    }
    if (cfg.adaptiveGpuFallback && now < gpuFallbackUntil) {
        setReason("GPU busy");
        return false;
    }
    setReason("");
    return true;
}

// Called after each separator run: grow the hop at once when inference eats into it, shrink slowly back to
// the configured minimum when there is headroom. Larger hops cost latency but run the model less often.
void Engine::Impl::adaptHop() {
    const double minMs = cfg.hopSec * 1000.0;
    const double maxMs = std::max(minMs, std::min<double>(cfg.maxHopSec * 1000.0, 1000.0 * sep.maxHop() / kInternalRate));
    const double want = std::clamp(inferencePeakMs.load() / kTargetGpuLoad, minMs, maxMs);
    hopMs = want > hopMs ? want : std::max(want, hopMs * 0.97);
    sep.setHop(static_cast<int>(hopMs / 1000.0 * kInternalRate));
    currentHopSec = static_cast<double>(sep.hop()) / kInternalRate;

    if (cfg.mode != Mode::Adaptive || !cfg.adaptiveGpuFallback) return;
    const bool atMax = hopMs >= maxMs - 1.0;
    overloadSteps = atMax && inferencePeakMs.load() > kOverloadGpuLoad * maxMs ? overloadSteps + 1 : 0;
    if (overloadSteps >= kOverloadSteps) {
        logx::warn("GPU can't keep up (separator " + std::to_string(static_cast<int>(inferencePeakMs.load())) +
                   " ms per " + std::to_string(static_cast<int>(maxMs)) + " ms hop); virtual speakers for " +
                   std::to_string(kGpuRetryMs / 1000) + " s");
        gpuFallbackUntil = GetTickCount64() + kGpuRetryMs;
        overloadSteps = 0;
        inferencePeakMs = 0.0;  // measured again when separation resumes
    }
}

void Engine::Impl::workerMain() {
    ComInit com;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    std::vector<float> inBuf, rsOut, sepOut, wide;
    while (!stopWorker) {
        if (capture.failed()) {
            setError("Audio capture stopped: " + capture.error());
            faulted = true;
        }
        if (render.failed()) {
            setError("Headphone output stopped: " + render.error());
            faulted = true;
        }
        if (faulted) {
            // Give the user their audio back immediately; the UI offers a restart.
            audio::RoutingJournal::instance().restoreAll();
            running = false;
            setStage("Stopped (device error)");
            return;
        }
        if (const uint64_t u = underruns.load(); u != loggedUnderruns && GetTickCount64() - lastUnderrunLog > 2000) {
            logx::warn("audio underrun (" + std::to_string(u) + " so far); playback cushion now " +
                       std::to_string(static_cast<int>(cushionMs.load())) + " ms, separator peak " +
                       std::to_string(static_cast<int>(inferencePeakMs.load())) + " ms");
            loggedUnderruns = u;
            lastUnderrunLog = GetTickCount64();
        }

        // Mode decisions happen here so a switch takes effect even while nothing is playing. A request stays
        // pending until the render thread has faded out and dropped the old audio.
        if (switchReq.load() == switchAck.load()) {
            workerStems = switchToStems;
            const bool want = decideStems(GetTickCount64());
            if (want != workerStems) {
                switchToStems = want;
                switchReq++;
            }
        }

        WaitForSingleObject(inputEvent, 20);
        const size_t n = inRing.readable();
        if (n == 0) continue;
        inBuf.resize(n * 2);
        inRing.read(inBuf.data(), n);
        rsOut.clear();
        inRs.process(inBuf.data(), n, rsOut);
        const size_t frames = rsOut.size() / 2;
        if (frames == 0) continue;

        if (stemsCapable) sep.push(rsOut.data(), frames);
        // While the render thread fades out for a switch, produce nothing; the separator window still advances.
        if (switchReq.load() != switchAck.load()) {
            if (stemsCapable) sep.absorb();
            continue;
        }
        if (!workerStems) {
            if (stemsCapable) sep.absorb();  // keep real context so separation can take over at any time
            wide.assign(frames * static_cast<size_t>(sceneCh), 0.0f);
            for (size_t i = 0; i < frames; ++i) {
                wide[i * sceneCh] = rsOut[2 * i];
                wide[i * sceneCh + 1] = rsOut[2 * i + 1];
            }
            if (sceneRing.write(wide.data(), frames) < frames) underruns++;  // overflow counts as a glitch
            continue;
        }
        while (sep.ready() && !stopWorker) {
            sepOut.clear();
            if (!sep.step(sepOut)) logx::warn("separation failed for one hop; passing audio through");
            if (!sep.lastSkipped()) {
                inferenceMs = sep.lastInferenceMs();
                inferencePeakMs = std::max(sep.lastInferenceMs(), inferencePeakMs.load() * 0.97);  // ~7 s memory
                adaptHop();
            }
            const size_t outFrames = sepOut.size() / static_cast<size_t>(sceneCh);
            if (sceneRing.write(sepOut.data(), outFrames) < outFrames) underruns++;
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Render: scene ring -> binaural -> drift-corrected resample -> device.

// Smooths the tracker orientation per block; eases back to front when tracking is lost.
void Engine::Impl::updateHead() {
    const track::HeadTracker* t = tracker.load();
    track::Orientation o;
    const bool live = t != nullptr && t->current(o);
    if (!live) o = {};
    const float a = live ? 0.6f : 0.05f;  // follow fast; recover gently so a dropout doesn't swing the scene
    head.yaw = std::remainder(head.yaw + a * std::remainder(o.yaw - head.yaw, 360.0f), 360.0f);
    head.pitch += a * (o.pitch - head.pitch);
    head.roll = std::remainder(head.roll + a * std::remainder(o.roll - head.roll, 360.0f), 360.0f);
    headTracked = live;
    headYaw = head.yaw;
    headPitch = head.pitch;
    headRoll = head.roll;
}

// Each group (stem, or the whole mix as the speaker pair) becomes centre / left / right sources at
// azimuth, azimuth - width/2, azimuth + width/2 in the room, seen through the rotated head.
void Engine::Impl::buildPoses(const HeadRotation& rot) {
    const SceneParams& sc = renderScene;
    const float master = dbToLin(sc.masterGainDb);
    auto place = [&](const StemParams& p, float gain, int g) {
        const float direct = gain * distanceGain(std::clamp(p.distance, 0.3f, 10.0f));
        const float az[3] = {p.azimuthDeg, p.azimuthDeg - 0.5f * p.widthDeg, p.azimuthDeg + 0.5f * p.widthDeg};
        for (int k = 0; k < 3; ++k) {
            const Vec3 world = directionFromAngles(az[k] + sc.sceneYawDeg, p.elevationDeg);
            poses[3 * g + k] = SourcePose{rot.toListener(world), direct};
        }
        sendGains[g] = gain;
    };
    // Groups the current mode doesn't use stay silent (the binaural renderer skips them).
    for (auto& p : poses) p.gain = 0.0f;
    std::fill(sendGains.begin(), sendGains.end(), 0.0f);
    if (!renderStems) {
        place(sc.speakers, sc.speakers.mute ? 0.0f : dbToLin(sc.speakers.gainDb) * master, 0);
        return;
    }
    bool anySolo = false;
    for (int s = 0; s < numStems; ++s) anySolo |= sc.stems[s].solo;
    for (int s = 0; s < numStems; ++s) {
        const StemParams& p = sc.stems[s];
        const bool audible = !p.mute && (!anySolo || p.solo);
        place(p, audible ? dbToLin(p.gainDb) * master : 0.0f, s);
    }
}

// Worker asked for the other kind of audio: once faded out, drop what is queued (it belongs to the old latency)
// and start over with a cushion for the new mode.
void Engine::Impl::handleModeSwitch() {
    const uint32_t req = switchReq.load();
    if (req == switchAck.load() || modeGain > 0.0f) return;
    sceneRing.read(nullptr, sceneRing.readable());
    renderStems = switchToStems;
    activeStems = renderStems;
    primed = false;
    padSilence = 0;
    targetMin = cushionBase() +
                (renderStems ? static_cast<size_t>(inferencePeakMs.load() / 1000.0 * kInternalRate) : 0);
    targetMin = std::min(targetMin, cushionMax());
    for (auto& u : upmix) u.reset();
    binaural.reset();
    room.reset();
    switchAck = req;
}

void Engine::Impl::produceBlock() {
    const bool switchPending = switchReq.load() != switchAck.load();
    handleModeSwitch();

    const size_t fill = sceneRing.readable();
    periodMin = std::min(periodMin, fill);
    avgFill = 0.98 * avgFill.load() + 0.02 * static_cast<double>(fill);

    bool haveAudio = false;
    if (!primed && fill >= static_cast<size_t>(kBlock)) {
        primed = true;
        padSilence = targetMin;  // start with a cushion so the first bursty refill doesn't underrun
    }
    if (primed && padSilence == 0) {
        if (fill < static_cast<size_t>(kBlock)) {
            primed = false;
            if (!switchPending) {
                underruns++;
                // Ran dry: the separator was later than the cushion allowed. Grow it (re-priming inserts it).
                targetMin = std::min(targetMin + static_cast<size_t>(0.06 * kInternalRate), cushionMax());
            }
        } else {
            const size_t hopFrames =
                renderStems ? static_cast<size_t>(currentHopSec.load() * kInternalRate) : 0;
            const size_t maxFill = targetMin + static_cast<size_t>(0.5 * kInternalRate) + hopFrames;
            if (fill > maxFill) sceneRing.read(nullptr, fill - targetMin);  // fell far behind: resync
            sceneRing.read(block.data(), kBlock);
            haveAudio = true;
        }
    }
    if (padSilence > 0) padSilence -= std::min<size_t>(padSilence, kBlock);

    // Drift control on the minimum fill per period (robust to the separator's bursty delivery).
    if (++periodCount * kBlock >= periodLen) {
        const double errSec = (static_cast<double>(periodMin) - static_cast<double>(targetMin)) / kInternalRate;
        driftAdj = std::clamp(1.0 - 0.05 * errSec, 0.996, 1.004);
        periodMin = SIZE_MAX;
        periodCount = 0;
        // The separator's chunks arrive up to one inference late, so the cushion must cover its recent worst
        // case. Follow increases at once; give back latency slowly after a slow spell (e.g. GPU contention).
        if (renderStems) {
            const size_t want = std::min(
                cushionMax(), cushionBase() + static_cast<size_t>(inferencePeakMs.load() / 1000.0 * kInternalRate));
            targetMin = want > targetMin ? want : targetMin - (targetMin - want) / 20;
        }
        cushionMs = 1000.0 * static_cast<double>(targetMin) / kInternalRate;
    }

    // Mode fade: out while a switch is pending, back in once audio of the new kind flows.
    const float modeTarget = switchReq.load() != switchAck.load() ? 0.0f : 1.0f;
    const float m0 = modeGain;
    const float modeStep = static_cast<float>(kBlock) / (kModeFadeSec * kInternalRate);
    const float m1 = m0 < modeTarget ? std::min(modeTarget, m0 + modeStep) : std::max(modeTarget, m0 - modeStep);
    modeGain = haveAudio || modeTarget == 0.0f ? m1 : m0;

    float* out = spatial.data();
    if (!haveAudio) {
        std::fill(spatial.begin(), spatial.end(), 0.0f);
    } else {
        const uint64_t v = sceneVer.load();
        if (v != renderSceneVer) {
            std::unique_lock lk(sceneMu, std::try_to_lock);
            if (lk.owns_lock()) {
                renderScene = scene;
                renderSceneVer = v;
            }
        }
        const int G = groups();
        const int srcOffset = renderStems ? 2 : 0;  // the speaker pair renders the dry mix
        // The upmix delays by exactly one block, so the dry path does too: `dry` becomes the previous block
        // and this block's dry is kept in `dryDelayed` for next time.
        std::swap(dry, dryDelayed);
        for (int i = 0; i < kBlock; ++i) {
            const float* f = &block[static_cast<size_t>(i) * sceneCh];
            dryDelayed[2 * i] = f[0];
            dryDelayed[2 * i + 1] = f[1];
            for (int g = 0; g < G; ++g) {
                stemIn[2 * g][i] = f[srcOffset + 2 * g];
                stemIn[2 * g + 1][i] = f[srcOffset + 2 * g + 1];
            }
        }
        if (renderStems) {
            for (int s = 0; s < numStems; ++s) {
                double acc = 0.0;
                for (int i = 0; i < kBlock; ++i) {
                    const float m = 0.5f * (stemIn[2 * s][i] + stemIn[2 * s + 1][i]);
                    acc += double(m) * m;
                }
                const float rms = static_cast<float>(std::sqrt(acc / kBlock));
                levels[s] = 0.8f * levels[s].load() + 0.2f * rms;
            }
        } else {
            for (auto& l : levels) l = 0.8f * l.load();
        }
        for (int g = 0; g < G; ++g)
            upmix[g].process(stemIn[2 * g].data(), stemIn[2 * g + 1].data(), mono[3 * g].data(),
                             mono[3 * g + 1].data(), mono[3 * g + 2].data());

        const float target = enabled ? 1.0f : 0.0f;
        const float g0 = enableGain;
        const float stepMax = static_cast<float>(kBlock) / (kFadeSec * kInternalRate);
        const float g1 = g0 < target ? std::min(target, g0 + stepMax) : std::max(target, g0 - stepMax);
        enableGain = g1;
        const float master = dbToLin(renderScene.masterGainDb);
        updateHead();
        std::fill(spatial.begin(), spatial.end(), 0.0f);
        if (g0 > 0.0f || g1 > 0.0f) {
            const HeadRotation rot = HeadRotation::fromYawPitchRoll(head.yaw, head.pitch, head.roll);
            buildPoses(rot);
            binaural.process(monoPtrs.data(), poses.data(), out);

            const Room::Preset wantRoom = toRoom(renderScene.room);
            if (wantRoom != roomPreset) {
                room.setPreset(wantRoom);
                roomPreset = wantRoom;
            }
            if (roomPreset != Room::Preset::Off) {
                std::fill(send.begin(), send.end(), 0.0f);
                for (int g = 0; g < G; ++g) {
                    if (sendGains[g] == 0.0f) continue;
                    const float* c = mono[3 * g].data();
                    const float* l = mono[3 * g + 1].data();
                    const float* r = mono[3 * g + 2].data();
                    for (int i = 0; i < kBlock; ++i) send[i] += sendGains[g] * (c[i] + l[i] + r[i]);
                }
                room.process(send.data(), rot, dbToLin(renderScene.roomWetDb), out);
            }

            // HRTFs, the source layout and the room change loudness with the scene; track ~3 s energies so
            // the spatial mix stays as loud as plain stereo and toggling never jumps in level.
            double es = 0.0, ed = 0.0;
            for (int i = 0; i < 2 * kBlock; ++i) {
                es += double(out[i]) * out[i];
                ed += double(dry[i]) * dry[i];
            }
            constexpr double a = static_cast<double>(kBlock) / (3.0 * kInternalRate);
            spatialEnergy += a * (es - spatialEnergy);
            dryEnergy += a * (ed * master * master - dryEnergy);
            if (spatialEnergy > 1e-9) {
                const float want = static_cast<float>(std::clamp(std::sqrt(dryEnergy / spatialEnergy), 0.5, 2.0));
                levelMatch += 0.05f * (want - levelMatch);
            }
        }
        for (int i = 0; i < kBlock; ++i) {
            const float t = static_cast<float>(i) / kBlock;
            const float g = g0 + (g1 - g0) * t;
            const float mg = m0 + (modeGain - m0) * t;
            for (int c = 0; c < 2; ++c) {
                const float y = g * levelMatch * out[2 * i + c] + (1.0f - g) * dry[2 * i + c] * master;
                out[2 * i + c] = softLimit(y) * mg;
            }
        }
    }

    outRs.setRatio(outRatio * driftAdj);
    outRs.process(out, kBlock, outFifo);
}

void Engine::Impl::renderCallback(float* out, size_t frames) {
    if (!outRsReady) {
        renderRate = render.sampleRate();
        outRatio = static_cast<double>(renderRate.load()) / kInternalRate;
        // Always a real converter (not pass-through) so drift correction works even at 44.1 kHz.
        outRs.init(2, outRatio + 1e-9, Resampler::Quality::Medium);
        outRsReady = true;
    }
    // CPU time spent producing audio relative to the audio it covers; above ~0.7 the device starts glitching.
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    while ((outFifo.size() / 2) - outRead < frames) produceBlock();
    QueryPerformanceCounter(&t1);
    static const double qpcFreq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    const double load = (static_cast<double>(t1.QuadPart - t0.QuadPart) / qpcFreq) /
                        (static_cast<double>(frames) / static_cast<double>(renderRate.load()));
    renderLoad = 0.95 * renderLoad.load() + 0.05 * load;
    renderLoadPeak = std::max(load, renderLoadPeak.load() * 0.999);
    std::copy_n(outFifo.begin() + static_cast<std::ptrdiff_t>(outRead * 2), frames * 2, out);
    outRead += frames;
    if (outRead * 2 > outFifo.size() / 2) {
        outFifo.erase(outFifo.begin(), outFifo.begin() + static_cast<std::ptrdiff_t>(outRead * 2));
        outRead = 0;
    }
}

// ------------------------------------------------------------------------------------------------

Engine::Engine() : impl_(std::make_unique<Impl>()) {}

Engine::~Engine() { stop(); }

bool Engine::running() const { return impl_->running; }

bool Engine::start(const Config& cfg) {
    stop();
    ComInit com;
    Impl& m = *impl_;
    cfg_ = cfg;
    m.cfg = cfg;
    m.faulted = false;
    m.underruns = 0;
    m.renderLoad = 0.0;
    m.renderLoadPeak = 0.0;
    {
        std::lock_guard lk(m.statusMu);
        m.error.clear();
        m.adaptiveReason.clear();
    }
    auto fail = [&](const std::string& e) {
        m.setError(e);
        stop();
        m.setStage("Stopped");
        return false;
    };

    if (cfg.headphoneDeviceId.empty()) return fail("Choose your headphones.");
    if (cfg.sinkDeviceId.empty() || cfg.sinkDeviceId == cfg.headphoneDeviceId)
        return fail("Choose a silent device that is not your headphones.");
    if (cfg.capture == CaptureMode::SelectedApps && cfg.appPids.empty()) return fail("Select at least one app.");

    // 1) Separation model (kept loaded across restarts). Adaptive mode still runs without one.
    m.stemsCapable = false;
    if (cfg.mode != Mode::VirtualSpeakers) {
        if (m.loadedModel != cfg.modelPath || !m.model.loaded()) {
            m.setStage("Loading model");
            std::string err;
            if (m.model.load(cfg.modelPath, HtDemucs::Provider::Auto, &err)) {
                m.loadedModel = cfg.modelPath;
            } else {
                m.loadedModel.clear();
                if (cfg.mode == Mode::Stems) return fail("Could not load the separation model: " + err);
                logx::warn("separation model unavailable, adaptive mode stays on virtual speakers: " + err);
            }
        }
        m.stemsCapable = m.model.loaded() && m.loadedModel == cfg.modelPath;
    }
    if (m.stemsCapable) {
        m.sep.init(&m.model, static_cast<int>(cfg.hopSec * kInternalRate),
                   static_cast<int>(cfg.lookaheadSec * kInternalRate));
        m.numStems = std::min(m.model.numSources(), kMaxStems);
        m.sceneCh = m.sep.channels();
        m.maxGroups = m.numStems;
        std::lock_guard lk(m.statusMu);
        m.stemNames = m.model.sources();
        m.provider = m.model.providerName();
    } else {
        m.numStems = 0;
        m.sceneCh = 2;
        m.maxGroups = 1;
        std::lock_guard lk(m.statusMu);
        m.stemNames.clear();
        m.provider.clear();
        if (cfg.mode == Mode::Adaptive) m.adaptiveReason = "no separation model";
    }
    m.hopMs = cfg.hopSec * 1000.0;
    m.currentHopSec = m.stemsCapable ? static_cast<double>(m.sep.hop()) / kInternalRate : 0.0;
    m.lastHintMs = 0;
    m.gpuFallbackUntil = 0;
    m.overloadSteps = 0;

    // Start in the mode the worker would pick right now, so there is no switch at startup.
    const bool initialStems = m.decideStems(GetTickCount64());
    m.workerStems = m.renderStems = initialStems;
    m.switchToStems = initialStems;
    m.activeStems = initialStems;
    m.switchReq = 0;
    m.switchAck = 0;
    m.modeGain = 1.0f;

    m.setStage("Starting audio");
    std::string err;
    // Three sources (centre, left, right) per group; sized for the larger of the two modes.
    if (!m.binaural.init(kInternalRate, kBlock, 3 * m.maxGroups, &err)) return fail("HRTF renderer: " + err);
    if (!m.room.init(kInternalRate, kBlock, &err)) return fail("Room renderer: " + err);
    m.roomPreset = Room::Preset::Off;

    // 2) Render-side state (owned by the render thread once it starts).
    const auto G = static_cast<size_t>(m.maxGroups);
    m.block.assign(static_cast<size_t>(kBlock) * m.sceneCh, 0.0f);
    m.dry.assign(2 * kBlock, 0.0f);
    m.dryDelayed.assign(2 * kBlock, 0.0f);
    m.spatial.assign(2 * kBlock, 0.0f);
    m.send.assign(kBlock, 0.0f);
    m.stemIn.assign(2 * G, std::vector<float>(kBlock, 0.0f));
    m.mono.assign(3 * G, std::vector<float>(kBlock, 0.0f));
    m.monoPtrs.resize(m.mono.size());
    for (size_t i = 0; i < m.mono.size(); ++i) m.monoPtrs[i] = m.mono[i].data();
    m.poses.assign(m.mono.size(), SourcePose{});
    m.sendGains.assign(G, 0.0f);
    m.upmix.resize(G);
    for (auto& u : m.upmix) u.init();
    m.head = {};
    m.outFifo.clear();
    m.outFifo.reserve(1 << 16);
    m.outRead = 0;
    m.outRsReady = false;
    m.primed = false;
    m.padSilence = 0;
    m.driftAdj = 1.0;
    m.enableGain = m.enabled ? 1.0f : 0.0f;
    m.levelMatch = 1.0f;
    m.spatialEnergy = m.dryEnergy = 0.0;
    // Cushion = fixed margin + (stems) the separator's worst recent inference; starts assuming ~150 ms.
    m.targetMin = m.cushionBase() + (m.renderStems ? static_cast<size_t>(0.15 * kInternalRate) : 0);
    m.cushionMs = 1000.0 * static_cast<double>(m.targetMin) / kInternalRate;
    m.inferencePeakMs = 0.0;
    m.loggedUnderruns = 0;
    m.periodLen = static_cast<size_t>(std::max(1.0, m.stemsCapable ? 3.0 * cfg.maxHopSec : 0.0) * kInternalRate);
    m.periodMin = SIZE_MAX;
    m.periodCount = 0;
    m.avgFill = 0.0;
    m.renderSceneVer = 0;
    for (auto& l : m.levels) l = 0.0f;
    m.sceneRing.init(static_cast<size_t>(6 * kInternalRate), m.sceneCh);

    // 3) Headphones first: if they fail nothing has been rerouted yet.
    if (!m.render.start(cfg.headphoneDeviceId, [&m](float* out, size_t n) { m.renderCallback(out, n); }))
        return fail("Could not open headphones: " + m.render.error());

    // 4) Tap the silent device. The keep-alive stream keeps its loopback clock running through silence.
    if (!m.inputEvent) m.inputEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m.inRing.init(kInputRingFrames, 2);
    {
        SpscRing* r = &m.inRing;
        HANDLE ev = m.inputEvent;
        std::atomic<float>* peak = &m.inputPeak;
        audio::CaptureStream::Config cc;
        cc.deviceId = cfg.sinkDeviceId;
        const bool ok = m.capture.start(cc, [r, ev, peak](const float* stereo, size_t frames) {
            float p = 0.0f;
            for (size_t i = 0; i < frames * 2; ++i) p = std::max(p, std::fabs(stereo[i]));
            peak->store(std::max(p, peak->load() * 0.95f));
            r->write(stereo, frames);
            SetEvent(ev);
        });
        if (!ok) return fail("Could not tap the silent device: " + m.capture.error());
    }
    if (!m.sinkKeepAlive.start(cfg.sinkDeviceId, [](float* out, size_t n) { std::fill_n(out, n * 2, 0.0f); }))
        logx::warn("silent keep-alive stream failed: " + m.sinkKeepAlive.error());
    m.captureRate = m.capture.sampleRate();
    m.inRs.init(2, static_cast<double>(kInternalRate) / m.captureRate, Resampler::Quality::Medium);

    // 5) Routing last, so the user only loses direct audio once the pipeline is live.
    auto& journal = audio::RoutingJournal::instance();
    switch (cfg.capture) {
        case CaptureMode::WholeSystem:
            if (!journal.redirectSystemDefault(cfg.sinkDeviceId)) return fail("Could not change the default output device.");
            break;
        case CaptureMode::SelectedApps:
            if (!audio::perAppRoutingSupported()) return fail("Per-app output routing is not available on this Windows build.");
            for (uint32_t pid : cfg.appPids)
                if (!journal.redirectApp(pid, cfg.sinkDeviceId))
                    logx::warn("could not route app " + std::to_string(pid) + " (it may have exited)");
            break;
    }

    m.stopWorker = false;
    m.worker = std::thread(&Impl::workerMain, &m);
    m.running = true;
    m.setStage("Running");
    const char* modeName = cfg.mode == Mode::Adaptive ? "adaptive" : cfg.mode == Mode::Stems ? "stems" : "virtual speakers";
    logx::info(std::string("engine running: ") + modeName + (initialStems ? " (stems)" : " (virtual speakers)") +
               ", capture " + std::to_string(m.captureRate) + " Hz");
    return true;
}

void Engine::stop() {
    Impl& m = *impl_;
    ComInit com;
    m.stopWorker = true;
    if (m.worker.joinable()) m.worker.join();
    // Silence our output before handing audio back, so nothing plays twice.
    m.render.stop();
    audio::RoutingJournal::instance().restoreAll();
    m.capture.stop();
    m.sinkKeepAlive.stop();
    m.binaural.shutdown();
    if (m.running.exchange(false)) logx::info("engine stopped");
    m.setStage("Stopped");
}

void Engine::setEnabled(bool on) { impl_->enabled = on; }
bool Engine::enabled() const { return impl_->enabled; }

void Engine::setScene(const SceneParams& s) {
    std::lock_guard lk(impl_->sceneMu);
    impl_->scene = s;
    impl_->sceneVer++;
}

SceneParams Engine::scene() const {
    std::lock_guard lk(impl_->sceneMu);
    return impl_->scene;
}

void Engine::setHeadTracker(const track::HeadTracker* tracker) { impl_->tracker = tracker; }

void Engine::setLowLatencyHint(bool on, const std::string& reason) {
    {
        std::lock_guard lk(impl_->statusMu);
        impl_->hintReason = reason;
    }
    impl_->lowLatencyHint = on;
}

Status Engine::status() const {
    const Impl& m = *impl_;
    Status st;
    st.running = m.running;
    st.enabled = m.enabled;
    {
        std::lock_guard lk(m.statusMu);
        st.error = m.error;
        st.stage = m.stage;
        st.stemNames = m.stemNames;
        st.provider = m.provider;
        st.adaptiveReason = m.adaptiveReason;
    }
    const bool stems = m.activeStems;
    st.activeMode = stems ? Mode::Stems : Mode::VirtualSpeakers;
    st.hopSec = stems ? m.currentHopSec.load() : 0.0;
    st.inferenceMs = m.inferenceMs;
    st.gpuLoad = stems && st.hopSec > 0 ? m.inferenceMs / (1000.0 * st.hopSec) : 0.0;
    st.inputPeak = m.inputPeak;
    for (int s = 0; s < kMaxStems; ++s) st.stemLevels[s] = m.levels[s];
    st.underruns = m.underruns;
    st.renderLoad = m.renderLoad;
    st.renderLoadPeak = m.renderLoadPeak;
    st.captureRate = m.captureRate;
    st.renderRate = m.renderRate;
    st.headTracked = st.running && m.headTracked;
    st.headYawDeg = m.headYaw;
    st.headPitchDeg = m.headPitch;
    st.headRollDeg = m.headRoll;
    if (st.running) {
        const double algorithmic = stems ? m.cfg.lookaheadSec + st.hopSec + m.inferenceMs / 1000.0 : 0.0;
        st.latencySec = algorithmic + m.avgFill / kInternalRate + m.render.latencySeconds() + 0.02;
    }
    return st;
}

}  // namespace engine

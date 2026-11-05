#include "sep/HtDemucs.h"

#include "util/Log.h"

#include <windows.h>
#include <dxgi1_6.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <dml_provider_factory.h>
#include <onnxruntime_cxx_api.h>
#include <pocketfft_hdronly.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <sstream>
#include <execution>
#include <thread>

namespace {

constexpr int kNfft = 4096;
constexpr int kHop = 1024;
constexpr int kPad = kHop / 2 * 3;  // 1536
constexpr int kBins = kNfft / 2;    // 2048, Nyquist dropped
constexpr float kNorm = 1.0f / 64.0f;  // torch.stft normalized=True: 1/sqrt(n_fft)

Ort::Env& ortEnv() {
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "spatiall");
    return env;
}

// numpy/torch 'reflect' padding index (edge sample not repeated). Valid while pad < n.
inline int reflectIndex(int i, int n) {
    if (i < 0) return -i;
    if (i >= n) return 2 * (n - 1) - i;
    return i;
}

std::vector<std::string> readSources(const std::filesystem::path& json) {
    std::ifstream f(json);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string s = ss.str();
    std::vector<std::string> out;
    const size_t key = s.find("\"sources\"");
    if (key == std::string::npos) return out;
    const size_t open = s.find('[', key), close = s.find(']', key);
    if (open == std::string::npos || close == std::string::npos) return out;
    for (size_t p = open; p < close;) {
        const size_t a = s.find('"', p + 1);
        if (a == std::string::npos || a > close) break;
        const size_t b = s.find('"', a + 1);
        out.push_back(s.substr(a + 1, b - a - 1));
        p = b;
    }
    return out;
}

std::string gpuKey() {
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return "no-dxgi";
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))))
        return "no-adapter";
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    LARGE_INTEGER driver{};
    adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver);
    return logx::narrow(desc.Description) + " driver " + std::to_string(driver.QuadPart);
}

std::filesystem::path checkCachePath() {
    PWSTR base = nullptr;
    std::filesystem::path p;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        p = std::filesystem::path(base) / L"Spatiall" / L"dml-check.txt";
        CoTaskMemFree(base);
    }
    return p;
}

}  // namespace

struct HtDemucs::Session {
    Ort::Session session{nullptr};
    std::string provider;
};

HtDemucs::HtDemucs() = default;
HtDemucs::~HtDemucs() = default;

bool HtDemucs::loaded() const { return session_ != nullptr; }

bool HtDemucs::load(const std::wstring& modelPath, Provider provider, std::string* error) {
    session_.reset();
    const std::filesystem::path model(modelPath);
    sources_ = readSources(std::filesystem::path(model).replace_extension(".json"));
    if (sources_.empty()) {
        if (error) *error = "model metadata (.json with \"sources\") missing next to " + logx::narrow(modelPath);
        return false;
    }

    bool useDml = provider != Provider::Cpu;
    if (provider == Provider::Auto) useDml = selfCheck(modelPath, error);
    session_ = createSession(modelPath, useDml, error);
    if (!session_ && useDml) {
        logx::warn("DirectML session failed, falling back to CPU: " + (error ? *error : std::string()));
        session_ = createSession(modelPath, false, error);
    }
    if (!session_) return false;
    providerName_ = session_->provider;

    // Shapes come from the graph: mix [1, 2, N].
    auto shape = session_->session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    auto outShape = session_->session.GetOutputTypeInfo(1).GetTensorTypeAndShapeInfo().GetShape();
    if (shape.size() != 3 || outShape.size() != 4 || outShape[1] != static_cast<int64_t>(sources_.size())) {
        if (error) *error = "unexpected model I/O shapes";
        session_.reset();
        return false;
    }
    segment_ = static_cast<int>(shape[2]);
    frames_ = (segment_ + kHop - 1) / kHop;

    window_.resize(kNfft);
    for (int i = 0; i < kNfft; ++i)
        window_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * i / kNfft));  // periodic

    const size_t S = sources_.size();
    specIn_.assign(4ull * kBins * frames_, 0.0f);
    freqOut_.assign(S * 4ull * kBins * frames_, 0.0f);
    timeOut_.assign(S * 2ull * segment_, 0.0f);
    // One scratch set per STFT input channel / iSTFT output plane so planes can be processed in parallel.
    const size_t olaLen = static_cast<size_t>(kNfft) + static_cast<size_t>(kHop) * (frames_ + 3);
    scratch_.resize(std::max<size_t>(2, 2 * S));
    for (auto& sc : scratch_) {
        sc.padded.assign(static_cast<size_t>(frames_) * kHop + 2 * kPad, 0.0f);
        sc.fft.assign(kNfft, 0.0f);
        sc.ola.assign(olaLen, 0.0f);
    }

    // iSTFT window-square envelope over all frames (incl. the zero frames torch pads with), at output positions.
    const int totalFrames = frames_ + 4;
    std::vector<double> env(olaLen, 0.0);
    for (int f = 0; f < totalFrames; ++f)
        for (int i = 0; i < kNfft; ++i) env[static_cast<size_t>(f) * kHop + i] += double(window_[i]) * window_[i];
    envInv_.resize(segment_);
    for (int i = 0; i < segment_; ++i) {
        const double e = env[static_cast<size_t>(kNfft / 2 + kPad + i)];
        envInv_[i] = e > 1e-11 ? static_cast<float>(1.0 / e) : 1.0f;
    }
    // The first DirectML run compiles shaders and takes seconds; do it now rather than mid-stream, where it
    // would stall playback and inflate the engine's jitter cushion.
    {
        std::vector<float> mix(2ull * segment_, 0.0f), out(S * 2ull * segment_);
        const auto t0 = std::chrono::steady_clock::now();
        run(mix.data(), out.data());
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        logx::info("model warm-up " + std::to_string(static_cast<int>(ms)) + " ms");
    }
    logx::info("model " + logx::narrow(model.filename().wstring()) + ": " + std::to_string(S) + " sources, window " +
               std::to_string(segment_) + " samples, provider " + providerName_);
    return true;
}

std::unique_ptr<HtDemucs::Session> HtDemucs::createSession(const std::wstring& path, bool dml, std::string* error) {
    try {
        Ort::SessionOptions so;
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        if (dml) {
            so.DisableMemPattern();
            so.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
            // DirectML's whole-graph fusion produces garbage for htdemucs (verified on GTX 1080 Ti and
            // reported on RTX 5060); per-operator execution is correct and only ~15% slower.
            so.AddConfigEntry("ep.dml.disable_graph_fusion", "1");
            const OrtDmlApi* api = nullptr;
            Ort::ThrowOnError(
                Ort::GetApi().GetExecutionProviderApi("DML", ORT_API_VERSION, reinterpret_cast<const void**>(&api)));
            OrtDmlDeviceOptions opts{OrtDmlPerformancePreference::HighPerformance, OrtDmlDeviceFilter::Gpu};
            Ort::ThrowOnError(api->SessionOptionsAppendExecutionProvider_DML2(so, &opts));
        } else {
            const unsigned hw = std::thread::hardware_concurrency();
            so.SetIntraOpNumThreads(static_cast<int>(hw > 4 ? hw - 2 : hw));
        }
        auto s = std::make_unique<HtDemucs::Session>();
        s->session = Ort::Session(ortEnv(), path.c_str(), so);
        s->provider = dml ? "DirectML" : "CPU";
        return s;
    } catch (const Ort::Exception& e) {
        if (error) *error = e.what();
        return nullptr;
    }
}

bool HtDemucs::selfCheck(const std::wstring& modelPath, std::string* error) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(modelPath, ec);
    const auto mtime = std::filesystem::last_write_time(modelPath, ec).time_since_epoch().count();
    const std::string key = gpuKey() + " | " + logx::narrow(std::filesystem::path(modelPath).filename().wstring()) +
                            " " + std::to_string(size) + " " + std::to_string(mtime) + " | ort " +
                            Ort::GetVersionString();
    const auto cache = checkCachePath();
    {
        std::ifstream in(cache);
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind(key + " = ", 0) == 0) return line.substr(key.size() + 3) == "ok";
        }
    }

    logx::info("verifying DirectML output against CPU (once per GPU/driver/model)...");
    std::string err;
    auto dml = createSession(modelPath, true, &err);
    auto cpu = dml ? createSession(modelPath, false, &err) : nullptr;
    bool ok = false;
    if (dml && cpu) {
        auto shape = cpu->session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        segment_ = static_cast<int>(shape[2]);
        frames_ = (segment_ + kHop - 1) / kHop;
        const size_t S = sources_.size();
        specIn_.assign(4ull * kBins * frames_, 0.0f);
        freqOut_.assign(S * 4ull * kBins * frames_, 0.0f);
        timeOut_.assign(S * 2ull * segment_, 0.0f);
        // Deterministic pseudo-noise; the spectrogram input only needs to be self-consistent, not exact.
        std::vector<float> mix(2ull * segment_);
        uint32_t seed = 12345;
        for (auto& v : mix) {
            seed = seed * 1664525u + 1013904223u;
            v = (static_cast<float>(seed >> 8) / 16777216.0f - 0.5f) * 0.2f;
        }
        for (size_t i = 0; i < specIn_.size(); ++i) specIn_[i] = mix[i % mix.size()];
        std::vector<float> a, b;
        auto runRaw = [&](Session& s, std::vector<float>& outTime) {
            if (!runSession(s, mix.data(), nullptr)) return false;
            outTime = timeOut_;
            outTime.insert(outTime.end(), freqOut_.begin(), freqOut_.end());
            return true;
        };
        if (runRaw(*cpu, a) && runRaw(*dml, b)) {
            double sab = 0, saa = 0, sbb = 0;
            bool finite = true;
            for (size_t i = 0; i < a.size(); ++i) {
                finite &= std::isfinite(b[i]);
                sab += double(a[i]) * b[i];
                saa += double(a[i]) * a[i];
                sbb += double(b[i]) * b[i];
            }
            const double corr = sab / std::sqrt(saa * sbb + 1e-30);
            ok = finite && corr > 0.999;
            logx::info("DirectML self-check correlation " + std::to_string(corr) + (ok ? " (ok)" : " (MISMATCH)"));
        }
    } else {
        logx::warn("DirectML self-check could not create sessions: " + err);
    }
    std::filesystem::create_directories(cache.parent_path(), ec);
    std::ofstream(cache, std::ios::app) << key << " = " << (ok ? "ok" : "bad") << "\n";
    if (!ok && error) *error = "DirectML output check failed; using CPU";
    return ok;
}

bool HtDemucs::runSession(Session& s, const float* mix, float* out) {
    const int64_t S = static_cast<int64_t>(sources_.size());
    const int64_t mixShape[3] = {1, 2, segment_};
    const int64_t specShape[4] = {1, 4, kBins, frames_};
    const int64_t freqShape[5] = {1, S, 4, kBins, frames_};
    const int64_t timeShape[4] = {1, S, 2, segment_};
    try {
        auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value inputs[2] = {
            Ort::Value::CreateTensor<float>(mem, const_cast<float*>(mix), 2ull * segment_, mixShape, 3),
            Ort::Value::CreateTensor<float>(mem, specIn_.data(), specIn_.size(), specShape, 4)};
        Ort::Value outputs[2] = {
            Ort::Value::CreateTensor<float>(mem, freqOut_.data(), freqOut_.size(), freqShape, 5),
            Ort::Value::CreateTensor<float>(mem, timeOut_.data(), timeOut_.size(), timeShape, 4)};
        const char* inNames[2] = {"mix", "spec"};
        const char* outNames[2] = {"freq", "time"};
        s.session.Run(Ort::RunOptions{nullptr}, inNames, inputs, 2, outNames, outputs, 2);
    } catch (const Ort::Exception& e) {
        logx::error(std::string("inference failed: ") + e.what());
        return false;
    }
    if (out) {
        std::copy(timeOut_.begin(), timeOut_.end(), out);
        ispecAdd(out);
    }
    return true;
}

bool HtDemucs::run(const float* mix, float* out) {
    if (!session_) return false;
    spec(mix);
    if (!runSession(*session_, mix, out)) return false;
    // Guard against silent numerical failure of the GPU path (NaN/Inf would poison the whole mix).
    const size_t n = static_cast<size_t>(numSources()) * 2 * segment_;
    for (size_t i = 0; i < n; i += 997)
        if (!std::isfinite(out[i])) return false;
    return true;
}

// HTDemucs._spec + _magnitude(cac): reflect-pad, centred STFT, drop Nyquist, keep frames [2, 2 + le).
void HtDemucs::spec(const float* mix) {
    const int n = segment_;
    const int planes[2] = {0, 1};
    std::for_each(std::execution::par, planes, planes + 2, [&](int c) {
        static thread_local pocketfft::detail::pocketfft_r<float> plan(kNfft);
        Scratch& sc = scratch_[c];
        const int len1 = static_cast<int>(sc.padded.size());  // le*hop + 2*pad
        const float* x = mix + static_cast<size_t>(c) * n;
        for (int i = 0; i < len1; ++i) sc.padded[i] = x[reflectIndex(i - kPad, n)];
        float* re = &specIn_[static_cast<size_t>(2 * c) * kBins * frames_];
        float* im = &specIn_[static_cast<size_t>(2 * c + 1) * kBins * frames_];
        float* f = sc.fft.data();
        for (int t = 0; t < frames_; ++t) {
            // torch.stft(center=True) reflect-pads n_fft/2; frame f starts at f*hop in that padded signal.
            const int start = (t + 2) * kHop - kNfft / 2;
            for (int i = 0; i < kNfft; ++i) f[i] = sc.padded[reflectIndex(start + i, len1)] * window_[i];
            plan.exec(f, kNorm, true);  // halfcomplex: r0, r1, i1, r2, i2, ..., r(n/2)
            re[t] = f[0];
            im[t] = 0.0f;
            for (int k = 1; k < kBins; ++k) {
                re[static_cast<size_t>(k) * frames_ + t] = f[2 * k - 1];
                im[static_cast<size_t>(k) * frames_ + t] = f[2 * k];
            }
        }
    });
}

// HTDemucs._mask(cac) + _ispec: add zero Nyquist bin and 2 zero frames per side, centred iSTFT, crop.
void HtDemucs::ispecAdd(float* out) {
    const float invScale = 1.0f / (kNfft * kNorm);  // irfft 1/N, undo stft normalization
    std::vector<int> planes(static_cast<size_t>(numSources()) * 2);
    std::iota(planes.begin(), planes.end(), 0);
    std::for_each(std::execution::par, planes.begin(), planes.end(), [&](int p) {
        static thread_local pocketfft::detail::pocketfft_r<float> plan(kNfft);
        const int s = p / 2, c = p % 2;
        Scratch& sc = scratch_[p];
        std::fill(sc.ola.begin(), sc.ola.end(), 0.0f);
        const float* re = &freqOut_[(static_cast<size_t>(s) * 4 + 2 * c) * kBins * frames_];
        const float* im = &freqOut_[(static_cast<size_t>(s) * 4 + 2 * c + 1) * kBins * frames_];
        float* f = sc.fft.data();
        for (int t = 0; t < frames_; ++t) {
            f[0] = re[t];
            for (int k = 1; k < kBins; ++k) {
                f[2 * k - 1] = re[static_cast<size_t>(k) * frames_ + t];
                f[2 * k] = im[static_cast<size_t>(k) * frames_ + t];
            }
            f[kNfft - 1] = 0.0f;  // Nyquist
            plan.exec(f, invScale, false);
            float* dst = &sc.ola[static_cast<size_t>(t + 2) * kHop];
            for (int i = 0; i < kNfft; ++i) dst[i] += f[i] * window_[i];
        }
        float* o = out + static_cast<size_t>(p) * segment_;
        const float* src = &sc.ola[kNfft / 2 + kPad];
        for (int i = 0; i < segment_; ++i) o[i] += src[i] * envInv_[i];
    });
}

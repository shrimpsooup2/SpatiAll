#pragma once
// HTDemucs inference on one fixed-length window: C++ STFT/iSTFT around the exported ONNX core
// (tools/export_htdemucs.py; NumPy reference in tools/htdemucs_ref.py).

#include <memory>
#include <string>
#include <vector>

class HtDemucs {
public:
    enum class Provider { Auto, DirectML, Cpu };

    HtDemucs();
    ~HtDemucs();
    HtDemucs(const HtDemucs&) = delete;
    HtDemucs& operator=(const HtDemucs&) = delete;

    // Loads model + sidecar json. Auto = DirectML verified against CPU once per GPU/driver/model, else CPU.
    bool load(const std::wstring& modelPath, Provider provider, std::string* error);
    bool loaded() const;

    int segment() const { return segment_; }  // window length in samples at 44.1 kHz
    int numSources() const { return static_cast<int>(sources_.size()); }
    const std::vector<std::string>& sources() const { return sources_; }
    const std::string& providerName() const { return providerName_; }

    // mix: planar [2][segment]. out: planar [S][2][segment]. Returns false on inference error.
    bool run(const float* mix, float* out);

private:
    struct Session;
    static std::unique_ptr<Session> createSession(const std::wstring& path, bool dml, std::string* error);
    bool runSession(Session& s, const float* mix, float* out);
    void spec(const float* mix);
    void ispecAdd(float* out);
    bool selfCheck(const std::wstring& modelPath, std::string* error);

    std::unique_ptr<Session> session_;
    std::vector<std::string> sources_;
    std::string providerName_;
    int segment_ = 0;
    int frames_ = 0;  // STFT frames fed to the model (ceil(segment / hop))

    std::vector<float> window_;
    std::vector<float> specIn_;   // [1, 4, 2048, frames]
    std::vector<float> freqOut_;  // [1, S, 4, 2048, frames]
    std::vector<float> timeOut_;  // [1, S, 2, segment]
    std::vector<float> envInv_;   // 1 / iSTFT window envelope at output positions
    struct Scratch {
        std::vector<float> padded, fft, ola;
    };
    std::vector<Scratch> scratch_;  // per STFT channel / iSTFT plane
};

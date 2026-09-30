#include "audio/output.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

#include "core/log.h"
#include "core/platform.h"
#include "core/strings.h"

#if defined(_WIN32)
#include <windows.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#endif

namespace avc::audio {

namespace {

class NullOutput final : public IAudioOutput {
public:
    NullOutput(int sr, int block) : sr_(sr), block_(block) {}
    ~NullOutput() override { stop(); }
    bool start(Callback cb) override {
        stop();
        cb_ = std::move(cb);
        running_ = true;
        thread_ = std::thread([this] {
            setCurrentThreadName("audio-null");
            std::vector<float> buf(static_cast<size_t>(block_) * 2);
            auto next = std::chrono::steady_clock::now();
            const auto period = std::chrono::nanoseconds(static_cast<int64_t>(1e9 * block_ / sr_));
            while (running_) {
                cb_(buf.data(), block_);
                next += period;
                std::this_thread::sleep_until(next);
            }
        });
        return true;
    }
    void stop() override {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }
    int sampleRate() const override { return sr_; }
    int latencyFrames() const override { return block_; }
    std::string deviceName() const override { return "No audio device"; }

private:
    int sr_, block_;
    Callback cb_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

#if defined(_WIN32)

// Local definitions avoid depending on GUID libraries that differ between SDKs.
const GUID kSubtypeIeeeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

template <typename T>
void safeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

class WasapiOutput final : public IAudioOutput {
public:
    ~WasapiOutput() override {
        stop();
        safeRelease(render_);
        safeRelease(client_);
        safeRelease(device_);
        if (event_) CloseHandle(event_);
    }

    bool init(const std::string& deviceId, int bufferMs, std::string* error) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator* en = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void**>(&en)))) {
            if (error) *error = "MMDeviceEnumerator unavailable";
            return false;
        }
        HRESULT hr = deviceId.empty() ? en->GetDefaultAudioEndpoint(eRender, eConsole, &device_)
                                      : en->GetDevice(utf8ToWide(deviceId).c_str(), &device_);
        if (FAILED(hr) && !deviceId.empty()) hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
        en->Release();
        if (FAILED(hr)) {
            if (error) *error = "No audio output device";
            return false;
        }
        IPropertyStore* props = nullptr;
        if (SUCCEEDED(device_->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) name_ = wideToUtf8(v.pwszVal);
            PropVariantClear(&v);
            props->Release();
        }
        if (FAILED(device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client_)))) {
            if (error) *error = "IAudioClient activation failed";
            return false;
        }
        // 48 kHz stereo float; the audio engine converts to the mix format.
        WAVEFORMATEXTENSIBLE wf{};
        wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wf.Format.nChannels = 2;
        wf.Format.nSamplesPerSec = 48000;
        wf.Format.wBitsPerSample = 32;
        wf.Format.nBlockAlign = 8;
        wf.Format.nAvgBytesPerSec = 48000 * 8;
        wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wf.Samples.wValidBitsPerSample = 32;
        wf.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        wf.SubFormat = kSubtypeIeeeFloat;
        const REFERENCE_TIME dur = static_cast<REFERENCE_TIME>(std::max(5, bufferMs)) * 10000;
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, dur, 0, reinterpret_cast<WAVEFORMATEX*>(&wf), nullptr);
        if (FAILED(hr)) {
            if (error) *error = "IAudioClient::Initialize failed (0x" + hex64(static_cast<uint32_t>(hr)) + ")";
            return false;
        }
        client_->GetBufferSize(&bufferFrames_);
        REFERENCE_TIME lat = 0;
        client_->GetStreamLatency(&lat);
        latency_ = static_cast<int>(lat * 48000 / 10000000) + static_cast<int>(bufferFrames_);
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        client_->SetEventHandle(event_);
        if (FAILED(client_->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render_)))) {
            if (error) *error = "IAudioRenderClient unavailable";
            return false;
        }
        AVC_INFO("audio", "WASAPI output '{}' ({} frame buffer, ~{} ms latency)", name_, bufferFrames_, latency_ * 1000 / 48000);
        return true;
    }

    bool start(Callback cb) override {
        stop();
        cb_ = std::move(cb);
        running_ = true;
        // Prefill with silence to avoid a startup glitch.
        BYTE* data = nullptr;
        if (SUCCEEDED(render_->GetBuffer(bufferFrames_, &data))) render_->ReleaseBuffer(bufferFrames_, AUDCLNT_BUFFERFLAGS_SILENT);
        thread_ = std::thread([this] { loop(); });
        return SUCCEEDED(client_->Start());
    }

    void stop() override {
        if (!running_) return;
        running_ = false;
        SetEvent(event_);
        if (thread_.joinable()) thread_.join();
        client_->Stop();
        client_->Reset();
    }

    int sampleRate() const override { return 48000; }
    int latencyFrames() const override { return latency_; }
    std::string deviceName() const override { return name_; }
    uint64_t underruns() const override { return underruns_.load(); }

private:
    void loop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        setCurrentThreadName("audio-wasapi");
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        while (running_) {
            if (WaitForSingleObject(event_, 200) != WAIT_OBJECT_0) {
                ++underruns_;
                continue;
            }
            UINT32 padding = 0;
            if (FAILED(client_->GetCurrentPadding(&padding))) break;
            const UINT32 avail = bufferFrames_ - padding;
            if (avail == 0) continue;
            BYTE* data = nullptr;
            if (FAILED(render_->GetBuffer(avail, &data))) continue;
            cb_(reinterpret_cast<float*>(data), static_cast<int>(avail));
            render_->ReleaseBuffer(avail, 0);
        }
        CoUninitialize();
    }

    IMMDevice* device_ = nullptr;
    IAudioClient* client_ = nullptr;
    IAudioRenderClient* render_ = nullptr;
    HANDLE event_ = nullptr;
    UINT32 bufferFrames_ = 0;
    int latency_ = 0;
    std::string name_ = "Default output";
    Callback cb_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> underruns_{0};
    std::thread thread_;
};

#endif

}  // namespace

std::unique_ptr<IAudioOutput> createNullOutput(int sampleRate, int blockFrames) {
    return std::make_unique<NullOutput>(sampleRate, blockFrames);
}

#if defined(_WIN32)
std::unique_ptr<IAudioOutput> createWasapiOutput(const std::string& deviceId, int bufferMs, std::string* error) {
    auto out = std::make_unique<WasapiOutput>();
    if (!out->init(deviceId, bufferMs, error)) return nullptr;
    return out;
}
#endif

std::vector<AudioDeviceInfo> listAudioOutputs() {
    std::vector<AudioDeviceInfo> out;
#if defined(_WIN32)
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&en))))
        return out;
    std::wstring defId;
    IMMDevice* def = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &def))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(def->GetId(&id))) {
            defId = id;
            CoTaskMemFree(id);
        }
        def->Release();
    }
    IMMDeviceCollection* coll = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll))) {
        UINT n = 0;
        coll->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            IMMDevice* d = nullptr;
            if (FAILED(coll->Item(i, &d))) continue;
            AudioDeviceInfo info;
            LPWSTR id = nullptr;
            if (SUCCEEDED(d->GetId(&id))) {
                info.id = wideToUtf8(id);
                info.isDefault = defId == id;
                CoTaskMemFree(id);
            }
            IPropertyStore* props = nullptr;
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &props))) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) info.name = wideToUtf8(v.pwszVal);
                PropVariantClear(&v);
                props->Release();
            }
            d->Release();
            out.push_back(std::move(info));
        }
        coll->Release();
    }
    en->Release();
#endif
    return out;
}

std::unique_ptr<IAudioOutput> createDefaultOutput(const std::string& deviceId, int bufferMs) {
#if defined(_WIN32)
    std::string err;
    if (auto w = createWasapiOutput(deviceId, bufferMs, &err)) return w;
    AVC_WARN("audio", "WASAPI unavailable ({}); audio muted, using system clock", err);
#else
    (void)deviceId;
    (void)bufferMs;
#endif
    return createNullOutput();
}

}  // namespace avc::audio

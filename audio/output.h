#pragma once
// Audio output devices. The device clock (frames consumed) is the master
// clock for playback: video frames are chosen from the audio position so A/V
// never drift, even over hours.

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace avc::audio {

struct AudioDeviceInfo {
    std::string id;
    std::string name;
    bool isDefault = false;
};

class IAudioOutput {
public:
    // Fill `frames` interleaved stereo float frames.
    using Callback = std::function<void(float* out, int frames)>;
    virtual ~IAudioOutput() = default;
    virtual bool start(Callback cb) = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual int sampleRate() const = 0;
    [[nodiscard]] virtual int latencyFrames() const = 0;
    [[nodiscard]] virtual std::string deviceName() const = 0;
    [[nodiscard]] virtual uint64_t underruns() const { return 0; }
};

// Timer-driven output that discards samples (headless, tests, no device).
std::unique_ptr<IAudioOutput> createNullOutput(int sampleRate = 48000, int blockFrames = 480);

#if defined(_WIN32)
std::unique_ptr<IAudioOutput> createWasapiOutput(const std::string& deviceId, int bufferMs, std::string* error = nullptr);
#endif

std::vector<AudioDeviceInfo> listAudioOutputs();

// Best available output (WASAPI on Windows, else null).
std::unique_ptr<IAudioOutput> createDefaultOutput(const std::string& deviceId = {}, int bufferMs = 30);

}  // namespace avc::audio

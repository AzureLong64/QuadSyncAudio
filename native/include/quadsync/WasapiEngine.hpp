#pragma once
#include "Dsp.hpp"
#include <memory>
#include <string>
#include <vector>

namespace quadsync {
struct DeviceInfo { std::string id, name, containerId; bool float48=false; };
struct Status {
    bool running=false, correcting=false;
    std::array<float,4> peaks{};
    double lagFrames=0, ratio=1, bufferLagFrames=0;
    uint64_t underrunFrames=0, clippedSamples=0;
    uint32_t bufferA=0, bufferB=0;
    std::string error;
};
class WasapiEngine {
    struct Impl;
    std::unique_ptr<Impl> p_;
public:
    Controls controls;
    WasapiEngine();
    ~WasapiEngine();
    WasapiEngine(const WasapiEngine&)=delete;
    WasapiEngine& operator=(const WasapiEngine&)=delete;
    static std::vector<DeviceInfo> EnumerateDevices();
    // Start/Stop must be serialized. Stop the producer before either call.
    void Start(const std::string& deviceA, const std::string& deviceB);
    void Stop();
    // Single-producer, nonblocking. Return number of accepted FRAMES; retry the remainder.
    size_t WriteInterleaved(const float* pcm, size_t frames);
    size_t WritableFrames() const;
    bool Running() const;
    Status GetStatus();
};
}

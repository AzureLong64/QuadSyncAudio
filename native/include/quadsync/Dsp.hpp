#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace quadsync {
inline constexpr int SampleRate = 48000;
using Frame = std::array<float, 4>;
inline double checked(double value, double lo, double hi) {
    if (!std::isfinite(value) || value < lo || value > hi)
        throw std::invalid_argument("Value outside supported range");
    return value;
}
inline double distanceToMs(double metres) { return checked(metres, 0, 34.3) * 1000.0 / 343.0; }

struct Controls {
    std::array<std::atomic<float>, 4> gainDb{};
    std::array<std::atomic<float>, 4> delayMs{};
    std::atomic<float> master{0.5f};
    std::atomic<bool> mute{false};
    void setChannel(int ch, double db, double ms) {
        if (ch < 0 || ch >= 4) throw std::invalid_argument("Channel must be 0..3");
        checked(db, -60, 6); checked(ms, 0, 100);
        gainDb[ch].store(static_cast<float>(db));
        delayMs[ch].store(static_cast<float>(ms));
    }
};

// Audio-thread-owned history. Control targets alone are shared atomically.
class Dsp {
    static constexpr size_t Size = 4802;
    std::array<std::array<float, Size>, 4> history_{};
    std::array<double, 4> gains_{1,1,1,1}, targets_{1,1,1,1};
    std::array<double, 4> delay_{}, nextDelay_{};
    std::array<int, 4> fade_{};
    size_t write_ = 0;
    double master_ = 0, masterTarget_ = 0;
    const double alpha_ = 1.0 - std::exp(-1.0 / (0.010 * SampleRate));
    float tap(int ch, double samples) const {
        double pos = static_cast<double>(write_) - samples;
        if (pos < 0) pos += Size;
        auto i = static_cast<size_t>(pos);
        float f = static_cast<float>(pos - i);
        return history_[ch][i] * (1-f) + history_[ch][(i+1)%Size] * f;
    }
public:
    void update(const Controls& c) {
        masterTarget_ = c.mute.load() ? 0.f : c.master.load();
        for (int ch=0; ch<4; ++ch) {
            targets_[ch] = std::pow(10.f, c.gainDb[ch].load()/20.f);
            const double requested = c.delayMs[ch].load() * (SampleRate/1000.0);
            // Finish an existing 10-ms crossfade before accepting the latest target.
            if (!fade_[ch] && std::abs(requested-delay_[ch])>0.001) {
                nextDelay_[ch]=requested; fade_[ch]=480;
            }
        }
    }
    Frame process(const Frame& in) {
        Frame out{};
        master_ += alpha_ * (masterTarget_ - master_);
        for (int ch=0; ch<4; ++ch) {
            history_[ch][write_] = std::isfinite(in[ch]) ? in[ch] : 0.f;
            gains_[ch] += alpha_ * (targets_[ch]-gains_[ch]);
            float v = tap(ch, delay_[ch]);
            if (fade_[ch]) {
                float mix = (481-fade_[ch])/480.f;
                v = v*(1-mix) + tap(ch,nextDelay_[ch])*mix;
                if (--fade_[ch]==0) delay_[ch]=nextDelay_[ch];
            }
            out[ch] = static_cast<float>(v * gains_[ch] * master_);
        }
        write_ = (write_+1)%Size;
        return out;
    }
};
}

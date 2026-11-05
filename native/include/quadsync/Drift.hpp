#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace quadsync {
// Map hardware output frames to source frames, including the ratio used for EACH queued block.
class PlaybackMap {
    struct Segment { double output=0, source=0, count=0, step=1; };
    std::array<Segment,512> segments_{};
    uint64_t count_=0;
public:
    void add(double output, double source, double count, double step) {
        segments_[count_++%segments_.size()]={output,source,count,step};
    }
    double sourceAt(double output) const {
        if(!count_) return 0;
        for(uint64_t j=0;j<std::min<uint64_t>(count_,segments_.size());++j) {
            const auto& s=segments_[(count_-1-j)%segments_.size()];
            if(output>=s.output) return s.source+(output-s.output)*s.step;
        }
        throw std::runtime_error("Audio clock fell outside playback history");
    }
};

class DriftController {
    double filtered_=0, integral_=0, correction_=0;
    bool active_=false;
public:
    double update(double lagFrames, double dt) {
        dt=std::clamp(dt,0.0001,0.1);
        filtered_ += (1-std::exp(-dt/0.10))*(lagFrames-filtered_);
        if(std::abs(filtered_)>64) active_=true;
        if(active_) {
            integral_=std::clamp(integral_+filtered_*dt*0.000002,-0.0015,0.0015);
            double target=std::clamp(filtered_*0.000020+integral_,-0.002,0.002);
            correction_+=std::clamp(target-correction_,-0.0005*dt,0.0005*dt);
        }
        // Positive lag = B behind A; consume B's source faster to catch up.
        return 1+correction_;
    }
    double lag() const { return filtered_; }
    bool active() const { return active_; }
};
}

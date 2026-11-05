#pragma once
#include "Dsp.hpp"
#include <cstdint>
#include <vector>

namespace quadsync {
// One producer (WriteInterleaved), one consumer (render thread). Never overwrite unread PCM.
template<size_t Capacity> class InputRing {
    std::array<Frame, Capacity> data_{};
    alignas(64) std::atomic<uint64_t> read_{0};
    alignas(64) std::atomic<uint64_t> write_{0};
public:
    size_t available() const {
        auto w=write_.load(std::memory_order_acquire), r=read_.load(std::memory_order_acquire);
        return w>=r ? std::min(Capacity,static_cast<size_t>(w-r)) : 0;
    }
    size_t writable() const { return Capacity-available(); }
    size_t write(const float* pcm, size_t frames) {
        auto w=write_.load(std::memory_order_relaxed);
        auto n=std::min(frames, Capacity-static_cast<size_t>(w-read_.load(std::memory_order_acquire)));
        for(size_t i=0;i<n;++i) std::copy_n(pcm+i*4,4,data_[(w+i)%Capacity].begin());
        write_.store(w+n,std::memory_order_release); return n;
    }
    bool pop(Frame& f) {
        auto r=read_.load(std::memory_order_relaxed);
        if(r==write_.load(std::memory_order_acquire)) return false;
        f=data_[r%Capacity]; read_.store(r+1,std::memory_order_release); return true;
    }
    void reset() { read_=0; write_=0; } // only with producer and consumer stopped
};

// Single render-thread history, independently addressed by the two device cursors.
class Timeline {
    std::vector<Frame> data_;
    uint64_t end_=0;
public:
    explicit Timeline(size_t capacity=65536): data_(capacity) {}
    uint64_t end() const { return end_; }
    void append(const Frame& f) { data_[end_++%data_.size()]=f; }
    std::array<float,2> stereo(double pos, int firstChannel) const {
        auto i=static_cast<uint64_t>(pos);
        if(pos<0 || i+1>=end_ || (end_>data_.size() && i<end_-data_.size()))
            throw std::runtime_error("Audio history exhausted: restart the stream");
        float t=static_cast<float>(pos-i);
        return {data_[i%data_.size()][firstChannel]*(1-t)+data_[(i+1)%data_.size()][firstChannel]*t,
                data_[i%data_.size()][firstChannel+1]*(1-t)+data_[(i+1)%data_.size()][firstChannel+1]*t};
    }
};
}

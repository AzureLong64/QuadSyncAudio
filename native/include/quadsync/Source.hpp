#pragma once
#include "WasapiEngine.hpp"
#include <atomic>
#include <thread>

namespace quadsync {
class Source {
    std::vector<Frame> wav_;
    std::thread thread_;
    std::atomic<bool> stop_{false},eof_{false};
    std::string mode_="tone";
    bool loop_=true;
public:
    ~Source(){stop();}
    void prepare(const std::string& mode,const std::string& path,bool loop);
    void start(WasapiEngine& engine);
    void stop();
    bool eof()const{return eof_;}
    const std::string& mode()const{return mode_;}
};
}

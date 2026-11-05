#include "quadsync/Source.hpp"
#include <bit>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <numbers>

namespace quadsync { namespace {
uint16_t u16(const unsigned char* p){return static_cast<uint16_t>(p[0]|p[1]<<8);}
uint32_t u32(const unsigned char* p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
std::vector<Frame> loadWav(const std::string& path) {
    std::ifstream file(std::filesystem::path(std::u8string(path.begin(),path.end())),std::ios::binary);
    if(!file)throw std::runtime_error("Cannot open WAV file");
    unsigned char header[12]{};file.read(reinterpret_cast<char*>(header),12);
    if(!file || std::memcmp(header,"RIFF",4) || std::memcmp(header+8,"WAVE",4))throw std::runtime_error("Expected a RIFF/WAVE file");
    std::vector<unsigned char> fmt,data;
    while(file) {
        unsigned char h[8]{};file.read(reinterpret_cast<char*>(h),8);if(file.gcount()!=8)break;
        uint32_t size=u32(h+4);
        if(!std::memcmp(h,"fmt ",4) || !std::memcmp(h,"data",4)) {
            // Validate chunk size against the actual file before allocating.
            auto pos=file.tellg();file.seekg(0,std::ios::end);auto end=file.tellg();file.seekg(pos);
            if(end-pos<static_cast<std::streamoff>(size))throw std::runtime_error("Truncated WAV chunk");
            auto& target=!std::memcmp(h,"fmt ",4)?fmt:data;
            target.resize(size);file.read(reinterpret_cast<char*>(target.data()),size);
        } else file.seekg(size,std::ios::cur);
        if(size&1)file.seekg(1,std::ios::cur);
    }
    if(fmt.size()<16 || data.empty())throw std::runtime_error("WAV has no valid format or audio data");
    unsigned tag=u16(fmt.data()),channels=u16(fmt.data()+2),bits=u16(fmt.data()+14),block=u16(fmt.data()+12);
    if(tag==0xfffe) {
        const unsigned char guidTail[]={0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
        if(fmt.size()<40 || u16(fmt.data()+16)<22 || std::memcmp(fmt.data()+26,guidTail,14))throw std::runtime_error("Unsupported WAV subformat");
        tag=u16(fmt.data()+24);
        if(u16(fmt.data()+18)!=bits)throw std::runtime_error("WAV valid bits must equal container bits");
    }
    if(u32(fmt.data()+4)!=SampleRate || (channels!=2 && channels!=4))throw std::runtime_error("WAV must be 48000 Hz, stereo or four channels");
    if(!((tag==3 && bits==32)||(tag==1 && (bits==16||bits==24||bits==32))))throw std::runtime_error("WAV must be float32 or PCM16/24/32");
    if(block!=channels*bits/8 || data.size()%block)throw std::runtime_error("Invalid WAV block alignment");
    std::vector<Frame> frames(data.size()/block);
    for(size_t n=0;n<frames.size();++n)for(unsigned ch=0;ch<4;++ch) {
        auto p=data.data()+n*block+(ch%channels)*(bits/8);float value=0;
        if(tag==3)value=std::bit_cast<float>(u32(p));
        else if(bits==16)value=static_cast<int16_t>(u16(p))/32768.f;
        else if(bits==24) {int32_t v=p[0]|p[1]<<8|p[2]<<16;if(v&0x800000)v-=0x1000000;value=v/8388608.f;}
        else value=static_cast<int32_t>(u32(p))/2147483648.f;
        frames[n][ch]=std::isfinite(value)?value:0.f;
    }
    return frames;
}
}
void Source::prepare(const std::string& mode,const std::string& path,bool loop) {
    stop();
    if(mode!="tone" && mode!="wav" && mode!="external")throw std::invalid_argument("Source must be tone, wav or external");
    auto data=mode=="wav"?loadWav(path):std::vector<Frame>{};
    wav_=std::move(data);mode_=mode;loop_=loop;eof_=false;
}
void Source::start(WasapiEngine& engine) {
    if(mode_=="external")return;
    stop_=false;
    thread_=std::thread([this,&engine]{
        uint64_t position=0;
        std::array<Frame,256> block{};
        constexpr double frequencies[]={440,550,660,880};
        while(!stop_) {
            if(engine.WritableFrames()<16384-2048) {std::this_thread::sleep_for(std::chrono::milliseconds(2));continue;}
            for(size_t n=0;n<block.size();++n) {
                if(mode_=="tone")for(int ch=0;ch<4;++ch)block[n][ch]=static_cast<float>(0.063095734*std::sin(2*std::numbers::pi*frequencies[ch]*(position+n)/SampleRate));
                else if(loop_)block[n]=wav_[(position+n)%wav_.size()];
                else if(position+n<wav_.size())block[n]=wav_[position+n];
                else {block[n]={};eof_=true;}
            }
            position+=engine.WriteInterleaved(block[0].data(),block.size());
        }
    });
}
void Source::stop(){stop_=true;if(thread_.joinable())thread_.join();}
}

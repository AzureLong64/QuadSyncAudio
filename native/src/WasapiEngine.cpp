#include "quadsync/WasapiEngine.hpp"
#include "quadsync/Ring.hpp"
#include "quadsync/Drift.hpp"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <propvarutil.h>
#include <future>
#include <mutex>
#include <thread>
#include <sstream>
#include <iomanip>

namespace quadsync { namespace {
void check(HRESULT hr,const char* operation) {
    if(FAILED(hr)) {
        std::ostringstream s; s<<operation<<" failed (HRESULT 0x"<<std::hex<<std::uppercase<<static_cast<unsigned long>(hr)<<")";
        if(hr==AUDCLNT_E_UNSUPPORTED_FORMAT) s<<": device does not support exclusive stereo 48000 Hz float32";
        if(hr==AUDCLNT_E_DEVICE_IN_USE) s<<": device is already in use";
        if(hr==AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED) s<<": enable exclusive mode in Windows sound settings";
        if(hr==AUDCLNT_E_DEVICE_INVALIDATED) s<<": device disconnected or reconfigured";
        throw std::runtime_error(s.str());
    }
}
template<class T> struct Com {
    T* p=nullptr;
    ~Com(){reset();}
    Com()=default; Com(const Com&)=delete; Com& operator=(const Com&)=delete;
    void reset(){if(p){p->Release();p=nullptr;}}
    T** put(){reset();return &p;}
    T* operator->() const{return p;}
};
struct Apartment {
    Apartment(){check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"CoInitializeEx");}
    ~Apartment(){CoUninitialize();}
};
struct Handle {
    HANDLE h=nullptr;
    ~Handle(){if(h && h!=INVALID_HANDLE_VALUE) CloseHandle(h);}
};
std::wstring wide(const std::string& s) {
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0);
    if(!n && !s.empty()) throw std::invalid_argument("Invalid UTF-8 device ID");
    std::wstring w(n,0); MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),w.data(),n); return w;
}
std::string utf8(const wchar_t* w) {
    if(!w) return {};
    int n=WideCharToMultiByte(CP_UTF8,0,w,-1,nullptr,0,nullptr,nullptr);
    std::string s(n,0); WideCharToMultiByte(CP_UTF8,0,w,-1,s.data(),n,nullptr,nullptr);s.pop_back();return s;
}
WAVEFORMATEXTENSIBLE format() {
    WAVEFORMATEXTENSIBLE f{};
    f.Format={WAVE_FORMAT_EXTENSIBLE,2,SampleRate,SampleRate*8,8,32,22};
    f.Samples.wValidBitsPerSample=32; f.dwChannelMask=SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT;
    f.SubFormat={3,0,0x0010,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}}; return f;
}
// Some drivers advertise float only via legacy WAVEFORMATEX. Both describe exactly the same PCM.
bool chooseFormat(IAudioClient* client,WAVEFORMATEXTENSIBLE& f) {
    f=format();
    if(client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,&f.Format,nullptr)==S_OK)return true;
    f.Format.wFormatTag=WAVE_FORMAT_IEEE_FLOAT;f.Format.cbSize=0;
    return client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,&f.Format,nullptr)==S_OK;
}
void enumerator(Com<IMMDeviceEnumerator>& e) {
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(e.put())),"Create device enumerator");
}
struct Endpoint {
    Handle event;
    Com<IMMDevice> device;
    Com<IAudioClient> client;
    Com<IAudioRenderClient> render;
    Com<IAudioClock> clock;
    UINT32 frames=0;
    UINT64 frequency=0, submitted=0;
    double cursor=0;
    PlaybackMap mapping;
    ~Endpoint(){if(client.p)client->Stop();}
    void activate(){check(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(client.put())),"Activate endpoint");}
    void open(IMMDeviceEnumerator* e,const std::string& id) {
        check(e->GetDevice(wide(id).c_str(),device.put()),"Get endpoint"); activate();
        WAVEFORMATEXTENSIBLE f{};
        if(!chooseFormat(client.p,f))check(AUDCLNT_E_UNSUPPORTED_FORMAT,"IsFormatSupported");
        REFERENCE_TIME period=0,minimum=0;
        check(client->GetDevicePeriod(&period,&minimum),"GetDevicePeriod");
        period=std::max<REFERENCE_TIME>(minimum,100000); // prefer 10 ms; respect device minimum
        auto hr=client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,period,period,&f.Format,nullptr);
        if(hr==AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            check(client->GetBufferSize(&frames),"Get aligned buffer size");
            period=static_cast<REFERENCE_TIME>(std::llround(10000000.0*frames/SampleRate));
            activate();
            hr=client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,period,period,&f.Format,nullptr);
        }
        check(hr,"Initialize exclusive stream");
        event.h=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!event.h)throw std::runtime_error("Create audio event failed");
        check(client->SetEventHandle(event.h),"SetEventHandle");
        check(client->GetBufferSize(&frames),"GetBufferSize");
        if(frames==0 || frames>8192)throw std::runtime_error("Unsupported device buffer size (expected 1..8192 frames)");
        check(client->GetService(__uuidof(IAudioRenderClient),reinterpret_cast<void**>(render.put())),"Get render service");
        check(client->GetService(__uuidof(IAudioClock),reinterpret_cast<void**>(clock.put())),"Get audio clock");
        check(clock->GetFrequency(&frequency),"Get clock frequency");
        if(!frequency)throw std::runtime_error("Audio clock frequency is zero");
    }
};
struct Mmcss {
    HANDLE task=nullptr;
    Mmcss(){DWORD index=0;task=AvSetMmThreadCharacteristicsW(L"Pro Audio",&index);}
    ~Mmcss(){if(task)AvRevertMmThreadCharacteristics(task);}
};
}

struct WasapiEngine::Impl {
    InputRing<16384> input;
    Handle stop;
    std::thread thread;
    std::atomic<bool> running{false},correcting{false};
    std::array<std::atomic<float>,4> peaks{};
    std::atomic<double> lag{0},ratio{1},bufferLag{0};
    std::atomic<uint64_t> underruns{0},clipped{0};
    std::atomic<uint32_t> bufferA{0},bufferB{0};
    std::mutex errorMutex;
    std::string error;
    Impl(){stop.h=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!stop.h)throw std::runtime_error("Create stop event failed");}
    void run(const std::string& a,const std::string& b,Controls& controls,std::promise<void> ready) {
        bool announced=false;
        try {
            Apartment apartment;
            Com<IMMDeviceEnumerator> e;enumerator(e);
            Endpoint devices[2];devices[0].open(e.p,a);devices[1].open(e.p,b);
            bufferA=devices[0].frames;bufferB=devices[1].frames;
            Timeline timeline; Dsp dsp; DriftController drift;
            Mmcss mmcss;
            auto fill=[&](int index,double step) {
                auto& d=devices[index];
                double needed=d.cursor+(d.frames-1)*step+2;
                if(needed-std::min(devices[0].cursor,devices[1].cursor)>65000)
                    throw std::runtime_error("Devices lost synchronization; restart playback");
                dsp.update(controls);
                while(timeline.end()<static_cast<uint64_t>(std::ceil(needed))) {
                    Frame f{};if(!input.pop(f))++underruns;
                    timeline.append(dsp.process(f));
                }
                BYTE* raw=nullptr;check(d.render->GetBuffer(d.frames,&raw),"GetBuffer");
                auto out=reinterpret_cast<float*>(raw);
                std::array<float,2> peak{};
                const double begin=d.cursor;
                for(UINT32 n=0;n<d.frames;++n) {
                    auto pair=timeline.stereo(d.cursor,index*2);
                    for(int ch=0;ch<2;++ch) {
                        peak[ch]=std::max(peak[ch],std::abs(pair[ch]));
                        if(std::abs(pair[ch])>1)++clipped;
                        out[n*2+ch]=std::clamp(pair[ch],-1.f,1.f);
                    }
                    d.cursor+=step;
                }
                check(d.render->ReleaseBuffer(d.frames,0),"ReleaseBuffer");
                d.mapping.add(static_cast<double>(d.submitted),begin,d.frames,step);d.submitted+=d.frames;
                for(int ch=0;ch<2;++ch) {
                    auto& meter=peaks[index*2+ch];float old=meter.load();
                    while(old<peak[ch] && !meter.compare_exchange_weak(old,peak[ch])){}
                }
            };
            fill(0,1);fill(1,1);
            check(devices[0].client->Start(),"Start device A");
            check(devices[1].client->Start(),"Start device B");
            running=true;ready.set_value();announced=true;
            HANDLE waits[]={stop.h,devices[0].event.h,devices[1].event.h};
            double step=1,lastQpc=0;
            auto sync=[&] {
                UINT64 pos[2]{},qpc[2]{};
                for(int i=0;i<2;++i) {
                    auto hr=devices[i].clock->GetPosition(&pos[i],&qpc[i]);
                    check(hr,"Read audio clock");
                    if(hr!=S_OK || !pos[i])return;
                }
                auto now=std::max(qpc[0],qpc[1]);
                double played[2]{};
                for(int i=0;i<2;++i) {
                    double frames=static_cast<double>(pos[i])*SampleRate/devices[i].frequency
                        +static_cast<double>(now-qpc[i])*SampleRate/10000000.0;
                    if(frames>devices[i].submitted) return; // do not extrapolate beyond queued audio
                    played[i]=devices[i].mapping.sourceAt(frames);
                }
                double dt=lastQpc ? (now-lastQpc)/10000000.0 : devices[1].frames/double(SampleRate);
                lastQpc=static_cast<double>(now);
                step=drift.update(played[0]-played[1],dt);
                lag=drift.lag();ratio=step;correcting=drift.active();
                // Positive means more source frames remain ahead of B's playback point.
                bufferLag=(timeline.end()-played[1])-(timeline.end()-played[0]);
            };
            while(true) {
                DWORD w=WaitForMultipleObjects(3,waits,FALSE,2000);
                if(w==WAIT_OBJECT_0)break;
                if(w==WAIT_TIMEOUT)throw std::runtime_error("Audio event timed out; check device connection");
                if(w!=WAIT_OBJECT_0+1 && w!=WAIT_OBJECT_0+2)throw std::runtime_error("Audio event wait failed");
                int index=static_cast<int>(w-WAIT_OBJECT_0-1);
                if(index==1)sync();
                fill(index,index==0?1:step);
                int other=1-index;
                if(WaitForSingleObject(devices[other].event.h,0)==WAIT_OBJECT_0) {
                    if(other==1)sync();
                    fill(other,other==0?1:step);
                }
            }
        } catch(const std::exception& ex) {
            {std::lock_guard lock(errorMutex);error=ex.what();}
            if(!announced)ready.set_exception(std::current_exception());
        }
        running=false;
    }
};

WasapiEngine::WasapiEngine():p_(std::make_unique<Impl>()){}
WasapiEngine::~WasapiEngine(){Stop();}
std::vector<DeviceInfo> WasapiEngine::EnumerateDevices() {
    Apartment apartment;
    Com<IMMDeviceEnumerator> e;enumerator(e);
    Com<IMMDeviceCollection> list;
    check(e->EnumAudioEndpoints(eRender,DEVICE_STATE_ACTIVE,list.put()),"Enumerate render devices");
    UINT count=0;check(list->GetCount(&count),"Get device count");
    std::vector<DeviceInfo> result;
    for(UINT i=0;i<count;++i) {
        Com<IMMDevice> d;check(list->Item(i,d.put()),"Get device");
        LPWSTR id=nullptr;check(d->GetId(&id),"Get device ID");
        DeviceInfo info;info.id=utf8(id);CoTaskMemFree(id);
        Com<IPropertyStore> props;
        if(SUCCEEDED(d->OpenPropertyStore(STGM_READ,props.put()))) {
            PROPVARIANT v;PropVariantInit(&v);
            if(SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName,&v)) && v.vt==VT_LPWSTR)info.name=utf8(v.pwszVal);
            PropVariantClear(&v);
            if(SUCCEEDED(props->GetValue(PKEY_Device_ContainerId,&v)) && v.vt==VT_CLSID && v.puuid) {
                wchar_t text[40]{};StringFromGUID2(*v.puuid,text,40);info.containerId=utf8(text);
            }
            PropVariantClear(&v);
        }
        Com<IAudioClient> client;
        if(SUCCEEDED(d->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(client.put())))) {
            WAVEFORMATEXTENSIBLE f{};info.float48=chooseFormat(client.p,f);
        }
        if(info.name.empty())info.name=info.id;
        result.push_back(info);
    }
    return result;
}
void WasapiEngine::Start(const std::string& a,const std::string& b) {
    Stop();
    if(a.empty() || b.empty() || a==b)throw std::invalid_argument("Select two different output devices");
    const auto devices=EnumerateDevices();
    auto first=std::find_if(devices.begin(),devices.end(),[&](const DeviceInfo& d){return d.id==a;});
    auto second=std::find_if(devices.begin(),devices.end(),[&](const DeviceInfo& d){return d.id==b;});
    if(first!=devices.end() && second!=devices.end() && !first->containerId.empty()
       && first->containerId!="{00000000-0000-0000-FFFF-FFFFFFFFFFFF}"
       && first->containerId!="{00000000-0000-0000-0000-000000000000}"
       && first->containerId==second->containerId)
        throw std::invalid_argument("Both endpoints belong to the same physical audio device");
    p_->input.reset();p_->lag=0;p_->ratio=1;p_->bufferLag=0;p_->underruns=0;p_->clipped=0;p_->correcting=false;
    for(auto& peak:p_->peaks)peak=0;
    {std::lock_guard lock(p_->errorMutex);p_->error.clear();}
    ResetEvent(p_->stop.h);
    std::promise<void> ready;auto future=ready.get_future();
    p_->thread=std::thread(&Impl::run,p_.get(),a,b,std::ref(controls),std::move(ready));
    try{future.get();}catch(...){Stop();throw;}
}
void WasapiEngine::Stop(){SetEvent(p_->stop.h);if(p_->thread.joinable())p_->thread.join();p_->running=false;}
size_t WasapiEngine::WriteInterleaved(const float* pcm,size_t frames) {
    if(!pcm && frames)throw std::invalid_argument("PCM pointer is null");
    return p_->input.write(pcm,frames);
}
size_t WasapiEngine::WritableFrames()const{return p_->input.writable();}
bool WasapiEngine::Running()const{return p_->running.load();}
Status WasapiEngine::GetStatus() {
    Status s;s.running=p_->running;s.correcting=p_->correcting;s.lagFrames=p_->lag;s.ratio=p_->ratio;s.bufferLagFrames=p_->bufferLag;
    s.underrunFrames=p_->underruns;s.clippedSamples=p_->clipped;s.bufferA=p_->bufferA;s.bufferB=p_->bufferB;
    for(int i=0;i<4;++i)s.peaks[i]=p_->peaks[i].exchange(0);
    {std::lock_guard lock(p_->errorMutex);s.error=p_->error;}return s;
}
}

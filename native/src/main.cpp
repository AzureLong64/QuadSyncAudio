#include "quadsync/WasapiEngine.hpp"
#include "quadsync/Source.hpp"
#include "quadsync/PipeServer.hpp"
#include "json.hpp"
#include <iostream>

using nlohmann::json;
using namespace quadsync;
namespace {
HANDLE stopEvent=nullptr;
BOOL WINAPI consoleHandler(DWORD){if(stopEvent)SetEvent(stopEvent);return TRUE;}
json devices() {
    auto list=json::array();
    for(const auto& d:WasapiEngine::EnumerateDevices())list.push_back({{"id",d.id},{"name",d.name},{"containerId",d.containerId},{"supportsFloat48",d.float48}});
    return list;
}
}
int main(int argc,char** argv) {
    try {
        if(argc>1 && std::string(argv[1])=="--list") {std::cout<<devices().dump(2)<<"\n";return 0;}
        std::wstring pipe=L"QuadSyncAudio.v1";
        if(argc==3 && std::string(argv[1])=="--pipe") {
            std::string value=argv[2];
            if(value.empty() || value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_")!=std::string::npos)throw std::invalid_argument("Invalid pipe name");
            pipe.assign(value.begin(),value.end());
        }
        stopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!stopEvent)throw std::runtime_error("Cannot create shutdown event");
        SetConsoleCtrlHandler(consoleHandler,TRUE);
        WasapiEngine engine; Source source;
        auto dispatch=[&](const std::string& line) {
            json id=nullptr;
            try {
                auto q=json::parse(line);id=q.value("id",json(nullptr));std::string cmd=q.at("command");
                json data=json::object();
                if(cmd=="devices")data["devices"]=devices();
                else if(cmd=="start") {
                    source.stop();engine.Stop();
                    source.prepare(q.value("source",std::string("tone")),q.value("path",std::string()),q.value("loop",true));
                    engine.Start(q.at("deviceA"),q.at("deviceB"));
                    source.start(engine);
                } else if(cmd=="stop") {source.stop();engine.Stop();}
                else if(cmd=="settings") {
                    auto channels=q.at("channels");if(!channels.is_array() || channels.size()!=4)throw std::invalid_argument("Expected four channels");
                    std::array<double,4> gain{},delay{};
                    for(int i=0;i<4;++i) {
                        gain[i]=checked(channels[i].at("gainDb").get<double>(),-60,6);
                        const auto& c=channels[i];
                        delay[i]=c.contains("distanceM")?distanceToMs(c.at("distanceM").get<double>()):checked(c.at("delayMs").get<double>(),0,100);
                    }
                    auto master=checked(q.at("master").get<double>(),0,1);bool mute=q.at("mute").get<bool>();
                    for(int i=0;i<4;++i)engine.controls.setChannel(i,gain[i],delay[i]);
                    engine.controls.master=static_cast<float>(master);engine.controls.mute=mute;
                } else if(cmd=="pcm") {
                    if(source.mode()!="external")throw std::invalid_argument("Start with source=external before submitting PCM");
                    if(!engine.Running())throw std::invalid_argument("Audio outputs must be running before submitting PCM");
                    auto samples=q.at("samples").get<std::vector<float>>();
                    if(samples.size()%4 || samples.size()>4096*4)throw std::invalid_argument("Expected at most 4096 four-channel frames");
                    for(float v:samples)if(!std::isfinite(v))throw std::invalid_argument("PCM samples must be finite");
                    data["acceptedFrames"]=engine.WriteInterleaved(samples.data(),samples.size()/4);
                } else if(cmd=="status") {
                    auto s=engine.GetStatus();
                    data={{"running",s.running},{"correcting",s.correcting},{"peaks",s.peaks},{"lagFrames",s.lagFrames},
                        {"lagMicroseconds",s.lagFrames*1000000/SampleRate},{"ratio",s.ratio},{"bufferLagFrames",s.bufferLagFrames},
                        {"underrunFrames",s.underrunFrames},{"clippedSamples",s.clippedSamples},{"bufferA",s.bufferA},{"bufferB",s.bufferB},
                        {"error",s.error},{"eof",source.eof()},{"writableFrames",engine.WritableFrames()}};
                } else if(cmd=="shutdown") {source.stop();engine.Stop();SetEvent(stopEvent);}
                else throw std::invalid_argument("Unknown command");
                return json({{"id",id},{"ok",true},{"data",data}}).dump();
            }catch(const std::exception& e){return json({{"id",id},{"ok",false},{"error",e.what()}}).dump();}
        };
        std::cout<<"QuadSyncAudio engine ready. Pipe: ";std::wcout<<pipe<<std::endl;
        RunPipeServer(pipe,stopEvent,dispatch);
        source.stop();engine.Stop();SetConsoleCtrlHandler(consoleHandler,FALSE);CloseHandle(stopEvent);stopEvent=nullptr;
        return 0;
    }catch(const std::exception& ex){std::cerr<<ex.what()<<std::endl;return 1;}
}

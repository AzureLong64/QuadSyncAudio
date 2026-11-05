#include "quadsync/Dsp.hpp"
#include "quadsync/Ring.hpp"
#include "quadsync/Drift.hpp"
#include <iostream>
#include <thread>

using namespace quadsync;
void require(bool v,const char* message){if(!v)throw std::runtime_error(message);}
void close(double a,double b,double tolerance,const char* m){require(std::abs(a-b)<tolerance,m);}
int main() {
    try {
        close(distanceToMs(3.43),10,1e-8,"distance conversion");
        close(distanceToMs(34.3)*48,4800,1e-8,"100-ms limit");
        Controls c;c.master=1;
        c.setChannel(0,0,0);c.setChannel(1,-6,1);c.setChannel(2,6,100);c.setChannel(3,-60,0);
        Dsp dsp;dsp.update(c);
        for(int i=0;i<12000;++i)dsp.process({});
        Frame impulse{1,1,1,1};
        for(int i=0;i<=4800;++i) {
            auto out=dsp.process(i==0?impulse:Frame{});
            close(out[0],i==0?1:0,1e-5,"channel 0 impulse");
            close(out[1],i==48?std::pow(10.,-6./20):0,1e-5,"1-ms delayed impulse");
            close(out[2],i==4800?std::pow(10.,6./20):0,1e-5,"100-ms delayed impulse");
            close(out[3],i==0?0.001:0,1e-5,"channel 3 independent gain");
        }
        c.setChannel(0,0,0.1);dsp.update(c);
        for(int i=0;i<6000;++i)dsp.process({});
        for(int i=0;i<8;++i) {
            auto o=dsp.process(i==0?Frame{1,0,0,0}:Frame{});
            close(o[0],i==4?0.2:(i==5?0.8:0),1e-5,"fractional delay interpolation");
        }
        Controls smooth;smooth.master=1;Dsp gain;gain.update(smooth);
        for(int i=0;i<20000;++i)gain.process({1,1,1,1});
        smooth.gainDb[0]=-60;gain.update(smooth);
        auto first=gain.process({1,1,1,1});require(first[0]>0.99,"gain change must be smoothed");
        for(int i=0;i<10000;++i)gain.process({1,1,1,1});
        smooth.master=0.25;gain.update(smooth);
        for(int i=0;i<10000;++i)gain.process({1,1,1,1});
        auto quarter=gain.process({1,1,1,1});close(quarter[1],0.25,1e-5,"linear master");
        smooth.mute=true;gain.update(smooth);
        for(int i=0;i<20000;++i)gain.process({1,1,1,1});
        require(gain.process({1,1,1,1})[1]<1e-8,"mute reaches silence");
        bool invalid=false;try{c.setChannel(4,0,0);}catch(...){invalid=true;}require(invalid,"channel bounds");
        invalid=false;try{distanceToMs(-1);}catch(...){invalid=true;}require(invalid,"distance bounds");

        InputRing<128> ring;
        std::thread producer([&]{for(int i=0;i<50000;++i){Frame f{float(i),float(i+1),float(i+2),float(i+3)};while(ring.write(f.data(),1)==0)std::this_thread::yield();}});
        bool ordered=true;
        for(int i=0;i<50000;++i){Frame f{};while(!ring.pop(f))std::this_thread::yield();for(int c1=0;c1<4;++c1)if(f[c1]!=i+c1)ordered=false;}
        producer.join();require(ordered,"SPSC order/wraparound");require(ring.available()==0,"ring empty");
        Timeline timeline(16);for(int i=0;i<20;++i)timeline.append({float(i),float(i+10),float(i+20),float(i+30)});
        close(timeline.stereo(14.25,0)[0],14.25,1e-6,"A routing");close(timeline.stereo(14.25,2)[1],44.25,1e-6,"B routing + interpolation");
        PlaybackMap map;map.add(0,0,480,1);map.add(480,480,480,1.001);
        close(map.sourceAt(400),400,1e-6,"historical playback ratio");close(map.sourceAt(580),580.1,1e-6,"current playback ratio");

        for(double ppm:{-500.,-100.,100.,500.}) {
            DriftController drift;double lag=0,step=1,maxStep=0;
            // Ten minutes of two independent clocks, 10-ms controller cadence.
            for(int i=0;i<60000;++i) {
                lag+=480-480*(1+ppm*1e-6)*step;
                step=drift.update(lag,0.01);maxStep=std::max(maxStep,std::abs(step-1));
            }
            require(drift.active(),"64-frame trigger activates");
            require(std::abs(lag)<2,"drift convergence under +/-500 ppm");
            require(maxStep<=0.002001,"ratio bounded");
            std::cout<<"Clock "<<ppm<<" ppm: final lag "<<lag<<" frames, ratio "<<step<<"\n";
        }
        DriftController quiet;for(int i=0;i<500;++i)close(quiet.update(20,0.01),1,1e-9,"below-threshold drift stays idle");
        std::cout<<"All DSP, routing, SPSC, playback-map and drift tests passed.\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}

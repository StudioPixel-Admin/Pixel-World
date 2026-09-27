#include "pixel/Audio.hpp"
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#include <miniaudio.h>
#include <atomic>
#include <cmath>
#include <algorithm>

namespace pixel {
struct Audio::Impl {
    ma_device device{};
    bool ready=false;
    std::atomic<float> master{.65f},ambience{.5f},effects{.7f},music{.25f},daylight{1},eventPitch{0};
    std::atomic<bool> playing{false},moving{false};
    double phase=0;
    float filtered=0,eventEnvelope=0,pitch=1;
    uint32_t random=42973;
    static void callback(ma_device* device,void* output,const void*,ma_uint32 count) {
        auto& s=*static_cast<Impl*>(device->pUserData);
        auto* samples=static_cast<float*>(output);
        const float m=s.master.load(),amb=s.ambience.load(),fx=s.effects.load(),music=s.music.load();
        const bool playing=s.playing.load(),moving=s.moving.load();
        const float day=s.daylight.load();
        if (const float p=s.eventPitch.exchange(0);p>0) {s.pitch=p;s.eventEnvelope=1;}
        for(ma_uint32 i=0;i<count;++i) {
            s.phase+=1.0/48000;
            s.random=s.random*1664525u+1013904223u;
            float noise=static_cast<float>(s.random>>8)*(2.0f/16777216.0f)-1;
            s.filtered=.985f*s.filtered+.015f*noise;
            const double t=s.phase;
            const float wind=s.filtered*.28f*(.7f+.3f*static_cast<float>(std::sin(t*.19)));
            const float chirpGate=std::pow(std::max(0.0f,static_cast<float>(std::sin(t*.71))),28.0f);
            const float bird=chirpGate*.018f*static_cast<float>(std::sin(t*(2400+180*std::sin(t*13))));
            const float foot= moving ? std::pow(std::max(0.0f,static_cast<float>(std::sin(t*17))),22.0f)*noise*.075f : 0;
            const float pad=.012f*static_cast<float>(std::sin(t*164.81*6.2831853)+.4*std::sin(t*246.94*6.2831853));
            const float click=s.eventEnvelope*.07f*static_cast<float>(std::sin(t*720*s.pitch));
            s.eventEnvelope*=.9990f;
            const float sample=m*((playing ? (wind+bird*day)*amb+foot*fx:wind*amb*.25f)+pad*music+click*fx);
            samples[i*2]=std::clamp(sample,-.7f,.7f); samples[i*2+1]=std::clamp(sample*.97f,-.7f,.7f);
        }
    }
};
Audio::Audio():impl_(std::make_unique<Impl>()) {
    auto config=ma_device_config_init(ma_device_type_playback);
    config.playback.format=ma_format_f32;config.playback.channels=2;config.sampleRate=48000;
    config.dataCallback=Impl::callback;config.pUserData=impl_.get();
    if(ma_device_init(nullptr,&config,&impl_->device)==MA_SUCCESS) {
        if(ma_device_start(&impl_->device)==MA_SUCCESS)impl_->ready=true;
        else ma_device_uninit(&impl_->device);
    }
}
Audio::~Audio(){if(impl_->ready)ma_device_uninit(&impl_->device);}
void Audio::setVolumes(float m,float a,float e,float u){impl_->master=m;impl_->ambience=a;impl_->effects=e;impl_->music=u;}
void Audio::setEnvironment(bool p,float d,bool m){impl_->playing=p;impl_->daylight=d;impl_->moving=m;}
void Audio::trigger(float pitch){impl_->eventPitch=pitch;}
bool Audio::available()const{return impl_->ready;}
}

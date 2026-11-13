/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Echo.h"
#include "Quiet.h"
#include "miniaudio.h"
#include <api/audio/builtin_audio_processing_builder.h>
#include <api/environment/environment_factory.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <new>

// Render writes a preallocated single-producer ring. Run both WebRTC entry
// points on capture's thread; speaker callbacks never enter WebRTC's locks.
struct rq_echo {
    webrtc::scoped_refptr<webrtc::AudioProcessing> apm;
    webrtc::StreamConfig format{RQ_RATE,1};
    std::atomic<bool> have_render{false};
    ma_pcm_rb render{};
    std::array<float,RQ_FRAME> frame{};
    rq_echo() {
        webrtc::AudioProcessing::Config cfg;
        cfg.echo_canceller.enabled=true;
        apm=webrtc::BuiltinAudioProcessingBuilder(cfg).Build(webrtc::CreateEnvironment());
    }
};
rq_echo *rq_echo_open() {
    try {
        auto echo=std::make_unique<rq_echo>();
        if (!echo->apm || ma_pcm_rb_init(ma_format_f32,1,12*RQ_FRAME,nullptr,nullptr,&echo->render)!=MA_SUCCESS)
            return nullptr;
        // Prime allocation and state before enabling the capture stream.
        const float *in[]={echo->frame.data()};
        float *out[]={echo->frame.data()};
        echo->apm->AnalyzeReverseStream(in,echo->format);
        echo->apm->set_stream_delay_ms(20);
        echo->apm->ProcessStream(in,echo->format,echo->format,out);
        return echo.release();
    } catch (...) { return nullptr; }
}
void rq_echo_close(rq_echo *echo) {
    if (!echo) return;
    ma_pcm_rb_uninit(&echo->render);
    delete echo;
}
void rq_echo_render(rq_echo *echo, const float *audio, unsigned frames, unsigned channels) {
    if (!echo || !channels || ma_pcm_rb_available_write(&echo->render)<frames) return;
    bool nonzero=false;
    for (unsigned done=0; done<frames;) {
        ma_uint32 n=frames-done;
        void *memory;
        if (ma_pcm_rb_acquire_write(&echo->render,&n,&memory)!=MA_SUCCESS || !n) break;
        float *dst=static_cast<float *>(memory);
        for (unsigned i=0;i<n;++i) {
            dst[i]=audio[(done+i)*channels];
            nonzero |= dst[i]!=0.f;
        }
        ma_pcm_rb_commit_write(&echo->render,n);
        done+=n;
    }
    if (nonzero) echo->have_render.store(true,std::memory_order_release);
}
bool rq_echo_capture(rq_echo *echo, float *mono) {
    if (!echo) return false;
    bool active=echo->have_render.load(std::memory_order_acquire);
    if (ma_pcm_rb_available_read(&echo->render)>=RQ_FRAME) {
        // Keep the reference recent after a stall. AEC3 aligns the acoustic path.
        unsigned discard=ma_pcm_rb_available_read(&echo->render);
        discard=discard>5*RQ_FRAME ? discard-3*RQ_FRAME : 0;
        for (unsigned total=discard+RQ_FRAME, done=0;done<total;) {
            ma_uint32 n=total-done;
            void *src;
            if (ma_pcm_rb_acquire_read(&echo->render,&n,&src)!=MA_SUCCESS || !n) return false;
            auto *p=static_cast<const float *>(src);
            for (unsigned i=0;i<n;++i) if (done+i>=discard) echo->frame[done+i-discard]=p[i];
            ma_pcm_rb_commit_read(&echo->render,n);
            done+=n;
        }
    } else {
        echo->frame.fill(0.f);
    }
    if (!active) return false;
    const float *ref[]={echo->frame.data()};
    const float *in[]={mono};
    float *out[]={mono};
    echo->apm->AnalyzeReverseStream(ref,echo->format);
    return echo->apm->ProcessStream(in,echo->format,echo->format,out)==0;
}

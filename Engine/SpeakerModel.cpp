/* SPDX-License-Identifier: GPL-3.0-or-later
 * Streaming overlap-add follows VoxLock 1.3 (Arbaz Siddiqui, Apache-2.0).
 */
#include "SpeakerModel.h"
#include <onnxruntime_cxx_api.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr size_t fft = 512, hop = 128, pad = fft / 2, sample = 48000;

unsigned read32(const unsigned char *p) {
    return p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}
std::vector<float> readVoice(const unsigned char *bytes, size_t size) {
    if (!bytes || size < 44 || size > 2'000'000 || memcmp(bytes, "RIFF", 4) || memcmp(bytes+8, "WAVE", 4))
        throw std::runtime_error("Invalid voice sample");
    unsigned rate = 0;
    std::vector<float> voice;
    for (size_t i = 12; i + 8 <= size;) {
        auto length = (size_t)read32(bytes+i+4);
        if (length > size-i-8) break;
        const auto *p = bytes+i+8;
        if (!memcmp(bytes+i, "fmt ", 4) && length >= 16) {
            if (p[0] != 1 || p[1] || p[2] != 1 || p[3] || p[14] != 16 || p[15])
                throw std::runtime_error("Voice sample needs mono PCM16");
            rate = read32(p+4);
        } else if (!memcmp(bytes+i, "data", 4)) {
            if (rate != 16000 && rate != 48000) throw std::runtime_error("Voice sample needs 16 or 48 kHz");
            for (size_t j = 0; j+1<length; j+=2)
                voice.push_back((int16_t)((unsigned)p[j] | (unsigned)p[j+1]<<8) / 32768.f);
            break;
        }
        i += 8 + length + (length & 1);
    }
    if (rate == 48000) {
        // Windowed sinc, cutoff below 8 kHz before decimating by three.
        constexpr int half=24;
        std::array<double,2*half+1> filter{};
        for (int k=-half;k<=half;++k) {
            double x = (2*std::acos(-1.)*7000/48000)*k;
            filter[k+half]=(k ? std::sin(x)/(std::acos(-1.)*k) : 14000./48000)
                           *(0.5+0.5*std::cos(std::acos(-1.)*k/(half+1)));
        }
        double gain=std::accumulate(filter.begin(),filter.end(),0.0);
        std::vector<float> reduced(voice.size()/3);
        for (size_t j=0;j<reduced.size();++j) {
            double v=0;
            for (int k=-half;k<=half;++k) {
                auto at=(ptrdiff_t)(3*j)+k;
                if (at>=0 && (size_t)at<voice.size()) v+=voice[(size_t)at]*filter[k+half];
            }
            reduced[j]=(float)(v/gain);
        }
        voice=std::move(reduced);
    }
    if (voice.size() < sample) throw std::runtime_error("Voice sample is too short");
    size_t best = 0;
    double sum = 0;
    for (size_t i=0; i<sample; ++i) sum += voice[i]*voice[i];
    double max = sum;
    for (size_t i=sample; i<voice.size(); ++i) {
        sum += voice[i]*voice[i] - voice[i-sample]*voice[i-sample];
        if (sum > max) { max=sum; best=i-sample+1; }
    }
    if (max < 0.05) throw std::runtime_error("Voice sample is too quiet");
    return {voice.begin()+(ptrdiff_t)best, voice.begin()+(ptrdiff_t)(best+sample)};
}
Ort::Env &env() { static Ort::Env instance(ORT_LOGGING_LEVEL_ERROR, "RalpQuiet"); return instance; }
Ort::SessionOptions options() {
    Ort::SessionOptions result;
    result.SetIntraOpNumThreads(1);
    result.SetInterOpNumThreads(1);
    result.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    return result;
}
}

struct rq_speaker_model {
    Ort::Session model;
    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::vector<Ort::Value> cue;
    std::vector<int64_t> shape;
    std::vector<float> h, c, buffer, accum, divisor;
    std::array<float,fft> win{};
    size_t consumed=0, received=0, skip=pad;
    bool ready=false;

    rq_speaker_model(const std::string &models, const unsigned char *voice, size_t voice_bytes)
        : model(env(), (models+"/chunk_int8.onnx").c_str(), options()) {
        auto samples = readVoice(voice, voice_bytes);
        Ort::Session enroll(env(), (models+"/enroll.onnx").c_str(), options());
        const int64_t dims[] = {1,(int64_t)samples.size()};
        const char *input[] = {"enroll"}, *output[] = {"enroll_n","k","v"};
        auto pcm = Ort::Value::CreateTensor<float>(memory, samples.data(), samples.size(), dims, 2);
        cue = enroll.Run(Ort::RunOptions{nullptr}, input, &pcm, 1, output, 3);
        shape = model.GetInputTypeInfo(4).GetTensorTypeAndShapeInfo().GetShape();
        size_t states=1;
        for (auto d: shape) {
            if (d <= 0 || d > 1024 || states > 262144/(size_t)d) throw std::runtime_error("Unsupported state shape");
            states *= (size_t)d;
        }
        h.resize(states); c.resize(states);
        for (size_t i=0;i<fft;++i) {
            double w=0.5-0.5*std::cos(2*std::acos(-1.)*i/fft);
            win[i]=(float)(w*w);
        }
        reset();
    }
    void reset() {
        std::fill(h.begin(),h.end(),0.f);
        std::fill(c.begin(),c.end(),0.f);
        buffer.clear(); accum.assign(fft,0.f); divisor.assign(fft,0.f);
        consumed=received=0; skip=pad; ready=false;
    }
    Ort::Value view(Ort::Value &value) {
        auto dims=value.GetTensorTypeAndShapeInfo().GetShape();
        return Ort::Value::CreateTensor<float>(memory, value.GetTensorMutableData<float>(),
                    value.GetTensorTypeAndShapeInfo().GetElementCount(), dims.data(), dims.size());
    }
    size_t run(const float *in, size_t count, float *out, size_t capacity, bool finish) {
        if (count) buffer.insert(buffer.end(), in, in+count);
        received+=count;
        if (!ready) {
            if (buffer.size() <= pad && !finish) return 0;
            if (buffer.empty()) return 0;
            std::vector<float> head(pad);
            for (size_t i=0;i<pad;++i) head[i]=buffer[std::min(pad-i,buffer.size()-1)];
            buffer.insert(buffer.begin(),head.begin(),head.end());
            ready=true;
        }
        if (finish) {
            std::array<float,pad> tail{};
            for (size_t i=0;i<pad && i+1<buffer.size();++i) tail[i]=buffer[buffer.size()-i-2];
            buffer.insert(buffer.end(),tail.begin(),tail.end());
        }
        size_t written=0;
        auto emit=[&](size_t n) {
            for (size_t i=0;i<n && consumed<received;++i) {
                if (skip) { --skip; continue; }
                if (written>=capacity) throw std::runtime_error("Output capacity exceeded");
                out[written++] = divisor[i]>1e-11f ? -0.5f*accum[i]/divisor[i] : 0.f;
                ++consumed;
            }
        };
        while (buffer.size()>=fft) {
            size_t nframes=std::min<size_t>(100, 1+(buffer.size()-fft)/hop);
            int64_t dims[]={1,(int64_t)((nframes-1)*hop+fft)};
            std::array<Ort::Value,6> values{
                Ort::Value::CreateTensor<float>(memory,buffer.data(),(size_t)dims[1],dims,2),
                view(cue[0]),view(cue[1]),view(cue[2]),
                Ort::Value::CreateTensor<float>(memory,h.data(),h.size(),shape.data(),shape.size()),
                Ort::Value::CreateTensor<float>(memory,c.data(),c.size(),shape.data(),shape.size())
            };
            const char *names[]={"wav","enroll_n","k","v","h","c"};
            const char *outputs[]={"frames","h_out","c_out"};
            auto result=model.Run(Ort::RunOptions{nullptr},names,values.data(),values.size(),outputs,3);
            if (result[0].GetTensorTypeAndShapeInfo().GetElementCount()!=nframes*fft ||
                result[1].GetTensorTypeAndShapeInfo().GetElementCount()!=h.size() ||
                result[2].GetTensorTypeAndShapeInfo().GetElementCount()!=c.size())
                throw std::runtime_error("Unexpected inference output shape");
            std::copy_n(result[1].GetTensorData<float>(),h.size(),h.begin());
            std::copy_n(result[2].GetTensorData<float>(),c.size(),c.begin());
            const float *frames=result[0].GetTensorData<float>();
            size_t done=nframes*hop;
            accum.resize(done+fft); divisor.resize(done+fft);
            for (size_t j=0;j<nframes;++j) for (size_t i=0;i<fft;++i) {
                accum[j*hop+i]+=frames[j*fft+i];
                divisor[j*hop+i]+=win[i];
            }
            emit(done);
            accum.erase(accum.begin(),accum.begin()+(ptrdiff_t)done);
            divisor.erase(divisor.begin(),divisor.begin()+(ptrdiff_t)done);
            buffer.erase(buffer.begin(),buffer.begin()+(ptrdiff_t)done);
        }
        if (finish) emit(std::min(fft,received-consumed));
        return written;
    }
};

rq_speaker_model *rq_speaker_open(const char *models, const unsigned char *voice, size_t voice_bytes) {
    try { return new rq_speaker_model(models,voice,voice_bytes); }
    catch (const std::exception &e) { fprintf(stderr,"RalpQuiet model setup: %s\n", e.what()); return nullptr; }
}
void rq_speaker_close(rq_speaker_model *m) { delete m; }
void rq_speaker_reset(rq_speaker_model *m) { m->reset(); }
int rq_speaker_run(rq_speaker_model *m,const float *in,size_t n,float *out,size_t cap,bool finish) {
    try { return (int)m->run(in,n,out,cap,finish); }
    catch (const std::exception &e) { fprintf(stderr,"RalpQuiet inference: %s\n",e.what()); return -1; }
}

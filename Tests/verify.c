/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Quiet.h"
#include "../Engine/Isolation.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned seed = 0x10203040;
static float uniform(void) {
    seed = 1664525u * seed + 1013904223u;
    return (float)(seed >> 8) / 8388608.f - 1.f;
}
static double ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return 1000. * t.tv_sec + 1.e-6 * t.tv_nsec;
}
static int devices(void) {
    rq_device list[128];
    for (int in = 0; in <= 1; ++in) {
        size_t n = rq_devices(in, list, 128);
        for (size_t i = 0; i < n; ++i) printf("%s\t%s\t%s\n", in ? "input" : "output", list[i].uid, list[i].name);
    }
    return 0;
}
static int filter_raw(const char *src, const char *dst) {
    if (strcmp(src, dst) == 0) return 2;
    FILE *in = fopen(src, "rb");
    if (!in) return 2;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return 2; }
    rq_filter *filter = rq_filter_create();
    if (!filter) { fclose(in); fclose(out); return 2; }
    float frame[RQ_FRAME];
    size_t total = 0, written = 0, delay = 2 * RQ_FRAME, processed = 0;
    int result = 0;
    for (;;) {
        size_t n = fread(frame, sizeof(float), RQ_FRAME, in);
        total += n;
        if (!n && written == total) break;
        memset(frame + n, 0, (RQ_FRAME - n) * sizeof(float));
        rq_filter_frame(filter, frame, frame);
        processed += RQ_FRAME;
        if (processed <= delay) continue;
        size_t nw = total - written < RQ_FRAME ? total - written : RQ_FRAME;
        if (fwrite(frame, sizeof(float), nw, out) != nw) { result = 2; break; }
        written += nw;
    }
    if (ferror(in)) result = 2;
    if (fclose(out) != 0) result = 2;
    fclose(in);
    rq_filter_destroy(filter);
    return result;
}
static unsigned char *read_profile(const char *path, size_t *count) {
    *count=0;
    FILE *file=fopen(path,"rb");
    if (!file) return NULL;
    if (fseek(file,0,SEEK_END)) { fclose(file); return NULL; }
    long bytes=ftell(file);
    rewind(file);
    if (bytes<44 || bytes>2000000) { fclose(file); return NULL; }
    unsigned char *data=malloc((size_t)bytes);
    if (data && fread(data,1,(size_t)bytes,file)!=(size_t)bytes) { free(data); data=NULL; }
    fclose(file);
    if (data) *count=(size_t)bytes;
    return data;
}
static int isolate(const char *models, const char *profile, const char *src, const char *dst) {
    if (!strcmp(src, dst)) return 2;
    FILE *in = fopen(src, "rb");
    if (!in) return 2;
    if (fseek(in, 0, SEEK_END)) { fclose(in); return 2; }
    long bytes = ftell(in);
    rewind(in);
    if (bytes <= 0 || bytes % sizeof(float) || bytes > 60 * RQ_RATE * (long)sizeof(float)) {
        fclose(in);
        return 2;
    }
    unsigned n = (unsigned)(bytes / (long)sizeof(float));
    float *input = malloc((size_t)bytes), *output = malloc((size_t)bytes);
    int rc = 2;
    if (input && output && fread(input, sizeof(float), n, in) == n) {
        double start = ms();
        size_t voice_bytes;
        unsigned char *voice=read_profile(profile,&voice_bytes);
        bool ok=voice && rq_personalized_compare(models,voice,voice_bytes,input,n,output);
        free(voice);
        if (ok) {
            printf("VoxLock: %.2f s processed in %.2f s (includes model load and enrollment)\n",
                   (double)n / RQ_RATE, (ms() - start) / 1000);
            FILE *out = fopen(dst, "wb");
            if (out) {
                size_t written = fwrite(output, sizeof(float), n, out);
                if (fclose(out) == 0 && written == n) rc = 0;
            }
        }
    }
    fclose(in);
    free(input); free(output);
    return rc;
}
static int timed_stream(const char *models, const char *profile, const char *src) {
    FILE *file=fopen(src,"rb");
    if (!file) return 2;
    size_t voice_bytes;
    unsigned char *voice=read_profile(profile,&voice_bytes);
    rq_isolation *stream=voice ? rq_isolation_create(models,voice,voice_bytes) : NULL;
    free(voice);
    if (!stream) { fclose(file); fprintf(stderr,"Voice isolation startup speed check failed\n"); return 2; }
    float frame[RQ_FRAME], out[RQ_FRAME];
    unsigned n=0, gaps=0, ready=0, overruns=0;
    bool primed=false;
    double due=ms(), sum=0, start_delay=0;
    while (fread(frame,sizeof(float),RQ_FRAME,file)==RQ_FRAME) {
        if (!rq_isolation_push(stream,frame,RQ_FRAME)) break;
        ++n;
        if (!primed && rq_isolation_available(stream)>=RQ_FRAME*RQ_ISOLATION_PRIME_FRAMES) {
            primed=true;
            start_delay=10.*n;
        }
        if (primed) {
            unsigned available=rq_isolation_available(stream);
            if (available>RQ_FRAME*RQ_ISOLATION_HIGH_FRAMES) ++overruns;
            unsigned samples=rq_isolation_pop(stream,out,RQ_FRAME);
            ready+=samples;
            if (samples<RQ_FRAME) ++gaps;
            for (unsigned i=0;i<samples;++i) sum+=out[i]*out[i];
        }
        due+=10.;
        double pause=due-ms();
        if (pause>0) {
            struct timespec wait={0,(long)(pause*1.e6)};
            nanosleep(&wait,NULL);
        }
    }
    bool failed=rq_isolation_failed(stream);
    rq_isolation_destroy(stream);
    fclose(file);
    printf("Paced worker: %.2f s input, first buffered output at %.0f ms, %u underruns, %u queue peaks, %u delivered frames\n",
           n/100.,start_delay,gaps,overruns,ready/RQ_FRAME);
    return !(n>=500 && primed && gaps==0 && !failed && overruns==0 && sum>0.001);
}
static int echo_test(void) {
    rq_echo *echo = rq_echo_open();
    if (!echo) return 2;
    enum { FRAMES = 1500, HISTORY = 4800, DELAY = 1100 };
    float line[HISTORY] = {0}, far[RQ_FRAME], mic[RQ_FRAME];
    double before = 0, after = 0, near_before = 0, near_after = 0, dot = 0;
    float correlated = 0;
    for (int f=0; f<FRAMES; ++f) {
        bool double_talk = f >= 1100;
        for (int i=0; i<RQ_FRAME; ++i) {
            int j=f*RQ_FRAME+i;
            correlated=0.83f*correlated+0.17f*uniform();
            far[i]=0.5f*correlated;
            line[j%HISTORY]=far[i];
            float bounce= j>=DELAY+190 ? line[(j-DELAY)%HISTORY]*0.56f + line[(j-DELAY-190)%HISTORY]*0.17f : 0;
            float near=double_talk ? 0.08f*sinf((float)(j%48000)*(2.f*3.14159265f*511.f/48000.f)) : 0;
            mic[i]=bounce+near;
            if (f>=650 && f<1100) before+=mic[i]*mic[i];
        }
        rq_echo_render(echo,far,RQ_FRAME,1);
        if (!rq_echo_capture(echo,mic)) { rq_echo_close(echo); return 2; }
        for (int i=0; i<RQ_FRAME; ++i) {
            if (f>=650 && f<1100) after+=mic[i]*mic[i];
            if (f>=1300) {
                int j=f*RQ_FRAME+i;
                float near=0.08f*sinf((float)(j%48000)*(2.f*3.14159265f*511.f/48000.f));
                near_before+=near*near; near_after+=mic[i]*mic[i]; dot+=near*mic[i];
            }
        }
    }
    rq_echo_close(echo);
    double reduction=10*log10(before/after), gain=20*log10(fabs(dot)/near_before);
    double correlation=fabs(dot)/sqrt(near_before*near_after);
    printf("AEC3: %.1f dB synthetic echo reduction; local tone change %+.1f dB (correlation %.3f)\n",
           reduction,gain,correlation);
    return !(reduction >= 12 && gain >= -3 && gain <= 3 && correlation >= .70);
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--devices")) return devices();
    if (argc == 4 && !strcmp(argv[1], "--raw")) return filter_raw(argv[2], argv[3]);
    if (argc == 6 && !strcmp(argv[1], "--isolate")) return isolate(argv[2], argv[3], argv[4], argv[5]);
    if (argc == 5 && !strcmp(argv[1], "--stream")) return timed_stream(argv[2], argv[3], argv[4]);
    if (argc == 2 && !strcmp(argv[1], "--echo")) return echo_test();
    if (argc != 1) { fprintf(stderr, "usage: rq-verify [--devices | --raw input.f32 output.f32 | --isolate models voice.wav input.f32 output.f32 | --stream models voice.wav input.f32 | --echo]\n"); return 2; }
    rq_filter *a = rq_filter_create(), *b = rq_filter_create();
    if (!a || !b) return 1;
    float x[RQ_FRAME] = {0}, y[RQ_FRAME], z[RQ_FRAME];
    rq_filter_frame(a, x, y);
    for (unsigned i = 0; i < RQ_FRAME; ++i) if (fabsf(y[i]) > 1e-6) return 1;
    rq_filter_destroy(a);
    a = rq_filter_create();
    if (!a) return 1;
    for (unsigned i = 0; i < RQ_FRAME; ++i) x[i] = 0.14f * uniform();
    x[0] = NAN; x[1] = INFINITY;
    rq_filter_frame(a, x, y); rq_filter_frame(b, x, z);
    if (memcmp(y, z, sizeof y)) return 1;
    rq_filter_destroy(b);
    double before = 0, after = 0, start = ms();
    for (unsigned f = 0; f < 1200; ++f) {
        for (unsigned i = 0; i < RQ_FRAME; ++i) x[i] = 0.14f * uniform();
        rq_filter_frame(a, x, y);
        for (unsigned i = 0; i < RQ_FRAME; ++i) {
            if (!isfinite(y[i]) || fabsf(y[i]) > 1) return 1;
            if (f >= 100) { before += x[i] * x[i]; after += y[i] * y[i]; }
        }
    }
    double average = (ms() - start) / 1200;
    double reduction = 10 * log10(before / after);
    printf("12 s synthetic white noise: %.1f dB RMS reduction, %.3f ms per 10 ms block\n", reduction, average);
    rq_filter_destroy(a);
    return reduction < 20 || average >= 10;
}

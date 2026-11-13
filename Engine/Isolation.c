/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Quiet.h"
#include "Isolation.h"
#include "miniaudio.h"
#include "sherpa-v2.0.1.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { BLOCK = 8 * RQ_FRAME, RING = 32 * RQ_FRAME, VOX_RATE = 16000 };
struct rq_isolation {
    const SherpaOnnxOnlineSpeechDenoiser *model;
    ma_pcm_rb in, out;
    ma_resampler up;
    pthread_t worker;
    bool in_ok, out_ok, up_ok, thread_ok;
    atomic_bool quit, failed;
};

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return 1000. * ts.tv_sec + 1.e-6 * ts.tv_nsec;
}
static const SherpaOnnxOnlineSpeechDenoiser *make_model(const char *models, const char *sample) {
    char enroll[4096], chunk[4096];
    if (!models || !sample || access(sample, R_OK) != 0) return NULL;
    if (snprintf(enroll, sizeof enroll, "%s/voxlock_enroll.onnx", models) >= (int)sizeof enroll ||
        snprintf(chunk, sizeof chunk, "%s/voxlock_chunk.onnx", models) >= (int)sizeof chunk ||
        access(enroll, R_OK) != 0 || access(chunk, R_OK) != 0) return NULL;
    // v2.0.1 accepts enrollment as WAV. Validate its length so isolation cannot
    // silently fall back to capturing training speech from the live call.
    const SherpaOnnxWave *wave = SherpaOnnxReadWave(sample);
    if (!wave) return NULL;
    bool long_enough = wave->sample_rate > 0 && wave->num_samples >= 3 * (int64_t)wave->sample_rate;
    SherpaOnnxFreeWave(wave);
    if (!long_enough) return NULL;
    SherpaOnnxOnlineSpeechDenoiserConfig cfg = {0};
    cfg.model.voxlock.enroll_model = enroll;
    cfg.model.voxlock.chunk_model = chunk;
    cfg.model.voxlock.voice_sample = sample;
    cfg.model.num_threads = 2;
    cfg.model.provider = "cpu";
    return SherpaOnnxCreateOnlineSpeechDenoiser(&cfg);
}

static unsigned read_ring(ma_pcm_rb *rb, float *dst, unsigned n) {
    unsigned done = 0;
    while (done < n) {
        ma_uint32 k = n - done;
        void *src;
        if (ma_pcm_rb_acquire_read(rb, &k, &src) != MA_SUCCESS || !k) break;
        memcpy(dst + done, src, k * sizeof(float));
        ma_pcm_rb_commit_read(rb, k);
        done += k;
    }
    return done;
}
static bool write_ring(ma_pcm_rb *rb, const float *src, unsigned n) {
    if (ma_pcm_rb_available_write(rb) < n) return false;
    while (n) {
        ma_uint32 k = n;
        void *dst;
        if (ma_pcm_rb_acquire_write(rb, &k, &dst) != MA_SUCCESS || !k) return false;
        memcpy(dst, src, k * sizeof(float));
        ma_pcm_rb_commit_write(rb, k);
        src += k;
        n -= k;
    }
    return true;
}
static bool init_up(ma_resampler *up) {
    ma_resampler_config conf = ma_resampler_config_init(ma_format_f32, 1, VOX_RATE, RQ_RATE, ma_resample_algorithm_linear);
    return ma_resampler_init(&conf, NULL, up) == MA_SUCCESS;
}
static unsigned expand(ma_resampler *up, const SherpaOnnxDenoisedAudio *audio, float *out, unsigned limit) {
    if (!audio || audio->n <= 0 || audio->sample_rate != VOX_RATE) return 0;
    ma_uint64 input = (ma_uint64)audio->n, output = limit;
    if (ma_resampler_process_pcm_frames(up, audio->samples, &input, out, &output) != MA_SUCCESS ||
        input != (ma_uint64)audio->n) return 0;
    for (unsigned i = 0; i < output; ++i)
        out[i] = isfinite(out[i]) ? fminf(1, fmaxf(-1, out[i])) : 0;
    return (unsigned)output;
}
static void *isolation_thread(void *opaque) {
    rq_isolation *s = opaque;
    float in[BLOCK], out[BLOCK * 2];
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
    while (!atomic_load_explicit(&s->quit, memory_order_relaxed)) {
        if (ma_pcm_rb_available_read(&s->in) < BLOCK) {
            const struct timespec interval = {0, 2000000};
            nanosleep(&interval, NULL);
            continue;
        }
        unsigned read = read_ring(&s->in, in, BLOCK);
        if (read != BLOCK) break;
        const SherpaOnnxDenoisedAudio *audio = SherpaOnnxOnlineSpeechDenoiserRun(s->model, in, BLOCK, RQ_RATE);
        unsigned n = expand(&s->up, audio, out, BLOCK * 2);
        SherpaOnnxDestroyDenoisedAudio(audio);
        if (n && !write_ring(&s->out, out, n)) break;
    }
    atomic_store_explicit(&s->failed, true, memory_order_release);
    return NULL;
}
static bool can_stream(const SherpaOnnxOnlineSpeechDenoiser *model) {
    float silence[BLOCK] = {0};
    double total = 0;
    for (int i = 0; i < 5; ++i) {
        double begin = now_ms();
        const SherpaOnnxDenoisedAudio *audio = SherpaOnnxOnlineSpeechDenoiserRun(model, silence, BLOCK, RQ_RATE);
        SherpaOnnxDestroyDenoisedAudio(audio);
        if (i > 0) total += now_ms() - begin;
        if (total > 4 * (1000. * BLOCK / RQ_RATE) * 0.90) return false;
    }
    SherpaOnnxOnlineSpeechDenoiserReset(model);
    return true;
}
rq_isolation *rq_isolation_create(const char *models, const char *sample) {
    rq_isolation *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    atomic_init(&s->quit, false);
    atomic_init(&s->failed, false);
    s->model = make_model(models, sample);
    if (!s->model || !can_stream(s->model)) goto fail;
    s->in_ok = ma_pcm_rb_init(ma_format_f32, 1, RING, NULL, NULL, &s->in) == MA_SUCCESS;
    if (!s->in_ok) goto fail;
    s->out_ok = ma_pcm_rb_init(ma_format_f32, 1, RING, NULL, NULL, &s->out) == MA_SUCCESS;
    if (!s->out_ok) goto fail;
    s->up_ok = init_up(&s->up);
    if (!s->up_ok) goto fail;
    s->thread_ok = pthread_create(&s->worker, NULL, isolation_thread, s) == 0;
    if (s->thread_ok) return s;
fail:
    rq_isolation_destroy(s);
    return NULL;
}
void rq_isolation_destroy(rq_isolation *s) {
    if (!s) return;
    atomic_store_explicit(&s->quit, true, memory_order_relaxed);
    if (s->thread_ok) pthread_join(s->worker, NULL);
    if (s->in_ok) ma_pcm_rb_uninit(&s->in);
    if (s->out_ok) ma_pcm_rb_uninit(&s->out);
    if (s->up_ok) ma_resampler_uninit(&s->up, NULL);
    if (s->model) SherpaOnnxDestroyOnlineSpeechDenoiser(s->model);
    free(s);
}
bool rq_isolation_push(rq_isolation *s, const float *frame, unsigned frames) {
    if (rq_isolation_failed(s)) return false;
    if (write_ring(&s->in, frame, frames)) return true;
    atomic_store_explicit(&s->failed, true, memory_order_release);
    atomic_store_explicit(&s->quit, true, memory_order_relaxed);
    return false;
}
unsigned rq_isolation_available(rq_isolation *s) { return ma_pcm_rb_available_read(&s->out); }
unsigned rq_isolation_pop(rq_isolation *s, float *frame, unsigned frames) {
    return read_ring(&s->out, frame, frames);
}
bool rq_isolation_failed(const rq_isolation *s) {
    return atomic_load_explicit(&s->failed, memory_order_acquire);
}
bool rq_isolation_file(const char *models, const char *profile, const float *input, unsigned count, float *output) {
    const SherpaOnnxOnlineSpeechDenoiser *sd = make_model(models, profile);
    if (!sd) return false;
    ma_resampler up;
    if (!init_up(&up)) {
        SherpaOnnxDestroyOnlineSpeechDenoiser(sd);
        return false;
    }
    unsigned written = 0, blocks = 0, late = 0;
    double total_ms = 0, max_ms = 0;
    float expanded[4 * BLOCK];
    for (unsigned offset = 0;;) {
        unsigned n = count - offset > BLOCK ? BLOCK : count - offset;
        double start = now_ms();
        const SherpaOnnxDenoisedAudio *audio = n
            ? SherpaOnnxOnlineSpeechDenoiserRun(sd, input + offset, (int32_t)n, RQ_RATE)
            : SherpaOnnxOnlineSpeechDenoiserFlush(sd);
        unsigned got = expand(&up, audio, expanded, 4 * BLOCK);
        SherpaOnnxDestroyDenoisedAudio(audio);
        if (got > count - written) got = count - written;
        memcpy(output + written, expanded, got * sizeof(float));
        written += got;
        if (!n) break;
        double elapsed = now_ms() - start;
        if (blocks++) {
            total_ms += elapsed;
            if (elapsed > max_ms) max_ms = elapsed;
            if (elapsed > 1000. * n / RQ_RATE) ++late;
        }
        offset += n;
    }
    memset(output + written, 0, (count - written) * sizeof(float));
    ma_resampler_uninit(&up, NULL);
    SherpaOnnxDestroyOnlineSpeechDenoiser(sd);
    if (getenv("RQ_BENCH") && blocks > 1)
        fprintf(stderr, "VoxLock warm process: %.2f ms average, %.2f ms maximum per %u ms block (%u/%u late)\n",
                total_ms / (blocks - 1), max_ms, 1000 * BLOCK / RQ_RATE, late, blocks - 1);
    return written + RQ_FRAME >= count;
}

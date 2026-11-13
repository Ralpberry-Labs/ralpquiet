/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Quiet.h"
#include "Isolation.h"
#include "miniaudio.h"
#include "SpeakerModel.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { BLOCK = 4 * RQ_FRAME, RING = 32 * RQ_FRAME, VOX_RATE = 16000 };
// Fixed-ratio conversion with linear phase. Delay is 24 samples in each
// direction at 48 kHz; the offline comparison removes those 48 samples.
enum { HALF = 24, TAPS = 2 * HALF + 1, CONVERSION_DELAY = 2 * HALF };
typedef struct { float h[TAPS], history[TAPS]; unsigned phase, at; } rq_rate;
static void init_rate(rq_rate *r) {
    memset(r, 0, sizeof *r);
    double sum = 0;
    for (int i=-HALF; i<=HALF; ++i) {
        double p = 3.141592653589793 * i;
        double cutoff = 14000. / RQ_RATE;
        float h = (float)((i ? sin(cutoff*p)/p : cutoff) * (0.5+0.5*cos(p/(HALF+1))));
        r->h[i+HALF] = h;
        sum += h;
    }
    for (unsigned i=0; i<TAPS; ++i) r->h[i] = (float)(r->h[i]/sum);
}
static void push_rate(rq_rate *r, float x) {
    r->history[r->at] = x;
    if (++r->at == TAPS) r->at = 0;
}
static float fir(const rq_rate *r) {
    float value = 0;
    for (unsigned i=0,k=r->at;i<TAPS;++i) {
        value += r->h[i] * r->history[k];
        if (++k == TAPS) k=0;
    }
    return value;
}
struct rq_isolation {
    rq_speaker_model *model;
    ma_pcm_rb in, out;
    rq_rate down, up;
    pthread_t worker;
    bool in_ok, out_ok, thread_ok;
    atomic_bool quit, failed;
};

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return 1000. * ts.tv_sec + 1.e-6 * ts.tv_nsec;
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
static int process(rq_speaker_model *model, rq_rate *down, rq_rate *up,
                   const float *input, unsigned n, float *out, unsigned limit) {
    float reduced[BLOCK / 3 + 16], isolated[BLOCK / 3 + 512];
    unsigned nr = 0;
    for (unsigned i=0; i<n; ++i) {
        push_rate(down,input[i]);
        if (!down->phase) reduced[nr++] = fir(down);
        down->phase = (down->phase + 1) % 3;
    }
    int samples = rq_speaker_run(model, reduced, nr, isolated, sizeof isolated / sizeof *isolated, n == 0);
    if (samples <= 0) return samples;
    if ((unsigned)samples * 3 > limit) return -1;
    for (int i=0; i<samples; ++i) for (int k=0; k<3; ++k) {
        push_rate(up, k ? 0 : 3*isolated[i]);
        float x = fir(up);
        out[3*i+k] = isfinite(x) ? fminf(1, fmaxf(-1, x)) : 0;
    }
    return samples * 3;
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
        int n = process(s->model, &s->down, &s->up, in, BLOCK, out, BLOCK * 2);
        if (n < 0 || (n && !write_ring(&s->out, out, (unsigned)n))) break;
    }
    atomic_store_explicit(&s->failed, true, memory_order_release);
    return NULL;
}
static bool can_stream(rq_isolation *s) {
    float silence[BLOCK] = {0}, out[BLOCK * 2];
    double total = 0;
    for (int i = 0; i < 15; ++i) {
        double begin = now_ms();
        int n = process(s->model, &s->down, &s->up, silence, BLOCK, out, BLOCK * 2);
        if (n < 0) return false;
        if (i >= 3) total += now_ms() - begin;
    }
    rq_speaker_reset(s->model);
    init_rate(&s->down);
    init_rate(&s->up);
    return total < 12 * (1000. * BLOCK / RQ_RATE) * 0.85;
}
rq_isolation *rq_isolation_create(const char *models, const unsigned char *sample, size_t voice_bytes) {
    rq_isolation *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    atomic_init(&s->quit, false);
    atomic_init(&s->failed, false);
    s->model = rq_speaker_open(models, sample, voice_bytes);
    if (!s->model) goto fail;
    s->in_ok = ma_pcm_rb_init(ma_format_f32, 1, RING, NULL, NULL, &s->in) == MA_SUCCESS;
    if (!s->in_ok) goto fail;
    s->out_ok = ma_pcm_rb_init(ma_format_f32, 1, RING, NULL, NULL, &s->out) == MA_SUCCESS;
    if (!s->out_ok) goto fail;
    init_rate(&s->down);
    init_rate(&s->up);
    if (!can_stream(s)) goto fail;
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
    rq_speaker_close(s->model);
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
bool rq_isolation_file(const char *models, const unsigned char *profile, size_t voice_bytes, const float *input, unsigned count, float *output) {
    rq_speaker_model *sd = rq_speaker_open(models, profile, voice_bytes);
    if (!sd) return false;
    rq_rate down, up;
    init_rate(&down);
    init_rate(&up);
    unsigned written = 0, blocks = 0, late = 0, skip = CONVERSION_DELAY;
    double total_ms = 0, max_ms = 0;
    bool failed = false;
    float expanded[4 * BLOCK];
    for (unsigned offset = 0;;) {
        unsigned n = count - offset > BLOCK ? BLOCK : count - offset;
        double start = now_ms();
        int result = process(sd, &down, &up, input + offset, n, expanded, 4 * BLOCK);
        if (result < 0) { failed = true; break; }
        unsigned got=(unsigned)result;
        unsigned cut = got < skip ? got : skip;
        skip -= cut;
        got -= cut;
        if (got > count - written) got = count - written;
        memcpy(output + written, expanded + cut, got * sizeof(float));
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
    rq_speaker_close(sd);
    if (getenv("RQ_BENCH") && blocks > 1)
        fprintf(stderr, "VoxLock warm process: %.2f ms average, %.2f ms maximum per %u ms block (%u/%u late)\n",
                total_ms / (blocks - 1), max_ms, 1000 * BLOCK / RQ_RATE, late, blocks - 1);
    return !failed && written + CONVERSION_DELAY + 2 >= count;
}

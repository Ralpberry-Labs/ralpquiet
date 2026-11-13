/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Quiet.h"
#include "Isolation.h"
#include "miniaudio.h"
#include "rnnoise.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(ATOMIC_INT_LOCK_FREE == 2 && ATOMIC_BOOL_LOCK_FREE == 2,
               "Audio callbacks require lock-free atomics");

struct rq_filter { DenoiseState *state; };

rq_filter *rq_filter_create(void) {
    rq_filter *filter = calloc(1, sizeof *filter);
    if (!filter) return NULL;
    filter->state = rnnoise_create(NULL);
    if (!filter->state) { free(filter); return NULL; }
    float warmup[RQ_FRAME] = {0};
    rnnoise_process_frame(filter->state, warmup, warmup);
    return filter;
}
void rq_filter_destroy(rq_filter *filter) {
    if (filter) { rnnoise_destroy(filter->state); free(filter); }
}
static inline float bounded(float x) {
    return isfinite(x) ? fmaxf(-1, fminf(1, x)) : 0;
}
void rq_filter_frame(rq_filter *filter, const float *in, float *out) {
    float pcm[RQ_FRAME];
    for (unsigned i = 0; i < RQ_FRAME; ++i) pcm[i] = bounded(in[i]) * 32768.f;
    rnnoise_process_frame(filter->state, pcm, pcm);
    for (unsigned i = 0; i < RQ_FRAME; ++i) out[i] = bounded(pcm[i] / 32768.f);
}

enum { FILTER_DELAY = 2 * RQ_FRAME, QUEUE = RQ_FRAME * 16, TARGET = RQ_FRAME * 4, TEST = RQ_RATE * RQ_TEST_SECONDS };
struct rq_route {
    ma_context ctx;
    ma_device capture, playback;
    ma_pcm_rb queue;
    bool ctx_ok, cap_ok, play_ok, queue_ok, primed;
    rq_filter *live, *comparison;
    rq_isolation *isolation;
    bool isolated_started, isolated_primed;
    float frame[RQ_FRAME], cleaned[RQ_FRAME], dry_delay[FILTER_DELAY], test_delay[FILTER_DELAY];
    unsigned frame_len, delay_pos, test_pos;
    float blend;
    atomic_bool suppress;
    atomic_int test_state; // 0=idle, 1=requested, 2=recording, 3=stop requested, 4=ready
    atomic_uint recorded, in_peak, out_peak, underruns, overruns;
    float *raw, *wet;
};

static void enqueue(rq_route *r, const float *data, unsigned count) {
    if (ma_pcm_rb_available_write(&r->queue) < count) {
        atomic_fetch_add_explicit(&r->overruns, 1, memory_order_relaxed);
        return;
    }
    while (count) {
        ma_uint32 size = count;
        void *dst;
        if (ma_pcm_rb_acquire_write(&r->queue, &size, &dst) != MA_SUCCESS || !size) return;
        memcpy(dst, data, size * sizeof(float));
        ma_pcm_rb_commit_write(&r->queue, size);
        count -= size;
        data += size;
    }
}

static unsigned dequeue(rq_route *r, float *data, unsigned count, unsigned stride) {
    unsigned copied = 0;
    while (copied < count) {
        ma_uint32 size = count - copied;
        void *src;
        if (ma_pcm_rb_acquire_read(&r->queue, &size, &src) != MA_SUCCESS || !size) break;
        if (data) {
            const float *v = src;
            for (unsigned i = 0; i < size; ++i)
                for (unsigned ch = 0; ch < stride; ++ch) data[(copied + i) * stride + ch] = v[i];
        }
        ma_pcm_rb_commit_read(&r->queue, size);
        copied += size;
    }
    return copied;
}
static void playback_cb(ma_device *device, void *output, const void *input, ma_uint32 n) {
    (void)input;
    rq_route *r = device->pUserData;
    unsigned channels = device->playback.channels;
    float *out = output;
    memset(out, 0, n * channels * sizeof(float));
    if (r->isolation) {
        // The dedicated worker never runs on the Core Audio thread. Keep a
        // small reserve to absorb compute jitter; missing output is silence.
        if (rq_isolation_failed(r->isolation)) return;
        unsigned ready = rq_isolation_available(r->isolation);
        if (!r->isolated_primed && (ready < 5 * RQ_FRAME || ready < n)) {
            atomic_store_explicit(&r->out_peak, 0, memory_order_relaxed);
            return;
        }
        r->isolated_primed = true;
        float mono[1024];
        if (ready > 11 * RQ_FRAME && ready > n + 8 * RQ_FRAME) {
            unsigned excess = ready - n - 8 * RQ_FRAME;
            (void)rq_isolation_pop(r->isolation, mono, excess > 1024 ? 1024 : excess);
            atomic_fetch_add_explicit(&r->overruns, 1, memory_order_relaxed);
        }
        unsigned filled = 0;
        while (filled < n) {
            unsigned want = n - filled > 1024 ? 1024 : n - filled;
            unsigned got = rq_isolation_pop(r->isolation, mono, want);
            float peak = 0;
            for (unsigned i = 0; i < got; ++i) {
                peak = fmaxf(peak, fabsf(mono[i]));
                for (unsigned ch = 0; ch < channels; ++ch) out[(filled + i) * channels + ch] = mono[i];
            }
            if (got) {
                atomic_store_explicit(&r->out_peak, (unsigned)(peak * 10000), memory_order_relaxed);
                r->isolated_started = true;
            }
            filled += got;
            if (got < want) break;
        }
        if (r->isolated_started && filled < n) {
            r->isolated_primed = false;
            atomic_fetch_add_explicit(&r->underruns, 1, memory_order_relaxed);
        }
        return;
    }
    unsigned available = ma_pcm_rb_available_read(&r->queue);
    if (!r->primed && (available < TARGET || available < n)) return;
    r->primed = true;
    // Independent hardware clocks drift. Bound added latency even after a slow consumer.
    if (available > TARGET + 3 * RQ_FRAME && available > n + TARGET) {
        unsigned excess = available - n - TARGET;
        dequeue(r, NULL, excess, 1);
        atomic_fetch_add_explicit(&r->overruns, 1, memory_order_relaxed);
    }
    unsigned copied = dequeue(r, out, n, channels);
    if (copied < n) {
        r->primed = false;
        atomic_fetch_add_explicit(&r->underruns, 1, memory_order_relaxed);
    }
}
static void compare_frame(rq_route *r) {
    int state = atomic_load_explicit(&r->test_state, memory_order_acquire);
    if (state == 3) { atomic_store_explicit(&r->test_state, 4, memory_order_release); return; }
    if (state == 1) {
        int expected = 1;
        if (!atomic_compare_exchange_strong(&r->test_state, &expected, 2)) return;
        memset(r->test_delay, 0, sizeof r->test_delay);
        r->test_pos = 0;
        state = 2;
    }
    if (state != 2) return;
    unsigned p = atomic_load_explicit(&r->recorded, memory_order_relaxed);
    if (p < TEST) {
        // Both clips are aligned to RNNoise v0.2's two-frame delay.
        memcpy(r->raw + p, r->test_delay + r->test_pos, sizeof r->frame);
        rq_filter_frame(r->comparison, r->frame, r->wet + p);
        memcpy(r->test_delay + r->test_pos, r->frame, sizeof r->frame);
        r->test_pos ^= RQ_FRAME;
        atomic_store_explicit(&r->recorded, p + RQ_FRAME, memory_order_release);
    }
    if (p + RQ_FRAME >= TEST) atomic_store_explicit(&r->test_state, 4, memory_order_release);
}
static void capture_cb(ma_device *device, void *output, const void *input, ma_uint32 n) {
    (void)output;
    if (!input) return;
    rq_route *r = device->pUserData;
    const float *in = input;
    for (unsigned offset = 0; offset < n;) {
        unsigned size = RQ_FRAME - r->frame_len;
        if (size > n - offset) size = n - offset;
        for (unsigned i = 0; i < size; ++i) r->frame[r->frame_len + i] = bounded(in[offset + i]);
        r->frame_len += size;
        offset += size;
        if (r->frame_len != RQ_FRAME) continue;
        r->frame_len = 0;
        compare_frame(r);
        rq_filter_frame(r->live, r->frame, r->cleaned);
        float aim = atomic_load_explicit(&r->suppress, memory_order_relaxed) ? 1.f : 0.f;
        float start = r->blend, peak_in = 0, peak_out = 0;
        for (unsigned i = 0; i < RQ_FRAME; ++i) {
            float mix = start + (aim - start) * (float)(i + 1) / RQ_FRAME;
            peak_in = fmaxf(peak_in, fabsf(r->frame[i]));
            r->cleaned[i] = r->dry_delay[r->delay_pos + i] + mix * (r->cleaned[i] - r->dry_delay[r->delay_pos + i]);
            peak_out = fmaxf(peak_out, fabsf(r->cleaned[i]));
        }
        r->blend = aim;
        memcpy(r->dry_delay + r->delay_pos, r->frame, sizeof r->frame);
        r->delay_pos ^= RQ_FRAME;
        atomic_store_explicit(&r->in_peak, (unsigned)(peak_in * 10000), memory_order_relaxed);
        if (!r->isolation) atomic_store_explicit(&r->out_peak, (unsigned)(peak_out * 10000), memory_order_relaxed);
        if (r->isolation) {
            (void)rq_isolation_push(r->isolation, r->frame, RQ_FRAME);
        } else if (r->play_ok) {
            enqueue(r, r->cleaned, RQ_FRAME);
        }
    }
}

static ma_result context_init(ma_context *ctx) {
    ma_backend backend = ma_backend_coreaudio;
    return ma_context_init(&backend, 1, NULL, ctx);
}
size_t rq_devices(bool input, rq_device *devices, size_t capacity) {
    ma_context ctx;
    if (context_init(&ctx) != MA_SUCCESS) return 0;
    ma_device_info *info = NULL;
    ma_uint32 count = 0;
    ma_result res = input ? ma_context_get_devices(&ctx, NULL, NULL, &info, &count)
                          : ma_context_get_devices(&ctx, &info, &count, NULL, NULL);
    if (res != MA_SUCCESS) count = 0;
    if (count > capacity) count = (ma_uint32)capacity;
    for (size_t i = 0; i < count; ++i) {
        snprintf(devices[i].uid, sizeof devices[i].uid, "%s", info[i].id.coreaudio);
        snprintf(devices[i].name, sizeof devices[i].name, "%s", info[i].name);
    }
    ma_context_uninit(&ctx);
    return count;
}
static bool device_id(ma_device_id *id, const char *uid) {
    if (!uid || !*uid || strlen(uid) >= sizeof id->coreaudio) return false;
    memset(id, 0, sizeof *id);
    memcpy(id->coreaudio, uid, strlen(uid));
    return true;
}
rq_route *rq_route_open(const char *input_uid, const char *output_uid, bool suppress,
                       const char *models, const char *voice_sample, char *error, size_t error_capacity) {
    const char *step = "allocate audio engine";
    ma_result result = MA_OUT_OF_MEMORY;
    rq_route *r = calloc(1, sizeof *r);
    if (!r) goto fail;
    if (output_uid && strcmp(input_uid ? input_uid : "", output_uid) == 0) {
        step = "route input to itself"; result = MA_INVALID_ARGS; goto fail;
    }
    atomic_init(&r->suppress, suppress);
    atomic_init(&r->test_state, 0);
    atomic_init(&r->recorded, 0);
    atomic_init(&r->in_peak, 0);
    atomic_init(&r->out_peak, 0);
    atomic_init(&r->underruns, 0);
    atomic_init(&r->overruns, 0);
    r->blend = suppress ? 1 : 0;
    r->live = rq_filter_create();
    r->comparison = rq_filter_create();
    r->raw = calloc(TEST, sizeof(float));
    r->wet = calloc(TEST, sizeof(float));
    if (!r->live || !r->comparison || !r->raw || !r->wet) goto fail;
    if (models && voice_sample) {
        step = "start voice isolation";
        r->isolation = rq_isolation_create(models, voice_sample);
        if (!r->isolation) {
            if (error && error_capacity)
                snprintf(error, error_capacity, "Only my voice could not initialize or keep up on this Mac. Try a local test or use Remove microphone noise for the call.");
            rq_route_close(r);
            return NULL;
        }
    }
    step = "open Core Audio";
    if ((result = context_init(&r->ctx)) != MA_SUCCESS) goto fail;
    r->ctx_ok = true;
    if (output_uid) {
        ma_device_id output_id;
        step = "select the output";
        if (!device_id(&output_id, output_uid)) { result = MA_INVALID_ARGS; goto fail; }
        if ((result = ma_pcm_rb_init(ma_format_f32, 1, QUEUE, NULL, NULL, &r->queue)) != MA_SUCCESS) goto fail;
        r->queue_ok = true;
        ma_device_config pc = ma_device_config_init(ma_device_type_playback);
        pc.playback.pDeviceID = &output_id;
        pc.playback.format = ma_format_f32;
        pc.playback.channels = 2;
        pc.sampleRate = RQ_RATE;
        pc.periodSizeInFrames = RQ_FRAME;
        pc.performanceProfile = ma_performance_profile_low_latency;
        pc.dataCallback = playback_cb;
        pc.pUserData = r;
        step = "open the output";
        if ((result = ma_device_init(&r->ctx, &pc, &r->playback)) != MA_SUCCESS) goto fail;
        r->play_ok = true;
    }
    ma_device_id input_id;
    step = "select the input";
    if (!device_id(&input_id, input_uid)) { result = MA_INVALID_ARGS; goto fail; }
    ma_device_config cc = ma_device_config_init(ma_device_type_capture);
    cc.capture.pDeviceID = &input_id;
    cc.capture.format = ma_format_f32;
    cc.capture.channels = 1;
    cc.sampleRate = RQ_RATE;
    cc.periodSizeInFrames = RQ_FRAME;
    cc.performanceProfile = ma_performance_profile_low_latency;
    cc.dataCallback = capture_cb;
    cc.pUserData = r;
    step = "open the input";
    if ((result = ma_device_init(&r->ctx, &cc, &r->capture)) != MA_SUCCESS) goto fail;
    r->cap_ok = true;
    step = "start the output";
    if (r->play_ok && (result = ma_device_start(&r->playback)) != MA_SUCCESS) goto fail;
    step = "start the input";
    if ((result = ma_device_start(&r->capture)) != MA_SUCCESS) goto fail;
    return r;
fail:
    if (error && error_capacity) snprintf(error, error_capacity, "Could not %s: %s.", step, ma_result_description(result));
    rq_route_close(r);
    return NULL;
}
void rq_route_close(rq_route *r) {
    if (!r) return;
    if (r->cap_ok) ma_device_uninit(&r->capture);
    if (r->play_ok) ma_device_uninit(&r->playback);
    if (r->queue_ok) ma_pcm_rb_uninit(&r->queue);
    if (r->ctx_ok) ma_context_uninit(&r->ctx);
    rq_isolation_destroy(r->isolation);
    rq_filter_destroy(r->live);
    rq_filter_destroy(r->comparison);
    free(r->raw); free(r->wet); free(r);
}
void rq_route_set_suppress(rq_route *r, bool on) {
    if (r) atomic_store_explicit(&r->suppress, on, memory_order_relaxed);
}
rq_status rq_route_status(rq_route *r) {
    if (!r) return (rq_status){0};
    return (rq_status){
        .input_peak = (float)atomic_load_explicit(&r->in_peak, memory_order_relaxed) / 10000,
        .output_peak = (float)atomic_load_explicit(&r->out_peak, memory_order_relaxed) / 10000,
        .underruns = atomic_load_explicit(&r->underruns, memory_order_relaxed),
        .overruns = atomic_load_explicit(&r->overruns, memory_order_relaxed),
        .recorded_frames = atomic_load_explicit(&r->recorded, memory_order_acquire),
        .isolation_failed = r->isolation && rq_isolation_failed(r->isolation),
        .running = ma_device_is_started(&r->capture) && (!r->play_ok || ma_device_is_started(&r->playback))
    };
}
bool rq_test_start(rq_route *r) {
    if (!r) return false;
    int state = atomic_load_explicit(&r->test_state, memory_order_acquire);
    if (state != 0 && state != 4) return false;
    rq_filter *fresh = rq_filter_create();
    if (!fresh) return false;
    rq_filter_destroy(r->comparison);
    r->comparison = fresh;
    atomic_store_explicit(&r->recorded, 0, memory_order_relaxed);
    atomic_store_explicit(&r->test_state, 1, memory_order_release);
    return true;
}
unsigned rq_test_stop(rq_route *r) {
    if (!r) return 0;
    int state = atomic_load_explicit(&r->test_state, memory_order_acquire);
    while (state == 1 || state == 2) {
        if (atomic_compare_exchange_weak(&r->test_state, &state, 3)) return 0;
    }
    return state == 4 ? atomic_load_explicit(&r->recorded, memory_order_acquire) : 0;
}
void rq_test_copy(const rq_route *r, float *raw, float *wet, unsigned frames) {
    if (r && atomic_load_explicit(&r->test_state, memory_order_acquire) == 4 &&
        frames <= atomic_load_explicit(&r->recorded, memory_order_acquire)) {
        memcpy(raw, r->raw, frames * sizeof(float));
        memcpy(wet, r->wet, frames * sizeof(float));
    }
}

bool rq_personalized_compare(const char *models, const char *voice_sample, const float *input, unsigned frames, float *output) {
    return rq_isolation_file(models, voice_sample, input, frames, output);
}

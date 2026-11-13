/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "Echo.h"

enum { RQ_RATE = 48000, RQ_FRAME = 480, RQ_TEST_SECONDS = 12 };
typedef struct rq_route rq_route;
typedef struct rq_filter rq_filter;
typedef struct { char uid[256]; char name[256]; } rq_device;
typedef struct {
    float input_peak, output_peak;
    uint32_t underruns, overruns, recorded_frames;
    bool running, isolation_failed;
} rq_status;

// Frame-based, mono normalized float. Input and output may alias.
rq_filter *rq_filter_create(void);
void rq_filter_destroy(rq_filter *);
void rq_filter_frame(rq_filter *, const float *input, float *output);
size_t rq_devices(bool input, rq_device *devices, size_t capacity);
rq_route *rq_route_open(const char *input_uid, const char *output_uid, bool suppress,
                       const char *models, const unsigned char *voice_sample, size_t voice_bytes, rq_echo *echo, bool render_reference,
                       char *error, size_t error_capacity);
void rq_route_close(rq_route *);
void rq_route_set_suppress(rq_route *, bool);
rq_status rq_route_status(rq_route *);
// Test capture is bounded, in memory, and independent of the live routing.
bool rq_test_start(rq_route *);
unsigned rq_test_stop(rq_route *);
void rq_test_copy(const rq_route *, float *raw, float *filtered, unsigned frames);

// Offline comparison using the same personalized stream as live mode.
bool rq_personalized_compare(const char *models, const unsigned char *voice_sample, size_t voice_bytes,
                             const float *input, unsigned frames, float *output);

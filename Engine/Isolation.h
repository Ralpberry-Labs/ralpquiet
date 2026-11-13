/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct rq_isolation rq_isolation;
enum { RQ_ISOLATION_PRIME_FRAMES = 10, RQ_ISOLATION_HIGH_FRAMES = 18 };
rq_isolation *rq_isolation_create(const char *models, const unsigned char *sample, size_t voice_bytes);
void rq_isolation_destroy(rq_isolation *);
bool rq_isolation_push(rq_isolation *, const float *frame, unsigned frames);
unsigned rq_isolation_available(rq_isolation *);
unsigned rq_isolation_pop(rq_isolation *, float *frame, unsigned frames);
bool rq_isolation_failed(const rq_isolation *);
bool rq_isolation_file(const char *models, const unsigned char *profile, size_t voice_bytes, const float *input, unsigned count, float *output);

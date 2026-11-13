/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct rq_isolation rq_isolation;
rq_isolation *rq_isolation_create(const char *models, const char *sample);
void rq_isolation_destroy(rq_isolation *);
bool rq_isolation_push(rq_isolation *, const float *frame, unsigned frames);
unsigned rq_isolation_available(rq_isolation *);
unsigned rq_isolation_pop(rq_isolation *, float *frame, unsigned frames);
bool rq_isolation_failed(const rq_isolation *);
bool rq_isolation_file(const char *models, const char *profile, const float *input, unsigned count, float *output);

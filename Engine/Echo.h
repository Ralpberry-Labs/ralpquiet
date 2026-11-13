/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct rq_echo rq_echo;
rq_echo *rq_echo_open(void);
void rq_echo_close(rq_echo *);
void rq_echo_render(rq_echo *, const float *interleaved, unsigned frames, unsigned channels);
bool rq_echo_capture(rq_echo *, float *mono);
#ifdef __cplusplus
}
#endif

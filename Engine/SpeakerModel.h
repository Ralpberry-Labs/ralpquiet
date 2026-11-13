/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct rq_speaker_model rq_speaker_model;
/* Enrollment is a 16 or 48 kHz mono PCM16 WAV. Calls belong to one worker thread. */
rq_speaker_model *rq_speaker_open(const char *models, const unsigned char *voice, size_t voice_bytes);
void rq_speaker_close(rq_speaker_model *);
void rq_speaker_reset(rq_speaker_model *);
int rq_speaker_run(rq_speaker_model *, const float *, size_t, float *, size_t, bool finish);
#ifdef __cplusplus
}
#endif

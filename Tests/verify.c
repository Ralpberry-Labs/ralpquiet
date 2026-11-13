/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Quiet.h"
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
        if (rq_personalized_compare(models, profile, input, n, output)) {
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
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--devices")) return devices();
    if (argc == 4 && !strcmp(argv[1], "--raw")) return filter_raw(argv[2], argv[3]);
    if (argc == 6 && !strcmp(argv[1], "--isolate")) return isolate(argv[2], argv[3], argv[4], argv[5]);
    if (argc != 1) { fprintf(stderr, "usage: rq-verify [--devices | --raw input.f32 output.f32 | --isolate models voice.wav input.f32 output.f32]\n"); return 2; }
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

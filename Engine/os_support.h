/* SPDX-License-Identifier: BSD-3-Clause
 * RNNoise v0.2's NEON header imports this missing header for OPUS_CLEAR.
 */
#pragma once
#include <string.h>
#define OPUS_CLEAR(dst, n) memset((dst), 0, (size_t)(n) * sizeof(*(dst)))

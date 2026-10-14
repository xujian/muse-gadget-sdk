/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "muse_motion.h"

#include <stdlib.h>

#define MUSE_MOTION_GRACE_MS 2000   /* after sleeping: settle before watching */
#define MUSE_MOTION_MG 150          /* off rest by this on any axis: about 9 degrees of tilt */
#define MUSE_MOTION_HITS 2          /* ... in this many of the last three readings */
#define MUSE_MOTION_DRIFT 5         /* rest follows still readings by 1/32 each */

static int32_t s_rest[3];       /* resting reading, in 1/32 mg */
static uint32_t s_since_ms;
static bool s_settled;
static uint8_t s_recent;        /* the last three readings, 1 = moved, newest in bit 0 */

void muse_motion_reset(uint32_t now_ms)
{
    s_since_ms = now_ms;
    s_settled = false;
    s_recent = 0;
}

bool muse_motion_update(const int16_t mg[3], uint32_t now_ms)
{
    if (!s_settled) {
        /* Still settling: rest is wherever the board is now. */
        for (int i = 0; i < 3; i++) {
            s_rest[i] = (int32_t)mg[i] << MUSE_MOTION_DRIFT;
        }
        s_settled = now_ms - s_since_ms >= MUSE_MOTION_GRACE_MS;
        return false;
    }
    int off = 0;
    for (int i = 0; i < 3; i++) {
        int d = abs(mg[i] - (int)(s_rest[i] >> MUSE_MOTION_DRIFT));
        off = d > off ? d : off;
    }
    bool moved = off > MUSE_MOTION_MG;
    s_recent = (uint8_t)((s_recent << 1 | moved) & 7);
    if (!moved) {
        /* Sensor offsets wander with temperature; let rest follow slowly. */
        for (int i = 0; i < 3; i++) {
            s_rest[i] += mg[i] - (s_rest[i] >> MUSE_MOTION_DRIFT);
        }
    }
    return __builtin_popcount(s_recent) >= MUSE_MOTION_HITS;
}

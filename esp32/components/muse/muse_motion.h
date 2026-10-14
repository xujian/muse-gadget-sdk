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

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Pick-up detection for a board with an accelerometer: tells a board lifted or
 * tilted from one resting still. Readings are in milli-g on the board's axes;
 * at rest they are gravity, so any change in position shows. No hardware here.
 */

/* Rest from now on: the next MUSE_MOTION_GRACE_MS of readings set the resting
 * position (the hand that put the screen to sleep is still settling). */
void muse_motion_reset(uint32_t now_ms);

/* One reading; true once the board has moved off its resting position. */
bool muse_motion_update(const int16_t mg[3], uint32_t now_ms);

#ifdef __cplusplus
}
#endif

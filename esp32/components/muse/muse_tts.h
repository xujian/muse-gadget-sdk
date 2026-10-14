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

/*
 * Speech for Muse's replies, from ElevenLabs (CONFIG_MUSE_TTS_ELEVENLABS).
 *
 * One reply at a time: muse_tts_start() hands the text to a task of its own,
 * which posts it to ElevenLabs and buffers the MP3 that streams back. The
 * session task (muse_chat_session.cpp) reads it with muse_tts_read() and plays
 * it. Starting again, or muse_tts_cancel(), drops whatever was in flight.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MUSE_TTS_IDLE,
    MUSE_TTS_RUNNING,
    MUSE_TTS_DONE,      /* all of the MP3 is buffered; read until it's empty */
    MUSE_TTS_FAILED,
} muse_tts_state_t;

/* An API key is set. */
bool muse_tts_enabled(void);
/* Starts fetching speech for `text` (copied). False if it couldn't start. */
bool muse_tts_start(const char *text);
/* Copies out MP3 that has arrived, up to `cap` bytes. Never blocks. */
size_t muse_tts_read(uint8_t *buf, size_t cap);
/* Read the state before muse_tts_read(): DONE or FAILED with nothing left to
 * read is then the end of the speech. */
muse_tts_state_t muse_tts_state(void);
void muse_tts_cancel(void);
/* Call the four above from one task: the session task's. */

/* The request, for the host test: the URL for a voice, and the JSON body
 * (for cJSON_free(); NULL if out of memory). */
void muse_tts_url(const char *voice_id, char *out, size_t cap);
char *muse_tts_body(const char *text, const char *model_id);

#ifdef __cplusplus
}
#endif

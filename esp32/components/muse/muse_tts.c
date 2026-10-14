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

#include "muse_tts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "sdkconfig.h"

/* ---- Request ---- */

/* 22.05 kHz at 32 kbps: the smallest MP3 ElevenLabs makes, and on every plan. */
#define TTS_URL "https://api.elevenlabs.io/v1/text-to-speech/%s/stream?output_format=mp3_22050_32"

bool muse_tts_enabled(void)
{
    return CONFIG_MUSE_TTS_ELEVENLABS_API_KEY[0] != '\0';
}

void muse_tts_url(const char *voice_id, char *out, size_t cap)
{
    snprintf(out, cap, TTS_URL, voice_id);
}

char *muse_tts_body(const char *text, const char *model_id)
{
    cJSON *root = cJSON_CreateObject();
    if (!root || !cJSON_AddStringToObject(root, "text", text) ||
        !cJSON_AddStringToObject(root, "model_id", model_id)) {
        cJSON_Delete(root);
        return NULL;
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

/* ---- Fetch ---- */

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "muse_tts";

#define BUF_BYTES (64 * 1024)   /* 16 s of speech waiting for the player */
#define CHUNK 2048
#define TIMEOUT_MS 15000

typedef struct {
    uint32_t gen;
    char *text;
} req_t;

static SemaphoreHandle_t s_lock;        /* s_gen, s_state and what's in s_buf go together */
static StreamBufferHandle_t s_buf;
static QueueHandle_t s_reqs;
static uint32_t s_gen;                  /* the request whose MP3 belongs in s_buf */
static muse_tts_state_t s_state;

static void finish(uint32_t gen, muse_tts_state_t state)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (gen == s_gen) {
        s_state = state;
    }
    xSemaphoreGive(s_lock);
}

/* Buffers MP3 for the player, waiting while it's full. False once `gen` is stale. */
static bool push(uint32_t gen, const uint8_t *data, size_t len)
{
    while (len) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool current = gen == s_gen;
        size_t sent = current ? xStreamBufferSend(s_buf, data, len, 0) : 0;
        xSemaphoreGive(s_lock);
        if (!current) {
            return false;
        }
        data += sent;
        len -= sent;
        if (len) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    return true;
}

static bool write_all(esp_http_client_handle_t c, const char *data, int len)
{
    while (len > 0) {
        int n = esp_http_client_write(c, data, len);
        if (n <= 0) {
            return false;
        }
        data += n;
        len -= n;
    }
    return true;
}

static muse_tts_state_t fetch(const req_t *req)
{
    char url[160];
    muse_tts_url(CONFIG_MUSE_TTS_ELEVENLABS_VOICE_ID, url, sizeof(url));
    char *body = muse_tts_body(req->text, CONFIG_MUSE_TTS_ELEVENLABS_MODEL);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = body ? esp_http_client_init(&cfg) : NULL;
    if (!c) {
        cJSON_free(body);
        return MUSE_TTS_FAILED;
    }
    esp_http_client_set_header(c, "xi-api-key", CONFIG_MUSE_TTS_ELEVENLABS_API_KEY);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Accept", "audio/mpeg");

    muse_tts_state_t result = MUSE_TTS_FAILED;
    static uint8_t chunk[CHUNK];   /* one worker */
    int len = (int)strlen(body);
    int status = 0;
    size_t total = 0;
    if (esp_http_client_open(c, len) != ESP_OK || !write_all(c, body, len) ||
        esp_http_client_fetch_headers(c) < 0) {
        ESP_LOGW(TAG, "can't reach ElevenLabs");
        goto out;
    }
    status = esp_http_client_get_status_code(c);
    if (status != 200) {
        /* The body says why: a bad key, an unknown voice, no credits left. */
        int n = esp_http_client_read(c, (char *)chunk, 200);
        ESP_LOGW(TAG, "ElevenLabs answered %d: %.*s", status, n > 0 ? n : 0, (const char *)chunk);
        goto out;
    }
    for (;;) {
        int n = esp_http_client_read(c, (char *)chunk, CHUNK);
        if (n < 0) {
            ESP_LOGW(TAG, "speech cut off after %u bytes", (unsigned)total);
            break;
        }
        if (n == 0) {
            if (esp_http_client_is_complete_data_received(c)) {
                result = MUSE_TTS_DONE;
            } else {
                ESP_LOGW(TAG, "speech cut off after %u bytes", (unsigned)total);
            }
            break;
        }
        if (!push(req->gen, chunk, (size_t)n)) {
            break;   /* cancelled, or a newer reply took over */
        }
        total += (size_t)n;
    }
    if (result == MUSE_TTS_DONE) {
        ESP_LOGI(TAG, "speech: %u bytes of MP3 for %u characters", (unsigned)total, (unsigned)strlen(req->text));
    }
out:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    cJSON_free(body);
    return result;
}

static void tts_task(void *arg)
{
    QueueHandle_t reqs = arg;   /* s_reqs isn't set until the task exists */
    for (;;) {
        req_t req;
        xQueueReceive(reqs, &req, portMAX_DELAY);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool current = req.gen == s_gen;
        xSemaphoreGive(s_lock);
        if (current) {
            finish(req.gen, fetch(&req));
        }
        free(req.text);
    }
}

static bool init(void)
{
    if (s_reqs) {
        return true;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_buf) {
        s_buf = xStreamBufferCreateWithCaps(BUF_BYTES, 1, MALLOC_CAP_SPIRAM);
    }
    QueueHandle_t reqs = xQueueCreate(2, sizeof(req_t));
    /* Stack in PSRAM, like the session task's: the TLS handshake runs here. */
    if (!s_lock || !s_buf || !reqs ||
        xTaskCreatePinnedToCoreWithCaps(tts_task, "muse_tts", 8 * 1024, reqs, 5, NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "start failed");
        if (reqs) {
            vQueueDelete(reqs);
        }
        return false;
    }
    s_reqs = reqs;
    return true;
}

/* Makes whatever is in flight stale and empties the buffer. */
static uint32_t supersede(muse_tts_state_t state)
{
    static uint8_t drop[256];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t gen = ++s_gen;
    s_state = state;
    while (xStreamBufferReceive(s_buf, drop, sizeof(drop), 0)) {
    }
    xSemaphoreGive(s_lock);
    return gen;
}

bool muse_tts_start(const char *text)
{
    if (!muse_tts_enabled() || !text || !text[0] || !init()) {
        return false;
    }
    req_t req = { .text = strdup(text) };
    if (!req.text) {
        return false;
    }
    req.gen = supersede(MUSE_TTS_RUNNING);
    if (xQueueSend(s_reqs, &req, 0) != pdTRUE) {
        /* The worker is still letting go of an earlier reply or two. */
        free(req.text);
        supersede(MUSE_TTS_IDLE);
        return false;
    }
    return true;
}

size_t muse_tts_read(uint8_t *buf, size_t cap)
{
    return s_buf ? xStreamBufferReceive(s_buf, buf, cap, 0) : 0;
}

muse_tts_state_t muse_tts_state(void)
{
    if (!s_lock) {
        return MUSE_TTS_IDLE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    muse_tts_state_t state = s_state;
    xSemaphoreGive(s_lock);
    return state;
}

void muse_tts_cancel(void)
{
    if (s_lock && s_buf && muse_tts_state() != MUSE_TTS_IDLE) {
        supersede(MUSE_TTS_IDLE);
    }
}

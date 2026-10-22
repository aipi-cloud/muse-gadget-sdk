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

/*
 * What a TTS service actually sent back, worked out before anything tries to
 * decode it. Owners point the firmware at their own speech service, and many
 * answer with WAV or bare PCM even when asked for MP3, so the reply is sniffed
 * rather than assumed. Plain C11: the host test builds this without ESP-IDF.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MUSE_AUDIO_MP3 = 0,
    MUSE_AUDIO_WAV_PCM16,
    MUSE_AUDIO_PCM16,
    MUSE_AUDIO_UNSUPPORTED,
} muse_audio_kind_t;

typedef struct {
    muse_audio_kind_t kind;
    uint32_t rate;        /* Hz; 0 when unsupported or unknown */
    uint8_t  channels;    /* 1 or 2; 0 when unsupported or unknown */
    size_t   offset;      /* first audio byte inside the buffer */
    size_t   length;      /* playable byte count from offset */
    const char *reason;   /* static string for unsupported: "ogg", "flac", "m4a",
                             "riff", "wav-codec", "wav-truncated", "text",
                             "unknown", "empty" */
} muse_audio_info_t;

/*
 * Classifies the buffer, never reading past `len`. `content_type` may be NULL;
 * it is only believed for raw PCM (audio/l16, audio/pcm), so a JSON error body
 * served with HTTP 200 is never played as noise. Returns false only when data
 * is NULL or len is 0 (the info is still filled in, as unsupported/"empty").
 */
bool muse_audio_probe(const uint8_t *data, size_t len, const char *content_type,
                      muse_audio_info_t *out);

#ifdef __cplusplus
}
#endif

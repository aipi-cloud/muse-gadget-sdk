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
 * The two request shapes the firmware speaks, and what comes back from each.
 * Owners point the device at their own speech service, so the protocol is a
 * setting, not a guess: "openai" is the OpenAI-compatible audio/speech shape,
 * "gemini" is Google's generateContent shape with the audio inline.
 *
 * Plain C11 with no ESP-IDF, so the host test builds this without a board.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The request shape. */
typedef enum {
    MUSE_TTS_API_OPENAI = 0,   /* POST {model,input,voice,response_format} */
    MUSE_TTS_API_GEMINI = 1,   /* generateContent, responseModalities AUDIO */
} muse_tts_api_t;

/* What a service answered with. */
typedef struct {
    size_t bytes;        /* audio bytes written to out[] */
    bool truncated;      /* the audio did not fit in out[] */
    char detail[128];    /* the service's own message when there was no audio */
    char mime[40];       /* the audio's own media type, when the service said
                             so ("audio/L16;codec=pcm;rate=24000") */
} muse_tts_reply_t;

/* The style named by the saved setting. Anything unknown is OpenAI. */
muse_tts_api_t muse_tts_api_parse(const char *name);

/*
 * The request body for `text`. `format` is the response_format value, or NULL
 * to leave the field out so the service picks its own format. False when the
 * body would not fit in cap.
 */
bool muse_tts_build_body(muse_tts_api_t api, const char *model, const char *voice,
                         const char *text, const char *format, char *out, size_t cap);

/* The header that carries the key: "Authorization" or "x-goog-api-key". */
const char *muse_tts_auth_header(muse_tts_api_t api);

/* The header's value: "Bearer <key>" for OpenAI, the key itself for Gemini. */
bool muse_tts_auth_value(muse_tts_api_t api, const char *key, char *out, size_t cap);

/*
 * Pulls the audio out of a reply. For OpenAI the body is the audio; for
 * Gemini it is JSON with base64 audio in candidates[].content.parts[].
 * False when there is no audio: reply->detail then says what the service
 * said, or "no-audio" when it said nothing.
 */
bool muse_tts_extract(muse_tts_api_t api, const uint8_t *body, size_t len,
                      uint8_t *out, size_t cap, muse_tts_reply_t *reply);

#ifdef __cplusplus
}
#endif

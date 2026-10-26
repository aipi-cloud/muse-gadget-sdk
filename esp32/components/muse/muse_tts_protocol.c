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

#include "muse_tts_protocol.h"

#include <string.h>

/* ---- small helpers ---- */

static void set_detail(char *dst, size_t cap, const char *text)
{
    if (!dst || !cap) {
        return;
    }
    size_t n = strlen(text);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, text, n);
    dst[n] = '\0';
}

static bool ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return !*a && !*b;
}

muse_tts_api_t muse_tts_api_parse(const char *name)
{
    if (name && ieq(name, "gemini")) {
        return MUSE_TTS_API_GEMINI;
    }
    return MUSE_TTS_API_OPENAI;
}

const char *muse_tts_auth_header(muse_tts_api_t api)
{
    return api == MUSE_TTS_API_GEMINI ? "x-goog-api-key" : "Authorization";
}

bool muse_tts_auth_value(muse_tts_api_t api, const char *key, char *out, size_t cap)
{
    if (!out || !cap) {
        return false;
    }
    out[0] = '\0';
    if (!key) {
        key = "";
    }
    if (api == MUSE_TTS_API_GEMINI) {
        if (strlen(key) + 1 > cap) {
            return false;
        }
        memcpy(out, key, strlen(key) + 1);
        return true;
    }
    /* "Bearer " + key + NUL */
    if (strlen(key) + 8 > cap) {
        return false;
    }
    memcpy(out, "Bearer ", 7);
    memcpy(out + 7, key, strlen(key) + 1);
    return true;
}

/* ---- JSON writing ---- */

/* Writes a JSON string, quotes and escapes included. False when it would not
 * fit; the buffer is left with what fitted. */
static bool json_string(char **p, char *end, const char *s)
{
    if (*p + 1 > end) {
        return false;
    }
    *(*p)++ = '"';
    for (const unsigned char *q = (const unsigned char *)(s ? s : ""); *q; q++) {
        char esc[8];
        const char *out = esc;
        size_t n = 1;
        switch (*q) {
        case '"':  out = "\\\""; n = 2; break;
        case '\\': out = "\\\\"; n = 2; break;
        case '\n': out = "\\n";  n = 2; break;
        case '\r': out = "\\r";  n = 2; break;
        case '\t': out = "\\t";  n = 2; break;
        case '\b': out = "\\b";  n = 2; break;
        case '\f': out = "\\f";  n = 2; break;
        default:
            if (*q < 0x20) {
                static const char hex[] = "0123456789abcdef";
                esc[0] = '\\';
                esc[1] = 'u';
                esc[2] = '0';
                esc[3] = '0';
                esc[4] = hex[(*q >> 4) & 0xf];
                esc[5] = hex[*q & 0xf];
                n = 6;
            } else {
                esc[0] = (char)*q;
                n = 1;
            }
            break;
        }
        if (*p + n > end) {
            return false;
        }
        memcpy(*p, out, n);
        *p += n;
    }
    if (*p + 1 > end) {
        return false;
    }
    *(*p)++ = '"';
    return true;
}

static bool json_raw(char **p, char *end, const char *raw)
{
    size_t n = strlen(raw);
    if (*p + n > end) {
        return false;
    }
    memcpy(*p, raw, n);
    *p += n;
    return true;
}

/* An empty voice is left out of the request, so the service uses its own default. */
bool muse_tts_build_body(muse_tts_api_t api, const char *model, const char *voice,
                         const char *text, const char *format, char *out, size_t cap)
{
    if (!out || !cap) {
        return false;
    }
    out[0] = '\0';
    char *p = out;
    char *end = out + cap - 1;   /* room for the NUL */
    bool have_voice = voice && voice[0];

    if (api == MUSE_TTS_API_GEMINI) {
        if (!json_raw(&p, end, "{\"contents\":[{\"parts\":[{\"text\":")) return false;
        if (!json_string(&p, end, text)) return false;
        if (!json_raw(&p, end, "}]}],\"generationConfig\":{\"responseModalities\":[\"AUDIO\"]")) return false;
        if (have_voice) {
            if (!json_raw(&p, end, ",\"speechConfig\":{\"voiceConfig\":{\"prebuiltVoiceConfig\":{\"voiceName\":")) return false;
            if (!json_string(&p, end, voice)) return false;
            if (!json_raw(&p, end, "}}}}}")) return false;
        } else {
            if (!json_raw(&p, end, "}}")) return false;
        }
    } else {
        if (!json_raw(&p, end, "{\"model\":")) return false;
        if (!json_string(&p, end, model)) return false;
        if (!json_raw(&p, end, ",\"input\":")) return false;
        if (!json_string(&p, end, text)) return false;
        if (have_voice) {
            if (!json_raw(&p, end, ",\"voice\":")) return false;
            if (!json_string(&p, end, voice)) return false;
        }
        if (format && format[0]) {
            if (!json_raw(&p, end, ",\"response_format\":")) return false;
            if (!json_string(&p, end, format)) return false;
        }
        if (!json_raw(&p, end, "}")) return false;
    }
    *p = '\0';
    return true;
}

/* ---- base64 ---- */

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

/*
 * Decodes base64 into out. Whitespace is skipped; '=' ends the data.
 * Returns 0 on success, 1 on a character that is not base64, 2 when the
 * output does not fit.
 */
static int b64_decode(const char *in, size_t len, uint8_t *out, size_t cap, size_t *out_len)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        char c = in[i];
        if (c == '=') {
            break;
        }
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            continue;
        }
        int v = b64_value(c);
        if (v < 0) {
            *out_len = n;
            return 1;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= cap) {
                *out_len = n;
                return 2;
            }
            out[n++] = (uint8_t)((acc >> bits) & 0xff);
        }
    }
    *out_len = n;
    return 0;
}

/* ---- reply reading ---- */

/* Finds `needle` in the body. */
static const char *find(const char *body, size_t len, const char *needle)
{
    size_t n = strlen(needle);
    if (!n || len < n) {
        return NULL;
    }
    for (size_t i = 0; i + n <= len; i++) {
        if (body[i] == needle[0] && memcmp(body + i, needle, n) == 0) {
            return body + i;
        }
    }
    return NULL;
}

/* Copies the string in "key":"value" into out, looking from `from` onwards
 * and no further than `window` bytes. Whitespace may sit around the colon.
 * False when there is no such field. */
static bool copy_field_from(const char *body, size_t len, const char *from, size_t window,
                             const char *key, char *out, size_t cap)
{
    if (!out || !cap) {
        return false;
    }
    out[0] = '\0';
    if (!from || from < body || from > body + len) {
        return false;
    }
    size_t avail = (size_t)(body + len - from);
    if (avail > window) {
        avail = window;
    }
    const char *end = from + avail;

    char pat[64];
    size_t k = strlen(key);
    if (k + 3 > sizeof(pat)) {
        return false;
    }
    pat[0] = '"';
    memcpy(pat + 1, key, k);
    pat[k + 1] = '"';
    pat[k + 2] = '\0';
    const char *p = find(from, avail, pat);
    if (!p) {
        return false;
    }
    p += k + 2;   /* past the key and its quotes */
    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }
    if (p >= end || *p != ':') {
        return false;
    }
    p++;
    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }
    if (p >= end || *p != '"') {
        return false;
    }
    p++;
    size_t o = 0;
    while (p < end && *p != '"') {
        if (*p == '\\' && p + 1 < end) {
            p++;
        }
        if (o + 1 < cap) {
            out[o++] = *p;
        }
        p++;
    }
    out[o] = '\0';
    return o > 0;
}

/* Copies the string in "key":"value" into out. False when there is none. */
static bool copy_field(const char *body, size_t len, const char *key, char *out, size_t cap)
{
    return copy_field_from(body, len, body, len, key, out, cap);
}

bool muse_tts_extract(muse_tts_api_t api, const uint8_t *body, size_t len,
                      uint8_t *out, size_t cap, muse_tts_reply_t *reply)
{
    if (!reply) {
        return false;
    }
    reply->bytes = 0;
    reply->truncated = false;
    reply->detail[0] = '\0';
    reply->mime[0] = '\0';

    if (!body || !len) {
        set_detail(reply->detail, sizeof(reply->detail), "empty");
        return false;
    }

    if (api != MUSE_TTS_API_GEMINI) {
        /* The OpenAI shape answers with the audio itself. */
        if (!out) {
            set_detail(reply->detail, sizeof(reply->detail), "no-buffer");
            return false;
        }
        size_t n = len < cap ? len : cap;
        memcpy(out, body, n);
        reply->bytes = n;
        reply->truncated = n < len;
        return true;
    }

    const char *text = (const char *)body;
    /* Gemini's audio: parts[].inlineData.data, base64. */
    const char *inline_data = find(text, len, "\"inlineData\"");
    /* The audio's own media type sits in the same block, before the data. It
     * is what says the audio is raw PCM and at what rate. */
    if (inline_data) {
        copy_field_from(text, len, inline_data, 256, "mimeType", reply->mime, sizeof(reply->mime));
    }
    const char *data_key = inline_data ? find(inline_data, (size_t)(text + len - inline_data), "\"data\"")
                                       : find(text, len, "\"data\"");
    if (!data_key) {
        if (!copy_field(text, len, "message", reply->detail, sizeof(reply->detail))) {
            set_detail(reply->detail, sizeof(reply->detail), "no-audio");
        }
        return false;
    }
    const char *p = data_key + 6;   /* past "\"data\"" */
    while (p < text + len && (*p == ' ' || *p == '\t')) {
        p++;
    }
    if (p >= text + len || *p != ':') {
        set_detail(reply->detail, sizeof(reply->detail), "no-audio");
        return false;
    }
    p++;
    while (p < text + len && (*p == ' ' || *p == '\t')) {
        p++;
    }
    if (p >= text + len || *p != '"') {
        set_detail(reply->detail, sizeof(reply->detail), "no-audio");
        return false;
    }
    p++;
    const char *start = p;
    while (p < text + len && *p != '"') {
        if (*p == '\\' && p + 1 < text + len) {
            p++;
        }
        p++;
    }
    size_t b64_len = (size_t)(p - start);

    /*
     * Decode where the base64 text is: the output always trails the input, so
     * it can never overwrite what has not been read yet. The audio then moves
     * to the front, so one buffer holds both the JSON and the audio.
     */
    size_t head = (size_t)(start - text);
    size_t room = (size_t)(text + len - start);
    if (cap < room) {
        room = cap;
    }
    size_t decoded = 0;
    int rc = b64_decode(start, b64_len, (uint8_t *)text + head, room, &decoded);
    if (rc) {
        if (!copy_field(text, len, "message", reply->detail, sizeof(reply->detail))) {
            set_detail(reply->detail, sizeof(reply->detail), rc == 2 ? "too-long" : "base64");
        }
        reply->truncated = rc == 2;
        return false;
    }
    memmove(out, text + head, decoded);
    reply->bytes = decoded;
    return true;
}

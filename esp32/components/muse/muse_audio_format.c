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
 * The rules, in order: an ID3 or MPEG frame-sync head is MP3 (minimp3 parses
 * its own rate and channels); RIFF/WAVE is WAV only if its fmt chunk is 16-bit
 * PCM and its data chunk is there; Ogg, FLAC and MP4 are named for the log;
 * bare PCM is believed when the Content-Type says so, and otherwise when the
 * bytes are not text. Anything else is "unknown" and is not played: an HTTP
 * 200 carrying a JSON error must not reach the speaker as noise.
 */

#include "muse_audio_format.h"

#include <ctype.h>
#include <string.h>

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool has(const uint8_t *data, size_t len, size_t at, const char *magic, size_t n)
{
    return at <= len && len - at >= n && memcmp(data + at, magic, n) == 0;
}

static void result(muse_audio_info_t *out, muse_audio_kind_t kind, uint32_t rate, uint8_t channels,
                   size_t offset, size_t length, const char *reason)
{
    out->kind = kind;
    out->rate = rate;
    out->channels = channels;
    out->offset = offset;
    out->length = length;
    out->reason = reason;
}

/* ---- WAV ---- */

/* fmt is PCM if it is WAVEFORMATEX PCM, or WAVE_FORMAT_EXTENSIBLE whose
 * sub-format GUID starts with PCM (1). Everything else is a codec minimp3
 * can't play. Returns NULL when the fmt chunk is good. */
static const char *wav_fmt_problem(const uint8_t *f, size_t avail, uint32_t *rate, uint8_t *channels)
{
    if (avail < 16) {
        return "wav-truncated";
    }
    uint16_t tag = rd16(f + 0);
    uint16_t ch = rd16(f + 2);
    uint32_t hz = rd32(f + 4);
    uint16_t bits = rd16(f + 14);
    if (tag == 0xFFFE) {
        /* cbSize(2) + validBits(2) + mask(4) + the 16-byte GUID: 22 bytes
         * past cbSize's own field, and the GUID's first two bytes are 1. */
        if (avail < 18 || rd16(f + 16) < 22) {
            return "wav-codec";
        }
        if (avail < 26 || rd16(f + 24) != 1) {
            return "wav-codec";
        }
    } else if (tag != 1) {
        return "wav-codec";
    }
    if (bits != 16 || (ch != 1 && ch != 2) || hz < 8000 || hz > 48000) {
        return "wav-codec";
    }
    *rate = hz;
    *channels = (uint8_t)ch;
    return NULL;
}

/* RIFF/WAVE: walk the chunks, then require both fmt and data. A data chunk
 * that claims more bytes than the buffer holds is clamped to the buffer. */
static void probe_wav(const uint8_t *data, size_t len, muse_audio_info_t *out)
{
    bool have_fmt = false, have_data = false;
    uint32_t rate = 0;
    uint8_t channels = 0;
    size_t data_at = 0, data_len = 0;

    size_t pos = 12;
    while (len - pos >= 8) {
        const uint8_t *h = data + pos;
        uint32_t size = rd32(h + 4);
        size_t body = pos + 8;
        size_t avail = len - body;   /* body <= len: the 8 header bytes are there */

        if (memcmp(h, "fmt ", 4) == 0) {
            const char *problem = wav_fmt_problem(h + 8, avail, &rate, &channels);
            if (problem) {
                result(out, MUSE_AUDIO_UNSUPPORTED, 0, 0, 0, 0, problem);
                return;
            }
            have_fmt = true;
        } else if (memcmp(h, "data", 4) == 0) {
            data_at = body;
            data_len = size > avail ? avail : size;
            have_data = true;
        }
        if (size > avail) {
            break;   /* nothing past this chunk is in the buffer */
        }
        size_t next = body + size + (size & 1u);   /* chunks are word-aligned */
        pos = next > len ? len : next;
    }
    if (!have_fmt || !have_data) {
        result(out, MUSE_AUDIO_UNSUPPORTED, 0, 0, 0, 0, "wav-truncated");
        return;
    }
    result(out, MUSE_AUDIO_WAV_PCM16, rate, channels, data_at, data_len, NULL);
}

/* ---- Content-Type ---- */

static bool ci_match(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (!ca || !cb) {
            return false;
        }
        if (ca >= 'A' && ca <= 'Z') {
            ca = (unsigned char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (unsigned char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

static bool ci_contains(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        if (ci_match(p, needle, n)) {
            return true;
        }
    }
    return false;
}

/* Finds `name=NUMBER` among the media type's parameters; false if absent. */
static bool ct_number(const char *ct, const char *name, uint32_t *out)
{
    size_t n = strlen(name);
    for (const char *p = ct; *p; p++) {
        if ((p != ct && p[-1] != ';' && p[-1] != ' ' && p[-1] != '\t') || !ci_match(p, name, n)) {
            continue;
        }
        const char *q = p + n;
        while (*q == ' ' || *q == '\t') {
            q++;
        }
        if (*q != '=') {
            continue;
        }
        q++;
        while (*q == ' ' || *q == '\t' || *q == '"') {
            q++;
        }
        if (*q < '0' || *q > '9') {
            continue;
        }
        uint32_t v = 0;
        while (*q >= '0' && *q <= '9') {
            if (v < 1000000) {
                v = v * 10 + (uint32_t)(*q - '0');
            }
            q++;
        }
        *out = v;
        return true;
    }
    return false;
}

/* ---- Entry point ---- */

/*
 * True when the body reads as text rather than audio: a JSON or HTML error
 * the service sent with HTTP 200, or a plain-text message. Raw PCM is
 * binary, so this is what keeps an error from reaching the speaker as noise.
 */
static bool looks_textual(const uint8_t *data, size_t len)
{
    size_t n = len < 64 ? len : 64;
    if (!n) {
        return true;
    }
    /* A JSON object or array, or a markup document, starts with a brace or an
     * angle bracket. */
    for (size_t i = 0; i < n; i++) {
        uint8_t c = data[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            continue;
        }
        if (c == '{' || c == '[' || c == '<') {
            return true;
        }
        break;
    }
    size_t printable = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = data[i];
        if (c == '\t' || c == '\r' || c == '\n' || (c >= 0x20 && c <= 0x7e)) {
            printable++;
        }
    }
    return printable * 10 >= n * 9;   /* nine tenths readable */
}

bool muse_audio_probe(const uint8_t *data, size_t len, const char *content_type,
                      muse_audio_info_t *out)
{
    if (!out) {
        return false;
    }
    result(out, MUSE_AUDIO_UNSUPPORTED, 0, 0, 0, 0, "empty");
    if (!data || !len) {
        return false;
    }

    /* MP3: an ID3 tag or an MPEG frame-sync head; minimp3 reads the rest. */
    if (has(data, len, 0, "ID3", 3) ||
        (len >= 2 && data[0] == 0xFF && (data[1] & 0xE0) == 0xE0)) {
        result(out, MUSE_AUDIO_MP3, 0, 0, 0, len, NULL);
        return true;
    }

    if (has(data, len, 0, "RIFF", 4) && has(data, len, 8, "WAVE", 4)) {
        probe_wav(data, len, out);
        return true;
    }

    static const struct {
        size_t at;
        const char *magic;
        const char *reason;
    } foreign[] = {
        { 0, "OggS", "ogg" },
        { 0, "fLaC", "flac" },
        { 4, "ftyp", "m4a" },
        { 0, "RIFF", "riff" },   /* a RIFF container that isn't WAVE */
    };
    for (size_t i = 0; i < sizeof(foreign) / sizeof(foreign[0]); i++) {
        if (has(data, len, foreign[i].at, foreign[i].magic, 4)) {
            result(out, MUSE_AUDIO_UNSUPPORTED, 0, 0, 0, 0, foreign[i].reason);
            return true;
        }
    }

    /* Bare PCM when the service says so: rate and channels come from the
     * parameters, with the phone-call defaults when they're missing or odd. */
    if (content_type && (ci_contains(content_type, "audio/l16") || ci_contains(content_type, "audio/pcm"))) {
        uint32_t rate = 24000, channels = 1;
        if (!ct_number(content_type, "rate", &rate) || rate < 8000 || rate > 48000) {
            rate = 24000;
        }
        if (!ct_number(content_type, "channels", &channels) || (channels != 1 && channels != 2)) {
            channels = 1;
        }
        result(out, MUSE_AUDIO_PCM16, rate, (uint8_t)channels, 0, len, NULL);
        return true;
    }

    /* A service that only speaks raw PCM and does not label it: believe the
     * bytes when they are not text. An HTTP 200 carrying a JSON error is
     * text, so it still never reaches the speaker. */
    if (!content_type || ci_contains(content_type, "octet-stream") ||
        ci_contains(content_type, "application/binary")) {
        if (!looks_textual(data, len)) {
            result(out, MUSE_AUDIO_PCM16, 24000, 1, 0, len, NULL);
            return true;
        }
        result(out, MUSE_AUDIO_UNSUPPORTED, 0, 0, 0, 0, "text");
        return true;
    }

    /* A service that answered with words instead of audio: a JSON error body
     * or a text reply. "text" says that, where "unknown" would leave the
     * setup page guessing. */
    if (content_type && (ci_contains(content_type, "application/json") ||
                         ci_contains(content_type, "text/"))) {
        result(out, MUSE_AUDIO_UNSUPPORTED, 0, 0, 0, 0, "text");
        return true;
    }

    result(out, MUSE_AUDIO_UNSUPPORTED, 0, 0, 0, 0, "unknown");
    return true;
}

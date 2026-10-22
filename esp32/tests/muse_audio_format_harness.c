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
 * Host checks for muse_audio_probe. Every fixture is copied into a heap block
 * of exactly its length, so the sanitizer fails the build on any read past
 * `len` as well as any other undefined behaviour.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_audio_format.h"

static int failures;

static void fail(const char *name, const char *what)
{
    fprintf(stderr, "%s: %s\n", name, what);
    failures++;
}

static size_t le16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    return 2;
}

static size_t le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    return 4;
}

/*
 * RIFF/WAVE with an optional JUNK chunk before fmt. `declared` is the size the
 * data chunk claims; `written` is what actually goes into the buffer, so a
 * value larger than the buffer exercises the clamp.
 */
static size_t build_wav(uint8_t *b, unsigned tag, unsigned channels, unsigned rate, unsigned bits,
                        size_t declared, size_t written, const uint8_t *junk, size_t junk_len)
{
    size_t n = 0;
    memcpy(b + n, "RIFF", 4);
    n += 4;
    n += le32(b + n, 0);   /* the probe reads len, not this field */
    memcpy(b + n, "WAVE", 4);
    n += 4;
    if (junk_len) {
        memcpy(b + n, "JUNK", 4);
        n += 4;
        n += le32(b + n, (uint32_t)junk_len);
        memcpy(b + n, junk, junk_len);
        n += junk_len;
        if (junk_len & 1) {
            b[n++] = 0;   /* word alignment */
        }
    }
    memcpy(b + n, "fmt ", 4);
    n += 4;
    if (tag == 0xFFFE) {
        n += le32(b + n, 40);
        n += le16(b + n, 0xFFFE);
        n += le16(b + n, channels);
        n += le32(b + n, rate);
        n += le32(b + n, rate * channels * 2);
        n += le16(b + n, channels * 2);
        n += le16(b + n, bits);
        n += le16(b + n, 22);   /* cbSize */
        n += le16(b + n, bits); /* valid bits */
        n += le32(b + n, 0);    /* channel mask */
        n += le16(b + n, 1);    /* the GUID's first two bytes: PCM */
        for (int i = 0; i < 14; i++) {
            b[n++] = 0;
        }
    } else {
        n += le32(b + n, 16);
        n += le16(b + n, tag);
        n += le16(b + n, channels);
        n += le32(b + n, rate);
        n += le32(b + n, rate * channels * 2);
        n += le16(b + n, channels * 2);
        n += le16(b + n, bits);
    }
    memcpy(b + n, "data", 4);
    n += 4;
    n += le32(b + n, (uint32_t)declared);
    for (size_t i = 0; i < written; i++) {
        b[n++] = (uint8_t)(i * 7 + 1);
    }
    return n;
}

static void expect_info(const char *name, const uint8_t *data, size_t len, const char *content_type,
                        muse_audio_kind_t kind, uint32_t rate, unsigned channels, size_t offset,
                        size_t length, const char *reason)
{
    uint8_t *copy = NULL;
    if (len) {
        copy = malloc(len);
        if (!copy) {
            fail(name, "out of memory");
            return;
        }
        memcpy(copy, data, len);
    }
    muse_audio_info_t info;
    memset(&info, 0x5a, sizeof(info));
    bool ok = muse_audio_probe(copy, len, content_type, &info);
    if (!ok) {
        fail(name, "probe returned false");
    } else if (info.kind != kind) {
        fprintf(stderr, "%s: kind %d, expected %d\n", name, (int)info.kind, (int)kind);
        failures++;
    } else if (info.rate != rate) {
        fprintf(stderr, "%s: rate %u, expected %u\n", name, (unsigned)info.rate, (unsigned)rate);
        failures++;
    } else if (info.channels != channels) {
        fprintf(stderr, "%s: channels %u, expected %u\n", name, (unsigned)info.channels, channels);
        failures++;
    } else if (info.offset != offset) {
        fprintf(stderr, "%s: offset %zu, expected %zu\n", name, info.offset, offset);
        failures++;
    } else if (info.length != length) {
        fprintf(stderr, "%s: length %zu, expected %zu\n", name, info.length, length);
        failures++;
    } else if (reason) {
        if (!info.reason || strcmp(info.reason, reason) != 0) {
            fprintf(stderr, "%s: reason %s, expected %s\n", name,
                    info.reason ? info.reason : "(null)", reason);
            failures++;
        }
    } else if (info.reason) {
        fprintf(stderr, "%s: unexpected reason %s\n", name, info.reason);
        failures++;
    }
    free(copy);
}

static void expect_empty(const uint8_t *data, size_t len, const char *what)
{
    muse_audio_info_t info;
    memset(&info, 0x5a, sizeof(info));
    if (muse_audio_probe(data, len, "audio/pcm", &info)) {
        fail(what, "probe returned true");
    } else if (info.kind != MUSE_AUDIO_UNSUPPORTED) {
        fail(what, "kind is not unsupported");
    } else if (!info.reason || strcmp(info.reason, "empty") != 0) {
        fail(what, "reason is not \"empty\"");
    } else if (info.rate || info.channels || info.offset || info.length) {
        fail(what, "fields are not zeroed");
    }
}

static void check_mp3(void)
{
    static const uint8_t id3[] = {
        'I', 'D', '3', 4, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    expect_info("id3", id3, sizeof(id3), "audio/mpeg", MUSE_AUDIO_MP3, 0, 0, 0, sizeof(id3), NULL);
    /* The Content-Type never overrides a magic head. */
    expect_info("id3_pcm_type", id3, sizeof(id3), "audio/pcm", MUSE_AUDIO_MP3, 0, 0, 0, sizeof(id3), NULL);

    static const uint8_t frame[] = { 0xFF, 0xFB, 0x90, 0x64, 0x00, 0x00, 0x00, 0x00 };
    expect_info("frame_sync", frame, sizeof(frame), NULL, MUSE_AUDIO_MP3, 0, 0, 0, sizeof(frame), NULL);
    static const uint8_t bare_sync[] = { 0xFF, 0xE0 };
    expect_info("sync_two_bytes", bare_sync, sizeof(bare_sync), NULL, MUSE_AUDIO_MP3, 0, 0, 0,
                sizeof(bare_sync), NULL);
}

static void check_wav(void)
{
    uint8_t buf[512];
    static const uint8_t junk[5] = { 0xde, 0xad, 0xbe, 0xef, 0x55 };

    size_t n = build_wav(buf, 1, 1, 24000, 16, 64, 64, NULL, 0);
    expect_info("wav_standard", buf, n, NULL, MUSE_AUDIO_WAV_PCM16, 24000, 1, 44, 64, NULL);
    expect_info("wav_standard_ct", buf, n, "audio/pcm", MUSE_AUDIO_WAV_PCM16, 24000, 1, 44, 64, NULL);

    n = build_wav(buf, 1, 2, 44100, 16, 8, 8, junk, sizeof(junk));
    /* 12 + JUNK(8 + 5 + 1 pad) + fmt(8 + 16) + data header(8) = 58. */
    expect_info("wav_junk", buf, n, NULL, MUSE_AUDIO_WAV_PCM16, 44100, 2, 58, 8, NULL);

    n = build_wav(buf, 1, 2, 16000, 16, 32, 32, NULL, 0);
    expect_info("wav_stereo", buf, n, NULL, MUSE_AUDIO_WAV_PCM16, 16000, 2, 44, 32, NULL);

    n = build_wav(buf, 0xFFFE, 1, 24000, 16, 16, 16, NULL, 0);
    expect_info("wav_extensible", buf, n, NULL, MUSE_AUDIO_WAV_PCM16, 24000, 1, 68, 16, NULL);

    /* A data chunk that claims more than the buffer holds is clamped. */
    n = build_wav(buf, 1, 1, 24000, 16, 4096, 16, NULL, 0);
    expect_info("wav_data_overrun", buf, n, NULL, MUSE_AUDIO_WAV_PCM16, 24000, 1, 44, 16, NULL);

    n = build_wav(buf, 1, 1, 24000, 8, 8, 8, NULL, 0);
    expect_info("wav_bits8", buf, n, NULL, MUSE_AUDIO_UNSUPPORTED, 0, 0, 0, 0, "wav-codec");

    n = build_wav(buf, 3, 1, 24000, 32, 8, 8, NULL, 0);
    expect_info("wav_float", buf, n, NULL, MUSE_AUDIO_UNSUPPORTED, 0, 0, 0, 0, "wav-codec");

    n = build_wav(buf, 1, 1, 96000, 16, 8, 8, NULL, 0);
    expect_info("wav_rate_96000", buf, n, NULL, MUSE_AUDIO_UNSUPPORTED, 0, 0, 0, 0, "wav-codec");

    n = build_wav(buf, 1, 3, 24000, 16, 8, 8, NULL, 0);
    expect_info("wav_three_channels", buf, n, NULL, MUSE_AUDIO_UNSUPPORTED, 0, 0, 0, 0, "wav-codec");

    /* Extensible, but the sub-format GUID says IEEE float. */
    n = build_wav(buf, 0xFFFE, 1, 24000, 16, 8, 8, NULL, 0);
    buf[44] = 3;
    buf[45] = 0;
    expect_info("wav_ext_float", buf, n, NULL, MUSE_AUDIO_UNSUPPORTED, 0, 0, 0, 0, "wav-codec");

    /* Only the 12-byte RIFF/WAVE head: no fmt, no data. */
    static const uint8_t head[12] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E' };
    expect_info("wav_truncated", head, sizeof(head), NULL, MUSE_AUDIO_UNSUPPORTED, 0, 0, 0, 0,
                "wav-truncated");

    /* A fmt chunk that claims 16 bytes with only 8 in the buffer. */
    uint8_t cut[32];
    memset(cut, 0, sizeof(cut));
    memcpy(cut, "RIFF", 4);
    memcpy(cut + 8, "WAVE", 4);
    memcpy(cut + 12, "fmt ", 4);
    le32(cut + 16, 16);
    le16(cut + 20, 1);
    le16(cut + 22, 1);
    expect_info("wav_fmt_cut", cut, sizeof(cut), NULL, MUSE_AUDIO_UNSUPPORTED, 0, 0, 0, 0,
                "wav-truncated");

    /* A RIFF container that isn't WAVE. */
    static const uint8_t avi[16] = { 'R', 'I', 'F', 'F', 4, 0, 0, 0, 'A', 'V', 'I', ' ', 'L', 'I', 'S', 'T' };
    expect_info("riff_avi", avi, sizeof(avi), NULL, MUSE_AUDIO_UNSUPPORTED, 0, 0, 0, 0, "riff");
}

static void check_foreign(void)
{
    static const uint8_t ogg[16] = { 'O', 'g', 'g', 'S', 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    expect_info("ogg", ogg, sizeof(ogg), "audio/ogg", MUSE_AUDIO_UNSUPPORTED, 0, 0, 0, 0, "ogg");

    static const uint8_t flac[8] = { 'f', 'L', 'a', 'C', 0, 0, 0, 34 };
    expect_info("flac", flac, sizeof(flac), "audio/flac", MUSE_AUDIO_UNSUPPORTED, 0, 0, 0, 0, "flac");

    static const uint8_t m4a[16] = { 0, 0, 0, 32, 'f', 't', 'y', 'p', 'M', '4', 'A', ' ', 0, 0, 0, 0 };
    expect_info("m4a", m4a, sizeof(m4a), "audio/mp4", MUSE_AUDIO_UNSUPPORTED, 0, 0, 0, 0, "m4a");

    /* A JSON error served with HTTP 200 must not become noise. */
    static const uint8_t json[] = "{\"error\":\"invalid api key\"}";
    expect_info("json_error", json, sizeof(json) - 1, "application/json", MUSE_AUDIO_UNSUPPORTED,
                0, 0, 0, 0, "text");
    expect_info("json_no_type", json, sizeof(json) - 1, NULL, MUSE_AUDIO_UNSUPPORTED, 0, 0, 0, 0,
                "text");
    /* The same body with a binary Content-Type: still text, still not played. */
    expect_info("json_binary_type", json, sizeof(json) - 1, "application/octet-stream",
                MUSE_AUDIO_UNSUPPORTED, 0, 0, 0, 0, "text");

    static const uint8_t html[] = "<html><body>Bad Gateway</body></html>";
    expect_info("html_binary_type", html, sizeof(html) - 1, "application/octet-stream",
                MUSE_AUDIO_UNSUPPORTED, 0, 0, 0, 0, "text");
    expect_info("html_json_type", html, sizeof(html) - 1, "application/json", MUSE_AUDIO_UNSUPPORTED,
                0, 0, 0, 0, "text");

    static const uint8_t plain[] = "Service temporarily unavailable";
    expect_info("text_binary_type", plain, sizeof(plain) - 1, "application/octet-stream",
                MUSE_AUDIO_UNSUPPORTED, 0, 0, 0, 0, "text");
    expect_info("text_no_type", plain, sizeof(plain) - 1, NULL, MUSE_AUDIO_UNSUPPORTED, 0, 0, 0, 0,
                "text");
    expect_info("text_plain_type", plain, sizeof(plain) - 1, "text/plain", MUSE_AUDIO_UNSUPPORTED,
                0, 0, 0, 0, "text");
}

static void check_pcm(void)
{
    uint8_t buf[32];
    for (size_t i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)(0x90 + i);
    }

    expect_info("l16", buf, sizeof(buf), "audio/l16;rate=24000;channels=1", MUSE_AUDIO_PCM16,
                24000, 1, 0, sizeof(buf), NULL);
    expect_info("pcm_case", buf, sizeof(buf), "Audio/PCM;RATE=48000;CHANNELS=2", MUSE_AUDIO_PCM16,
                48000, 2, 0, sizeof(buf), NULL);

    /* Out-of-range parameters fall back to the 24 kHz mono default. */
    expect_info("pcm_defaults", buf, sizeof(buf), "audio/l16;rate=96000;channels=3",
                MUSE_AUDIO_PCM16, 24000, 1, 0, sizeof(buf), NULL);
    expect_info("pcm_bare", buf, sizeof(buf), "audio/l16", MUSE_AUDIO_PCM16, 24000, 1, 0,
                sizeof(buf), NULL);
    expect_info("pcm_rate_8k", buf, sizeof(buf), "audio/pcm;rate=8000", MUSE_AUDIO_PCM16, 8000, 1, 0,
                sizeof(buf), NULL);

    /* Raw bytes with no Content-Type: the service did not say what it sent, so
     * they are taken as PCM at the phone-call defaults. */
    expect_info("raw_no_type", buf, sizeof(buf), NULL, MUSE_AUDIO_PCM16, 24000, 1, 0, sizeof(buf),
                NULL);
    expect_info("raw_json_type", buf, sizeof(buf), "application/octet-stream", MUSE_AUDIO_PCM16,
                24000, 1, 0, sizeof(buf), NULL);
    expect_info("raw_binary_type", buf, sizeof(buf), "application/binary", MUSE_AUDIO_PCM16, 24000, 1,
                0, sizeof(buf), NULL);

    /* A type that names a container the firmware cannot play is not guessed
     * away: an MP3 body is still an MP3, an unknown one stays unknown. */
    expect_info("raw_mp4_type", buf, sizeof(buf), "audio/mp4", MUSE_AUDIO_UNSUPPORTED, 0, 0, 0, 0,
                "unknown");
}

int main(void)
{
    check_mp3();
    check_wav();
    check_foreign();
    check_pcm();
    expect_empty(NULL, 0, "empty_null");
    static const uint8_t byte = 0;
    expect_empty(&byte, 0, "empty_zero_len");

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    puts("PASS muse_audio_format");
    return 0;
}

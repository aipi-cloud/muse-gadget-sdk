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
 * Host checks for the TTS request shapes and the reply reader. The fixtures
 * are the shapes the real services answer with: OpenAI's audio/speech returns
 * the audio itself, Google's generateContent returns JSON with the audio
 * base64 encoded inside.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_audio_format.h"
#include "muse_tts_protocol.h"

static int failures;

static void fail(const char *name, const char *what)
{
    fprintf(stderr, "%s: %s\n", name, what);
    failures++;
}

static void check_str(const char *name, const char *got, const char *want)
{
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "%s: got \"%s\", expected \"%s\"\n", name, got, want);
        failures++;
    }
}

/* Base64, so the fixtures can hold the audio the way a service sends it. */
static void b64_encode(const uint8_t *in, size_t len, char *out, size_t cap)
{
    static const char alpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        int have = 1;
        if (i + 1 < len) {
            v |= (uint32_t)in[i + 1] << 8;
            have++;
        }
        if (i + 2 < len) {
            v |= in[i + 2];
            have++;
        }
        for (int k = 0; k < 4; k++) {
            if (o + 1 >= cap) {
                out[o] = '\0';
                return;
            }
            out[o++] = (k <= have) ? alpha[(v >> (18 - 6 * k)) & 0x3f] : '=';
        }
    }
    out[o] = '\0';
}

static void check_api_parse(void)
{
    check_str("parse_gemini", muse_tts_api_parse("gemini") == MUSE_TTS_API_GEMINI ? "gemini" : "other", "gemini");
    check_str("parse_gemini_case", muse_tts_api_parse("GEMINI") == MUSE_TTS_API_GEMINI ? "gemini" : "other", "gemini");
    check_str("parse_openai", muse_tts_api_parse("openai") == MUSE_TTS_API_OPENAI ? "openai" : "other", "openai");
    check_str("parse_empty", muse_tts_api_parse("") == MUSE_TTS_API_OPENAI ? "openai" : "other", "openai");
    check_str("parse_null", muse_tts_api_parse(NULL) == MUSE_TTS_API_OPENAI ? "openai" : "other", "openai");
    check_str("parse_unknown", muse_tts_api_parse("whisper") == MUSE_TTS_API_OPENAI ? "openai" : "other", "openai");
}

static void check_auth(void)
{
    char v[64];
    check_str("header_openai", muse_tts_auth_header(MUSE_TTS_API_OPENAI), "Authorization");
    check_str("header_gemini", muse_tts_auth_header(MUSE_TTS_API_GEMINI), "x-goog-api-key");
    if (!muse_tts_auth_value(MUSE_TTS_API_OPENAI, "sk-abc", v, sizeof(v))) {
        fail("auth_openai", "returned false");
    } else {
        check_str("auth_openai_value", v, "Bearer sk-abc");
    }
    if (!muse_tts_auth_value(MUSE_TTS_API_GEMINI, "AIza123", v, sizeof(v))) {
        fail("auth_gemini", "returned false");
    } else {
        check_str("auth_gemini_value", v, "AIza123");
    }
    char small[8];
    if (muse_tts_auth_value(MUSE_TTS_API_OPENAI, "sk-0123456789", small, sizeof(small))) {
        fail("auth_small", "should not fit");
    }
}

static void check_body(void)
{
    char body[512];

    if (!muse_tts_build_body(MUSE_TTS_API_OPENAI, "gpt-4o-mini-tts", "alloy", "Hi.", "mp3", body, sizeof(body))) {
        fail("body_openai", "returned false");
    } else {
        check_str("body_openai", body,
                  "{\"model\":\"gpt-4o-mini-tts\",\"input\":\"Hi.\",\"voice\":\"alloy\","
                  "\"response_format\":\"mp3\"}");
    }
    if (!muse_tts_build_body(MUSE_TTS_API_OPENAI, "m", "v", "Hi.", NULL, body, sizeof(body))) {
        fail("body_openai_nofmt", "returned false");
    } else {
        check_str("body_openai_nofmt", body, "{\"model\":\"m\",\"input\":\"Hi.\",\"voice\":\"v\"}");
    }
    if (!muse_tts_build_body(MUSE_TTS_API_GEMINI, "gemini-3.8-flash-tts", "Kore", "Hi.", NULL, body, sizeof(body))) {
        fail("body_gemini", "returned false");
    } else {
        check_str("body_gemini", body,
                  "{\"contents\":[{\"parts\":[{\"text\":\"Hi.\"}]}],"
                  "\"generationConfig\":{\"responseModalities\":[\"AUDIO\"],"
                  "\"speechConfig\":{\"voiceConfig\":{\"prebuiltVoice\":{\"voiceName\":\"Kore\"}}}}}");
    }
    /* The text comes from a chat reply: quotes and newlines have to survive. */
    if (!muse_tts_build_body(MUSE_TTS_API_OPENAI, "m", "v", "say \"hi\"\nnow", "mp3", body, sizeof(body))) {
        fail("body_escape", "returned false");
    } else {
        check_str("body_escape", body,
                  "{\"model\":\"m\",\"input\":\"say \\\"hi\\\"\\nnow\",\"voice\":\"v\","
                  "\"response_format\":\"mp3\"}");
    }
    char tiny[16];
    if (muse_tts_build_body(MUSE_TTS_API_OPENAI, "m", "v", "Hi.", "mp3", tiny, sizeof(tiny))) {
        fail("body_tiny", "should not fit");
    }
}

/*
 * An empty voice name is left out of the request, so the service applies its
 * own default voice: an empty name is refused.
 */
static void check_body_empty_voice(void)
{
    char body[512];

    /* OpenAI: no "voice" member at all, and no comma left behind. */
    if (!muse_tts_build_body(MUSE_TTS_API_OPENAI, "m", "", "Hi.", NULL, body, sizeof(body))) {
        fail("body_openai_novoice", "returned false");
    } else {
        check_str("body_openai_novoice", body, "{\"model\":\"m\",\"input\":\"Hi.\"}");
        if (strstr(body, "\"voice\"")) {
            fail("body_openai_novoice", "the voice member is still there");
        }
        if (strstr(body, ",,") || strstr(body, ",}")) {
            fail("body_openai_novoice", "a stray comma is left behind");
        }
    }
    /* The rest of the request is unchanged: the format is still asked for. */
    if (!muse_tts_build_body(MUSE_TTS_API_OPENAI, "m", "", "Hi.", "mp3", body, sizeof(body))) {
        fail("body_openai_novoice_fmt", "returned false");
    } else {
        check_str("body_openai_novoice_fmt", body,
                  "{\"model\":\"m\",\"input\":\"Hi.\",\"response_format\":\"mp3\"}");
    }
    /* A NULL voice is the same as an empty one. */
    if (!muse_tts_build_body(MUSE_TTS_API_OPENAI, "m", NULL, "Hi.", NULL, body, sizeof(body))) {
        fail("body_openai_nullvoice", "returned false");
    } else {
        check_str("body_openai_nullvoice", body, "{\"model\":\"m\",\"input\":\"Hi.\"}");
    }

    /* Gemini: no speechConfig at all. */
    if (!muse_tts_build_body(MUSE_TTS_API_GEMINI, "gemini-3.8-flash-tts", "", "Hi.", NULL, body,
                             sizeof(body))) {
        fail("body_gemini_novoice", "returned false");
    } else {
        check_str("body_gemini_novoice", body,
                  "{\"contents\":[{\"parts\":[{\"text\":\"Hi.\"}]}],"
                  "\"generationConfig\":{\"responseModalities\":[\"AUDIO\"]}}");
        if (strstr(body, "speechConfig")) {
            fail("body_gemini_novoice", "speechConfig is still there");
        }
    }
}

static void check_extract_openai(void)
{
    static const uint8_t audio[] = { 'R', 'I', 'F', 'F', 1, 2, 3, 4 };
    uint8_t out[64];
    muse_tts_reply_t reply;
    memset(&reply, 0, sizeof(reply));
    if (!muse_tts_extract(MUSE_TTS_API_OPENAI, audio, sizeof(audio), out, sizeof(out), &reply)) {
        fail("extract_openai", "returned false");
    } else if (reply.bytes != sizeof(audio)) {
        fprintf(stderr, "extract_openai: %u bytes, expected %u\n", (unsigned)reply.bytes,
                (unsigned)sizeof(audio));
        failures++;
    } else if (memcmp(out, audio, sizeof(audio)) != 0) {
        fail("extract_openai", "the audio was not copied");
    }
    /* Too big for the buffer: what fits is kept, and the caller is told. */
    memset(&reply, 0, sizeof(reply));
    uint8_t small[4];
    if (!muse_tts_extract(MUSE_TTS_API_OPENAI, audio, sizeof(audio), small, sizeof(small), &reply)) {
        fail("extract_openai_small", "returned false");
    } else if (!reply.truncated || reply.bytes != sizeof(small)) {
        fail("extract_openai_small", "truncation was not reported");
    }
}

static void check_extract_gemini(void)
{
    static const uint8_t audio[] = { 'R', 'I', 'F', 'F', 0x24, 0x08, 0x00, 0x00, 'W', 'A', 'V', 'E' };
    char b64[64];
    b64_encode(audio, sizeof(audio), b64, sizeof(b64));

    char json[512];
    snprintf(json, sizeof(json),
             "{\"candidates\":[{\"content\":{\"parts\":[{\"inlineData\":{\"mimeType\":\"audio/wav\","
             "\"data\":\"%s\"}}]}}]}", b64);

    uint8_t out[64];
    muse_tts_reply_t reply;
    memset(&reply, 0, sizeof(reply));
    if (!muse_tts_extract(MUSE_TTS_API_GEMINI, (const uint8_t *)json, strlen(json), out, sizeof(out), &reply)) {
        fail("extract_gemini", "returned false");
    } else if (reply.bytes != sizeof(audio)) {
        fprintf(stderr, "extract_gemini: %u bytes, expected %u\n", (unsigned)reply.bytes,
                (unsigned)sizeof(audio));
        failures++;
    } else if (memcmp(out, audio, sizeof(audio)) != 0) {
        fail("extract_gemini", "the audio was not decoded");
    } else {
        /* The audio's own type comes back with it: it is what says the audio
         * is raw PCM and at what rate. */
        check_str("extract_gemini_mime", reply.mime, "audio/wav");
    }

    /* The older shape: the data key without inlineData. */
    snprintf(json, sizeof(json), "{\"audioContent\":{\"data\":\"%s\"}}", b64);
    memset(&reply, 0, sizeof(reply));
    if (!muse_tts_extract(MUSE_TTS_API_GEMINI, (const uint8_t *)json, strlen(json), out, sizeof(out), &reply)) {
        fail("extract_gemini_plain", "returned false");
    } else if (reply.bytes != sizeof(audio)) {
        fail("extract_gemini_plain", "wrong size");
    }

    /* An error reply: no audio, and the service's own message is kept. */
    const char *err = "{\"error\":{\"code\":400,\"message\":\"API key not valid\","
                      "\"status\":\"INVALID_ARGUMENT\"}}";
    memset(&reply, 0, sizeof(reply));
    if (muse_tts_extract(MUSE_TTS_API_GEMINI, (const uint8_t *)err, strlen(err), out, sizeof(out), &reply)) {
        fail("extract_gemini_error", "returned true");
    } else {
        check_str("extract_gemini_error", reply.detail, "API key not valid");
    }

    /* No audio and no message: the caller still gets a reason. */
    const char *bare = "{\"candidates\":[]}";
    memset(&reply, 0, sizeof(reply));
    if (muse_tts_extract(MUSE_TTS_API_GEMINI, (const uint8_t *)bare, strlen(bare), out, sizeof(out), &reply)) {
        fail("extract_gemini_bare", "returned true");
    } else {
        check_str("extract_gemini_bare", reply.detail, "no-audio");
    }

    /* More audio than the buffer holds. */
    snprintf(json, sizeof(json),
             "{\"candidates\":[{\"content\":{\"parts\":[{\"inlineData\":{\"data\":\"%s\"}}]}}]}", b64);
    uint8_t tiny[4];
    memset(&reply, 0, sizeof(reply));
    if (muse_tts_extract(MUSE_TTS_API_GEMINI, (const uint8_t *)json, strlen(json), tiny, sizeof(tiny), &reply)) {
        fail("extract_gemini_small", "returned true");
    } else if (!reply.truncated) {
        fail("extract_gemini_small", "truncation was not reported");
    }

    /* Nothing at all. */
    memset(&reply, 0, sizeof(reply));
    if (muse_tts_extract(MUSE_TTS_API_GEMINI, NULL, 0, out, sizeof(out), &reply)) {
        fail("extract_gemini_empty", "returned true");
    } else {
        check_str("extract_gemini_empty", reply.detail, "empty");
    }
}

/*
 * Google's reply says what the audio is in its own words. Without that, the
 * unpacked PCM is unlabelled and the probe has to guess, which is how a
 * Gemini reply ended up silent.
 */
static void check_gemini_pcm_chain(void)
{
    static const uint8_t pcm[] = { 0x00, 0x11, 0x22, 0x33, 0xFE, 0xFF, 0x80, 0x7F };
    char b64[64];
    b64_encode(pcm, sizeof(pcm), b64, sizeof(b64));

    char json[512];
    snprintf(json, sizeof(json),
             "{\"candidates\":[{\"content\":{\"parts\":[{\"inlineData\":{"
             "\"mimeType\":\"audio/L16;codec=pcm;rate=24000\",\"data\":\"%s\"}}]}}]}", b64);

    /* The whole reply lives in one buffer: the JSON is unpacked over itself. */
    uint8_t buf[512];
    size_t len = strlen(json);
    memcpy(buf, json, len + 1);

    muse_tts_reply_t reply;
    memset(&reply, 0, sizeof(reply));
    if (!muse_tts_extract(MUSE_TTS_API_GEMINI, buf, len, buf, sizeof(buf), &reply)) {
        fail("chain_gemini_pcm", "extract returned false");
        return;
    }
    check_str("chain_gemini_mime", reply.mime, "audio/L16;codec=pcm;rate=24000");
    if (reply.bytes != sizeof(pcm)) {
        fail("chain_gemini_pcm", "the audio is the wrong size");
        return;
    }
    if (memcmp(buf, pcm, sizeof(pcm)) != 0) {
        fail("chain_gemini_pcm", "the audio was not unpacked in place");
        return;
    }
    muse_audio_info_t info;
    memset(&info, 0, sizeof(info));
    if (!muse_audio_probe(buf, reply.bytes, reply.mime, &info)) {
        fail("chain_gemini_pcm", "probe returned false");
    } else if (info.kind != MUSE_AUDIO_PCM16) {
        fail("chain_gemini_pcm", "the reply was not recognised as PCM");
    } else if (info.rate != 24000 || info.channels != 1) {
        fail("chain_gemini_pcm", "the rate or the channels were not read");
    }

    /* Whitespace before the colon: a shape a service may answer with. */
    snprintf(json, sizeof(json),
             "{\"candidates\":[{\"content\":{\"parts\":[{\"inlineData\":{"
             "\"mimeType\" : \"audio/L16;codec=pcm;rate=24000\",\"data\":\"%s\"}}]}}]}", b64);
    uint8_t out[64];
    memset(&reply, 0, sizeof(reply));
    if (!muse_tts_extract(MUSE_TTS_API_GEMINI, (const uint8_t *)json, strlen(json), out, sizeof(out),
                          &reply)) {
        fail("chain_gemini_space", "extract returned false");
    } else {
        check_str("chain_gemini_space_mime", reply.mime, "audio/L16;codec=pcm;rate=24000");
    }
}

int main(void)
{
    check_api_parse();
    check_auth();
    check_body();
    check_body_empty_voice();
    check_extract_openai();
    check_extract_gemini();
    check_gemini_pcm_chain();

    /* The OpenAI shape has no buffer to write into: it says so. */
    {
        static const uint8_t audio[] = { 'R', 'I', 'F', 'F' };
        muse_tts_reply_t reply;
        memset(&reply, 0, sizeof(reply));
        if (muse_tts_extract(MUSE_TTS_API_OPENAI, audio, sizeof(audio), NULL, 0, &reply)) {
            fail("extract_openai_null", "returned true");
        }
    }

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    puts("PASS muse_tts_protocol");
    return 0;
}

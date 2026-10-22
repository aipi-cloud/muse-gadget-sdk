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
 * The Muse chat session: one HTTP-over-Noise connection to the VM (the transport
 * Home Link uses: TLS, WebSocket upgrade on /v1/noise, Noise handshake, then
 * multiplexed request streams), and the push-to-talk turns that run over it.
 *
 * A turn:
 *   1. POST /api/voice/dictation, request body left open; the mic is streamed
 *      up as 24 kHz PCM16 while the button is held, then half-closed. The VM
 *      answers with NDJSON transcripts (cumulative partials, then a final).
 *   2. POST /chat/stream with the transcript. The reply arrives as events on
 *      the connection's long-lived POST /chat/subscribe stream: one or more
 *      assistant messages, each delta.message_start / text_append / message_done.
 *   3. Each finished message is shown at reading pace (see start_tts to
 *      speak it with a TTS API of your own; Muse doesn't speak gadget replies).
 * A turn has no explicit end event; like Sidekick, it settles once every
 * message is done and nothing has arrived for a few seconds.
 *
 * Everything network-side runs on one task. The voice task talks to it through
 * a command queue, a stream buffer of mic audio, an event queue (captions) and
 * a stream buffer of reply audio. A generation number tags each turn so that
 * events and audio of a cancelled turn are dropped.
 *
 * A typed turn (muse_hatch_text_turn, from the serial console) skips steps 1
 * and 3: the text goes to /chat/stream and the reply streams back to the
 * console as "@chat" lines instead of to the voice task.
 */

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

extern "C" {
#include "cJSON.h"
#include "minimp3.h"
#include "muse_account_api.h"
#include "muse_audio_format.h"
#include "muse_link.h"
#include "muse_settings.h"
#include "muse_tts_protocol.h"
#include "muse_wifi.h"
#include "nvs.h"
}
#include "muse_chat_priv.h"

#include <xplat/noise/core/ClientSession.h>
#include <xplat/noise/core/PsaCryptoBackend.h>
#include <xplat/noise/core/ServiceCodec.h>

using namespace musegadgets::noise::core;

static const char *TAG = "muse_chat_session";

#define NOISE_PATH "/v1/noise"
#define NOISE_PORT 443
#define IO_TIMEOUT_US (15 * 1000000LL)

#define MIC_RATE 16000
#define DICT_RATE 24000
#define DICT_CHUNK_BYTES 8192              /* ~170 ms at 24 kHz, as hatch-test sends */
#define SCRATCH (64 * 1024)                /* inbound frames reassemble up to this */
#define NDJSON_LINE_MAX (16 * 1024)               /* one NDJSON line / the chat ack */
#define SUB_LINE_MAX (256 * 1024)          /* an event line grows its buffer up to this */
#define CHAT_PART (16 * 1024)              /* one body chunk of a long typed message */
/*
 * The reply buffer. A reply is held whole before it is played, and bare PCM
 * is six times the size of the same speech in MP3, so a board with PSRAM to
 * spare raises this (CONFIG_MUSE_TTS_REPLY_BUF_KB). The turn's buffer is
 * resident; a probe's is not.
 */
#ifdef CONFIG_MUSE_TTS_REPLY_BUF_KB
#define REPLY_BUF_KB CONFIG_MUSE_TTS_REPLY_BUF_KB
#else
#define REPLY_BUF_KB 512
#endif
#define TURN_AUDIO_BUF (REPLY_BUF_KB * 1024)   /* the turn's own reply buffer */
#define TTS_PROBE_BUF (256 * 1024)             /* ">tts.test" asks for one word */
#define MP3_HOLD (1441 + 4)               /* the largest MP3 frame and the next header */
#define TTS_PCM_CHUNK_SAMPLES 1152        /* raw replies: 16-bit samples folded per pass */
#define MP3_POLL_ROOM (SCRATCH + 8192)     /* stop reading the socket below this much MP3 room */
#define IN_BYTES (MIC_RATE * 2 * 8)        /* 8 s of mic backlog while connecting */
#define OUT_BYTES (MIC_RATE * 2 * 2)       /* 2 s of decoded reply */
#define EV_TEXT 72
#define TEXT_MAX 1024                      /* a message's text, for captions timed to its speech */
#define SPEECH_CHARS_PER_S 14              /* until the speech's length is known */
#define TEXT_CHARS_PER_S 16                /* speaker off: reading pace, a little over speech */
#define TEXT_HOLD_S 2                      /* speaker off: how long a message's last lines stay up */

#define PING_US (20 * 1000000LL)
#define DEAD_US (60 * 1000000LL)           /* nothing from the server, pongs included */
#define IDLE_CLOSE_US (10 * 60 * 1000000LL)
#define AUTO_RETRY_MIN_US (5 * 1000000LL)  /* connecting without a turn, after a failure */
#define AUTO_RETRY_MAX_US (120 * 1000000LL)
#define FINAL_TIMEOUT_US (15 * 1000000LL)  /* release -> final transcript */
#define REPLY_TIMEOUT_US (60 * 1000000LL)  /* chat posted -> first assistant message */
#define TURN_CAP_US (180 * 1000000LL)
#define SETTLE_US (3 * 1000000LL)          /* quiet period that ends a turn */
#define BUSY_HOLD_US (20 * 1000000LL)      /* how long a busy agent keeps it open */
#define TEXT_REPLY_TIMEOUT_US (5 * 60 * 1000000LL)   /* typed turns: agents can work a while */
#define TEXT_TURN_CAP_US (15 * 60 * 1000000LL)
#define TEXT_BUSY_HOLD_US (5 * 60 * 1000000LL)
/*
 * The VM's streaming dictation has no ASR behind it right now, so each press
 * goes to the chat as a voice note, the way the phone app sends them, and the
 * server transcribes it. Set to 0 to stream to /api/voice/dictation instead.
 */
#define VOICE_NOTE 1
#define NOTE_MAX_BYTES (MIC_RATE * 2 * 20) /* 20 s of 16 kHz PCM; Muse stops at 15 */
#define NOTE_PART_BYTES (DICT_CHUNK_BYTES / 4 * 3)   /* staged PCM that base64s to one body chunk */

#define MAX_MSGS 8

/* ---- Voice task <-> session task ---- */

enum cmd_type_t : uint8_t { CMD_CONNECT, CMD_FORGET, CMD_BEGIN, CMD_END, CMD_CANCEL, CMD_TEXT, CMD_TEXT_CANCEL, CMD_WAKE };

struct cmd_t {
    cmd_type_t type;
    uint32_t gen;
    char *text;              /* CMD_TEXT: malloc'd, freed by the session task */
};

struct ev_t {
    muse_hatch_ev_t type;
    uint32_t gen;
    char text[EV_TEXT];
};

static QueueHandle_t s_cmds, s_events;
static StreamBufferHandle_t s_in, s_out;
static std::atomic<uint32_t> s_gen{0};
static std::atomic<bool> s_resting{false};

/* ---- Connection ---- */

struct conn_t {
    esp_tls_t *tls;
    PsaCryptoBackend *crypto;
    ClientSession *session;
    uint8_t *ws, *rx, *tf, *sr, *svc, *env;   /* PSRAM scratch */
    int64_t next_id;
    int64_t sub_id;
    int64_t last_rx_us, last_ping_us, last_use_us;
};

static conn_t s_conn;
static bool s_connected;
static muse_hatch_vm_t s_vm;       /* cached per-VM credentials */
static bool s_vm_direct;           /* s_vm.vm_token is the device token itself */
static char s_host[MUSE_HOST_MAX + 1];
/* When to connect without a turn asking: once Wi-Fi is up, again with backoff
 * if that fails or the connection drops, not after an idle close. */
static int64_t s_auto_next_us;
static int64_t s_auto_backoff_us = AUTO_RETRY_MIN_US;

/* ---- Streams on the connection ---- */

enum kind_t : uint8_t { K_NONE, K_SUB, K_DICT, K_CHAT, K_TTS };

struct stream_t {
    int64_t id;
    kind_t kind;
    int status;
    int msg;                 /* K_TTS: index into the turn's messages */
    char *line;              /* NDJSON line / buffered body */
    size_t cap;              /* line's size: NDJSON_LINE_MAX, grown for long event lines */
    size_t len;
    bool overflow;
};

#define MAX_STREAMS 6
static stream_t s_streams[MAX_STREAMS];

/* ---- The current turn ---- */

enum phase_t : uint8_t { P_IDLE, P_LISTEN, P_WAIT_FINAL, P_WAIT_REPLY };

enum tts_t : uint8_t { TTS_NONE, TTS_QUEUED, TTS_ACTIVE, TTS_FINISHED };

struct msg_t {
    char id[80];
    size_t len;              /* reply text length so far */
    char tail[128];          /* its last characters, for the caption */
    bool done;
    tts_t tts;
    uint32_t pcm_start;      /* where its speech starts in the reply audio */
    uint32_t pcm_frames;     /* how long it is; 0 until the MP3 has all arrived */
};

struct resampler_t {
    uint32_t step;           /* Q16 input samples per output sample */
    uint32_t pos;
    int16_t prev;
};

struct turn_t {
    phase_t phase;
    uint32_t gen;
    bool text;               /* typed at the console: the reply goes there, unspoken */
    bool end_requested, end_sent, chat_posted, acked;
    int64_t dict_id, chat_id;
    int64_t start_us, end_sent_us, chat_us, last_event_us, last_content_us;
    uint64_t sent24;         /* 24 kHz frames sent to dictation */
    resampler_t up;
    uint8_t *chunk;          /* DICT_CHUNK_BYTES, plus the note's tail */
    size_t chunk_len;
    uint8_t *note;           /* NOTE_PART_BYTES of 16 kHz PCM waiting for base64 */
    size_t note_len;
    size_t pcm_bytes;        /* the note so far */
    size_t body_sent;
    char *texts;             /* MAX_MSGS * TEXT_MAX: each message's text */
    uint32_t pcm_out;        /* reply audio frames handed to the voice task */
    char committed[512];     /* finals that arrived before the half-close */
    char partial[512];
    char user_ids[2][80];
    muse_chat_rejected_t rejected;
    msg_t msgs[MAX_MSGS];
    int nmsgs;
    bool agent_busy;
    /* TTS */
    int tts_msg;             /* message being fetched (or shown, speaker off), or -1 */
    bool silent;             /* speaker off: tts_msg is paced by silence, not fetched */
    uint8_t *mp3;            /* TURN_AUDIO_BUF */
    size_t mp3_len;
    bool mp3_ended;
    mp3dec_t dec;
    resampler_t down;
    int kbps;
    int down_rate;
    /* TTS reply format, from muse_audio_probe: MP3 keeps the decoder path,
     * WAV/PCM16 are folded and resampled straight from mp3[]. */
    muse_audio_kind_t src_kind;
    uint32_t src_rate;
    uint8_t src_channels;
    size_t src_pos;          /* raw formats: bytes of mp3[] already played */
};

/* 10 KB, most of it the MP3 decoder: in PSRAM on boards that let static data go
 * there (the AIPI), so Wi-Fi setup and TLS have the internal RAM. */
EXT_RAM_BSS_ATTR static turn_t s_turn;

/* Turn milestones, logged together when the turn ends. */
enum mark_t : uint8_t { M_RELEASE, M_SENT, M_ACK, M_TEXT, M_DONE, M_TTS, M_MP3, M_AUDIO, M_COUNT };
static const char *const MARK_NAMES[M_COUNT] = { "release", "sent", "ack", "text", "done", "tts", "mp3", "audio" };
static int64_t s_marks[M_COUNT];
static char s_reply_shown[EV_TEXT];   /* the pre-speech caption last sent */

static void mark(mark_t m);
static bool open_note(void);
static void log_marks(void);
static int16_t *s_pcm;       /* MINIMP3_MAX_SAMPLES_PER_FRAME */
static int16_t *s_pcm16;     /* resampled output */
static int64_t s_last_seq;

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static void mark(mark_t m)
{
    if (!s_marks[m]) {
        s_marks[m] = now_us();
    }
}

static void log_marks(void)
{
    char line[160];
    int n = 0;
    for (int i = M_SENT; i < M_COUNT && n < (int)sizeof(line); i++) {
        if (s_marks[M_RELEASE] && s_marks[i]) {
            n += snprintf(line + n, sizeof(line) - n, " %s +%d", MARK_NAMES[i],
                          (int)((s_marks[i] - s_marks[M_RELEASE]) / 1000));
        }
    }
    ESP_LOGI(TAG, "timing (ms after release):%s", n ? line : " none");
}

static void *psram_alloc(size_t n)
{
    return heap_caps_malloc_prefer(n, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

static void *json_alloc(size_t n)
{
    return psram_alloc(n);
}

/* ---- Events to the voice task ---- */

static void emit(muse_hatch_ev_t type, const char *text)
{
    if (s_turn.text) {
        return;   /* typed turns report to the console instead */
    }
    ev_t ev = {};
    ev.type = type;
    ev.gen = s_turn.gen;
    if (type == MUSE_HATCH_EV_HEARD) {
        muse_hatch_tail_words(text ? text : "", ev.text, sizeof(ev.text));
    } else if (text) {
        strlcpy(ev.text, text, sizeof(ev.text));
    }
    /* Captions are lossy; the end of a turn, and whether the note got there, must get through. */
    TickType_t wait = (type == MUSE_HATCH_EV_DONE || type == MUSE_HATCH_EV_ERROR || type == MUSE_HATCH_EV_SENT)
                          ? pdMS_TO_TICKS(200)
                          : 0;
    xQueueSend(s_events, &ev, wait);
}

/* ---- TLS / WebSocket (from hatch-link's noise_control.cpp) ---- */

static bool write_all(esp_tls_t *tls, const void *buf, size_t len)
{
    const uint8_t *p = static_cast<const uint8_t *>(buf);
    int64_t deadline = now_us() + IO_TIMEOUT_US;
    while (len) {
        if (now_us() >= deadline) {
            return false;
        }
        ssize_t n = esp_tls_conn_write(tls, p, len);
        if (n > 0) {
            p += n;
            len -= n;
        } else if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(1);
        } else {
            ESP_LOGW(TAG, "tls write: %d (errno %d)", (int)n, errno);
            return false;
        }
    }
    return true;
}

/* Reads exactly len bytes, waiting up to the I/O timeout. */
static bool read_all(esp_tls_t *tls, uint8_t *buf, size_t len)
{
    int64_t deadline = now_us() + IO_TIMEOUT_US;
    while (len) {
        if (now_us() >= deadline) {
            return false;
        }
        ssize_t n = esp_tls_conn_read(tls, buf, len);
        if (n > 0) {
            buf += n;
            len -= n;
        } else if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(1);
        } else {
            return false;
        }
    }
    return true;
}

static bool ws_send_binary(esp_tls_t *tls, const uint8_t *payload, size_t len)
{
    uint8_t hdr[14];
    size_t hl;
    hdr[0] = 0x82;   /* FIN | binary */
    if (len < 126) {
        hdr[1] = 0x80 | len;
        hl = 2;
    } else if (len <= 65535) {
        hdr[1] = 0x80 | 126;
        hdr[2] = len >> 8;
        hdr[3] = len & 0xff;
        hl = 4;
    } else {
        hdr[1] = 0x80 | 127;
        for (int i = 0; i < 8; i++) {
            hdr[2 + i] = (uint64_t)len >> (56 - 8 * i);
        }
        hl = 10;
    }
    uint32_t key = esp_random();
    memcpy(hdr + hl, &key, 4);
    hl += 4;
    if (!write_all(tls, hdr, hl)) {
        return false;
    }
    const uint8_t *mk = reinterpret_cast<const uint8_t *>(&key);
    uint8_t chunk[512];
    for (size_t off = 0; off < len;) {
        size_t n = len - off < sizeof(chunk) ? len - off : sizeof(chunk);
        for (size_t i = 0; i < n; i++) {
            chunk[i] = payload[off + i] ^ mk[(off + i) & 3];
        }
        if (!write_all(tls, chunk, n)) {
            return false;
        }
        off += n;
    }
    return true;
}

static bool ws_send_ping(esp_tls_t *tls)
{
    uint8_t frame[6] = { 0x89, 0x80 };
    uint32_t key = esp_random();
    memcpy(frame + 2, &key, 4);
    return write_all(tls, frame, sizeof(frame));
}

/*
 * Reads one WebSocket frame. With `wait` false, returns -2 at once if nothing
 * is pending. Returns the payload length, -3 for a ping/pong (answered here),
 * 0 when the peer closed, -1 on error.
 */
static ssize_t ws_recv(esp_tls_t *tls, uint8_t *buf, size_t cap, bool wait)
{
    uint8_t hdr[2];
    int64_t deadline = now_us() + IO_TIMEOUT_US;
    for (;;) {
        ssize_t n = esp_tls_conn_read(tls, hdr, 1);
        if (n == 1) {
            break;
        }
        if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            if (!wait) {
                return -2;
            }
            if (now_us() >= deadline) {
                return -1;
            }
            vTaskDelay(1);
            continue;
        }
        return n == 0 ? 0 : -1;
    }
    if (!read_all(tls, hdr + 1, 1)) {
        return -1;
    }
    uint8_t opcode = hdr[0] & 0x0f;
    bool masked = hdr[1] & 0x80;
    uint64_t plen = hdr[1] & 0x7f;
    uint8_t ext[8];
    if (plen == 126) {
        if (!read_all(tls, ext, 2)) {
            return -1;
        }
        plen = (ext[0] << 8) | ext[1];
    } else if (plen == 127) {
        if (!read_all(tls, ext, 8)) {
            return -1;
        }
        plen = 0;
        for (int i = 0; i < 8; i++) {
            plen = (plen << 8) | ext[i];
        }
    }
    uint8_t mask[4] = { 0 };
    if (masked && !read_all(tls, mask, 4)) {
        return -1;
    }
    if (plen > cap) {
        ESP_LOGE(TAG, "WS frame too large: %llu", (unsigned long long)plen);
        return -1;
    }
    if (!read_all(tls, buf, plen)) {
        return -1;
    }
    if (masked) {
        for (size_t i = 0; i < plen; i++) {
            buf[i] ^= mask[i & 3];
        }
    }
    if (opcode == 0x8) {
        ESP_LOGW(TAG, "WS close frame");
        return -1;
    }
    if (opcode == 0x9) {
        uint8_t pong[6] = { 0x8A, (uint8_t)(0x80 | plen), 0, 0, 0, 0 };   /* mask key 0 */
        write_all(tls, pong, sizeof(pong));
        if (plen) {
            write_all(tls, buf, plen);
        }
        return -3;
    }
    if (opcode == 0xA) {
        return -3;
    }
    return (ssize_t)plen;
}

/* Query escaping as hatch-link's noise_upgrade.h: keeps RFC 3986 unreserved plus !~*'(). */
static void query_escape(const char *in, char *out, size_t cap)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < cap; in++) {
        unsigned char c = *in;
        if (isalnum(c) || strchr("-_.!~*'()", c)) {
            out[o++] = c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = '\0';
}

/* Returns the HTTP status of the upgrade (101 on success), or 0. */
static int ws_upgrade(esp_tls_t *tls, const char *vm_id, const char *token)
{
    char id[3 * 128 + 1];
    query_escape(vm_id, id, sizeof(id));
    size_t cap = 512 + strlen(id) + strlen(token) + strlen(s_host);
    char *req = static_cast<char *>(malloc(cap));
    if (!req) {
        return 0;
    }
    int len = snprintf(req, cap,
                       "GET " NOISE_PATH "?vm_id=%s HTTP/1.1\r\n"
                       "Host: %s\r\n"
                       "Authorization: Bearer %s\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n"
                       "Sec-WebSocket-Version: 13\r\n"
                       "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                       "\r\n",
                       id, s_host, token);
    bool sent = write_all(tls, req, len);
    free(req);
    if (!sent) {
        return 0;
    }
    /* Edge error responses carry a ~1.4 KB Proxy-Status header. */
    const size_t kCap = 4096;
    char *hdr = static_cast<char *>(malloc(kCap));
    if (!hdr) {
        return 0;
    }
    size_t have = 0;
    int status = 0;
    int64_t deadline = now_us() + IO_TIMEOUT_US;
    while (now_us() < deadline && have < kCap) {
        ssize_t n = esp_tls_conn_read(tls, hdr + have, kCap - have);
        if (n > 0) {
            size_t from = have >= 3 ? have - 3 : 0;
            have += n;
            bool end = false;
            for (size_t i = from; i + 3 < have; i++) {
                if (!memcmp(hdr + i, "\r\n\r\n", 4)) {
                    end = true;
                    break;
                }
            }
            if (end) {
                break;
            }
        } else if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(1);
        } else {
            break;
        }
    }
    if (have >= 12 && !memcmp(hdr, "HTTP/", 5)) {
        status = atoi(hdr + 9);
    }
    free(hdr);
    return status;
}

static bool noise_handshake(conn_t *c)
{
    ByteSpan out(c->ws, ClientSession::kMaxOutboundWebSocketPayloadSize);
    auto m1 = c->session->WriteHandshakeMessage1(out);
    if (!m1.ok() || !ws_send_binary(c->tls, c->ws, m1.size())) {
        ESP_LOGE(TAG, "handshake msg1 failed");
        return false;
    }
    ssize_t n = ws_recv(c->tls, c->rx, SCRATCH, true);
    if (n <= 0) {
        ESP_LOGE(TAG, "handshake msg2 not received (%d)", (int)n);
        return false;
    }
    uint8_t extra[256];
    size_t extra_len = 0;
    Status st = c->session->ReadHandshakeMessage2(ConstByteSpan(c->rx, n), ByteSpan(extra, sizeof(extra)), extra_len);
    if (!st.ok()) {
        ESP_LOGE(TAG, "handshake msg2: %s", st.str());
        return false;
    }
    /* The owner's msg3 payload is an empty protobuf; the bearer header authenticated us. */
    auto m3 = c->session->WriteHandshakeMessage3(ConstByteSpan(), out);
    if (!m3.ok() || !ws_send_binary(c->tls, c->ws, m3.size())) {
        ESP_LOGE(TAG, "handshake msg3 failed");
        return false;
    }
    return c->session->isEstablished();
}

static bool flush_outbound(conn_t *c)
{
    while (c->session->HasOutboundWebSocketPayload()) {
        auto r = c->session->WriteNextOutboundWebSocketPayload(
            ByteSpan(c->ws, ClientSession::kMaxOutboundWebSocketPayloadSize));
        if (!r.ok() || !ws_send_binary(c->tls, c->ws, r.size())) {
            ESP_LOGE(TAG, "send failed");
            return false;
        }
    }
    c->last_use_us = now_us();
    return true;
}

/* ---- Streams ---- */

static stream_t *find_stream(int64_t id)
{
    for (auto &s : s_streams) {
        if (s.kind != K_NONE && s.id == id) {
            return &s;
        }
    }
    return nullptr;
}

/* A handler may end a stream and a new one may take its slot. */
static bool alive(const stream_t *s, int64_t id)
{
    return s->kind != K_NONE && s->id == id;
}

static void close_stream(stream_t *s)
{
    if (s) {
        s->kind = K_NONE;
    }
}

static bool send_reset(int64_t id)
{
    if (!s_connected || id <= 0) {
        return true;
    }
    close_stream(find_stream(id));
    ResetView rv{ ResetCode::Cancelled, StringView("cancelled") };
    auto r = s_conn.session->StartOutboundReset(ServiceType::Daemon, id, rv, ByteSpan(s_conn.svc, SCRATCH),
                                                ByteSpan(s_conn.env, SCRATCH));
    return r.ok() && flush_outbound(&s_conn);
}

/* Opens a request stream. Returns its id, or 0 if the connection failed. */
static int64_t open_stream(kind_t kind, const char *verb, const char *path, const char *content_type,
                           const char *accept, const char *body, bool end_body)
{
    stream_t *slot = nullptr;
    for (auto &s : s_streams) {
        if (s.kind == K_NONE) {
            slot = &s;
            break;
        }
    }
    if (!slot) {
        ESP_LOGE(TAG, "no free stream slot");
        return 0;
    }
    char req_id[40];
    snprintf(req_id, sizeof(req_id), "muse-%08lx-%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    HeaderView hdrs[4];
    size_t nh = 0;
    hdrs[nh++] = { StringView("x-request-id"), StringView(req_id) };
    hdrs[nh++] = { StringView("x-app-id"), StringView("hatch-web") };
    if (content_type) {
        hdrs[nh++] = { StringView("Content-Type"), StringView(content_type) };
    }
    if (accept) {
        hdrs[nh++] = { StringView("accept"), StringView(accept) };
    }
    int64_t id = s_conn.next_id++;
    ApplicationRequestView req;
    req.verb = StringView(verb);
    req.path = StringView(path);
    req.headers = Span<const HeaderView>(hdrs, nh);
    req.body = body ? ConstByteSpan(reinterpret_cast<const uint8_t *>(body), strlen(body)) : ConstByteSpan();
    req.end_body = end_body;
    auto r = s_conn.session->StartOutboundApplicationRequest(ServiceType::Daemon, id, req,
                                                             ByteSpan(s_conn.svc, SCRATCH),
                                                             ByteSpan(s_conn.env, SCRATCH));
    if (!r.ok()) {
        ESP_LOGE(TAG, "%s %s: %s", verb, path, r.status().str());
        return 0;
    }
    if (!flush_outbound(&s_conn)) {
        return 0;
    }
    slot->id = id;
    slot->kind = kind;
    slot->status = 0;
    slot->msg = -1;
    slot->len = 0;
    slot->overflow = false;
    ESP_LOGI(TAG, "stream %lld: %s %s", (long long)id, verb, path);
    return id;
}

static bool send_body(int64_t id, const uint8_t *data, size_t len, bool end_body)
{
    BodyChunkView chunk{ ConstByteSpan(data, len), end_body };
    auto r = s_conn.session->StartOutboundBodyChunk(ServiceType::Daemon, id, chunk, ByteSpan(s_conn.svc, SCRATCH),
                                                    ByteSpan(s_conn.env, SCRATCH));
    if (!r.ok()) {
        ESP_LOGE(TAG, "body chunk: %s", r.status().str());
        return false;
    }
    return flush_outbound(&s_conn);
}

/* ---- Connect / disconnect ---- */

static void disconnect(const char *why)
{
    if (s_conn.tls) {
        ESP_LOGI(TAG, "disconnect: %s", why);
        esp_tls_conn_destroy(s_conn.tls);
    }
    if (s_conn.session) {
        s_conn.session->~ClientSession();
        heap_caps_free(s_conn.session);
    }
    if (s_conn.crypto) {
        s_conn.crypto->~PsaCryptoBackend();
        heap_caps_free(s_conn.crypto);
    }
    uint8_t *bufs[] = { s_conn.ws, s_conn.rx, s_conn.tf, s_conn.sr, s_conn.svc, s_conn.env };
    for (uint8_t *b : bufs) {
        heap_caps_free(b);
    }
    s_conn = conn_t{};
    for (auto &s : s_streams) {
        s.kind = K_NONE;
    }
    s_connected = false;
}

static void forget_vm(void)
{
    free(s_vm.vm_token);
    s_vm = muse_hatch_vm_t{};
    s_vm_direct = false;
}

/* Fills s_vm from the account API (or the token itself). */
static bool resolve_vm(char *err, size_t err_cap)
{
    if (s_vm.vm_token) {
        return true;
    }
    char want[MUSE_VM_MAX + 1];
    muse_settings_hatch_vm(want);
    if (!muse_settings_hatch_token_len()) {
        /* No token of our own: use Link's Hatch account. */
        if (muse_link_hatch_vm(want, s_vm.vm_id, sizeof(s_vm.vm_id), s_vm.vm_name, sizeof(s_vm.vm_name),
                               &s_vm.vm_token)) {
            ESP_LOGI(TAG, "VM %s (%s) via Link", s_vm.vm_id, s_vm.vm_name);
            return true;
        }
        strlcpy(err, muse_link_hatch_linked() ? "Can't reach Muse's server" : "Not paired", err_cap);
        return false;
    }
    char *token = static_cast<char *>(malloc(MUSE_TOKEN_MAX + 1));
    if (!token) {
        strlcpy(err, "Out of memory", err_cap);
        return false;
    }
    muse_settings_hatch_token(token);
    int rc = muse_hatch_api_find_vm(token, want, &s_vm);
    if (rc == 0) {
        ESP_LOGI(TAG, "VM %s (%s)", s_vm.vm_id, s_vm.vm_name);
        free(token);
        return true;
    }
    if (want[0]) {
        /* Not a device token (or the API is down): try it as the VM's own token. */
        ESP_LOGW(TAG, "account API %s; using the token for VM %s directly",
                 rc == MUSE_HATCH_API_AUTH ? "rejected the token" : "failed", want);
        strlcpy(s_vm.vm_id, want, sizeof(s_vm.vm_id));
        s_vm.vm_token = token;
        s_vm_direct = true;
        return true;
    }
    free(token);
    strlcpy(err, rc == MUSE_HATCH_API_AUTH ? "Token rejected" : "Can't reach Muse's server", err_cap);
    return false;
}

static bool open_subscription(void)
{
    s_last_seq = 0;
    s_conn.sub_id = open_stream(K_SUB, "POST", "/chat/subscribe", "application/json", "application/x-ndjson", "{}",
                                true);
    return s_conn.sub_id != 0;
}

static bool connect_once(char *err, size_t err_cap, int *http_status)
{
    *http_status = 0;
    conn_t &c = s_conn;
    c.tls = esp_tls_init();
    if (!c.tls) {
        strlcpy(err, "Out of memory", err_cap);
        return false;
    }
    esp_tls_cfg_t cfg = {};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 15000;
    int64_t t0 = now_us();
    if (esp_tls_conn_new_sync(s_host, strlen(s_host), NOISE_PORT, &cfg, c.tls) != 1) {
        snprintf(err, err_cap, "Can't reach %s", s_host);
        return false;
    }
    int status = ws_upgrade(c.tls, s_vm.vm_id, s_vm.vm_token);
    *http_status = status;
    if (status != 101) {
        ESP_LOGW(TAG, "upgrade rejected: HTTP %d", status);
        if (status == 401 || status == 403) {
            strlcpy(err, "VM refused the token", err_cap);
        } else {
            snprintf(err, err_cap, "VM connect failed (HTTP %d)", status);
        }
        return false;
    }
    void *mem = psram_alloc(sizeof(PsaCryptoBackend));
    c.crypto = mem ? new (mem) PsaCryptoBackend() : nullptr;
    mem = c.crypto ? psram_alloc(sizeof(ClientSession)) : nullptr;
    c.session = mem ? new (mem) ClientSession(*c.crypto) : nullptr;
    c.ws = static_cast<uint8_t *>(psram_alloc(ClientSession::kMaxOutboundWebSocketPayloadSize));
    c.rx = static_cast<uint8_t *>(psram_alloc(SCRATCH));
    c.tf = static_cast<uint8_t *>(psram_alloc(SCRATCH));
    c.sr = static_cast<uint8_t *>(psram_alloc(SCRATCH));
    c.svc = static_cast<uint8_t *>(psram_alloc(SCRATCH));
    c.env = static_cast<uint8_t *>(psram_alloc(SCRATCH));
    if (!c.session || !c.ws || !c.rx || !c.tf || !c.sr || !c.svc || !c.env) {
        strlcpy(err, "Out of memory", err_cap);
        return false;
    }
    if (!noise_handshake(&c)) {
        strlcpy(err, "Noise handshake failed", err_cap);
        return false;
    }
    int fd = -1;
    if (esp_tls_get_conn_sockfd(c.tls, &fd) != ESP_OK || fd < 0) {
        strlcpy(err, "Socket error", err_cap);
        return false;
    }
    lwip_fcntl(fd, F_SETFL, lwip_fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    c.next_id = 1;
    c.last_rx_us = c.last_ping_us = c.last_use_us = now_us();
    s_connected = true;
    if (!open_subscription()) {
        strlcpy(err, "Subscribe failed", err_cap);
        return false;
    }
    ESP_LOGI(TAG, "connected to VM %s in %d ms", s_vm.vm_id, (int)((now_us() - t0) / 1000));
    return true;
}

static bool ensure_connected(void)
{
    if (s_connected) {
        return true;
    }
    if (!muse_hatch_configured() || !muse_wifi_connected()) {
        return false;
    }
    muse_hatch_report(MUSE_HATCH_TESTING, "Connecting...");
    muse_settings_hatch_host(s_host);
    if (!s_host[0]) {
        strlcpy(s_host, "hatch.metaaivm.com", sizeof(s_host));
    }
    char err[48] = "";
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!resolve_vm(err, sizeof(err))) {
            break;
        }
        int status;
        if (connect_once(err, sizeof(err), &status)) {
            char detail[48] = "Connected to ";
            strlcat(detail, s_vm.vm_name[0] ? s_vm.vm_name : s_vm.vm_id, sizeof(detail));
            muse_hatch_report(MUSE_HATCH_REACHABLE, detail);
            return true;
        }
        disconnect(err);
        /* A refused VM token may just be stale: fetch a fresh one once. */
        if ((status == 401 || status == 403) && !s_vm_direct) {
            forget_vm();
            continue;
        }
        break;
    }
    ESP_LOGW(TAG, "connect failed: %s", err);
    muse_hatch_report(MUSE_HATCH_UNREACHABLE, err);
    return false;
}

/* ---- Resampling ---- */

static void resampler_init(resampler_t *r, int in_rate, int out_rate)
{
    r->step = (uint32_t)(((uint64_t)in_rate << 16) / out_rate);
    r->pos = 0;
    r->prev = 0;
}

/* Linear interpolation; state carries across calls. out must hold n*out/in + 2. */
static size_t resample(resampler_t *r, const int16_t *in, size_t n, int16_t *out)
{
    size_t o = 0;
    if (!n) {
        return 0;
    }
    /* Position 0 is the previous call's last sample, k is in[k-1]. */
    while ((r->pos >> 16) < n) {
        size_t i = r->pos >> 16;
        int32_t a = i ? in[i - 1] : r->prev;
        int32_t b = in[i];
        /* (b - a) spans 17 bits and the fraction 16, so the product needs 64. */
        out[o++] = (int16_t)(a + (int32_t)(((int64_t)(b - a) * (int64_t)(r->pos & 0xffff)) >> 16));
        r->pos += r->step;
    }
    r->pos -= n << 16;
    r->prev = in[n - 1];
    return o;
}

/* ---- Turn: dictation ---- */

static void turn_reset_streams(void)
{
    send_reset(s_turn.dict_id);
    send_reset(s_turn.chat_id);
    for (auto &s : s_streams) {
        if (s.kind == K_TTS) {
            send_reset(s.id);
        }
    }
}

static void turn_finish(void)
{
    turn_reset_streams();
    s_turn.phase = P_IDLE;
    s_turn.dict_id = s_turn.chat_id = 0;
    s_turn.tts_msg = -1;
    s_turn.silent = false;
    s_turn.mp3_len = 0;
}

static void turn_fail(const char *why)
{
    ESP_LOGW(TAG, "turn failed: %s", why);
    if (s_turn.text) {
        muse_hatch_console("error", why, nullptr);
    }
    emit(MUSE_HATCH_EV_ERROR, why);
    turn_finish();
}

/* Ends the turn once its reply is in; `complete` is false when it was cut off. */
static void turn_done(bool complete)
{
    if (s_turn.text) {
        muse_hatch_console("done", nullptr, "\"messages\":%d,\"complete\":%s", s_turn.nmsgs,
                           complete ? "true" : "false");
    }
    emit(MUSE_HATCH_EV_DONE, nullptr);
    turn_finish();
}

/* Ends any turn in progress and starts a fresh one. False if Hatch can't be reached. */
static bool turn_start(uint32_t gen, bool text)
{
    if (s_turn.phase != P_IDLE) {
        if (s_turn.text) {
            turn_fail("INTERRUPTED");
        } else {
            turn_finish();
        }
    }
    uint8_t *chunk = s_turn.chunk, *mp3 = s_turn.mp3, *note = s_turn.note;
    char *texts = s_turn.texts;
    s_turn = turn_t{};
    s_turn.chunk = chunk;
    s_turn.mp3 = mp3;
    s_turn.note = note;
    s_turn.texts = texts;
    s_turn.gen = gen;
    s_turn.text = text;
    s_turn.tts_msg = -1;
    memset(s_marks, 0, sizeof(s_marks));
    s_reply_shown[0] = '\0';
    s_turn.start_us = now_us();
    resampler_init(&s_turn.up, MIC_RATE, DICT_RATE);
    if (!ensure_connected()) {
        turn_fail(muse_hatch_configured() ? "CAN'T REACH MUSE" : "MUSE NOT SET UP");
        return false;
    }
    return true;
}

static void turn_begin(uint32_t gen)
{
    if (!turn_start(gen, false)) {
        return;
    }
    if (VOICE_NOTE) {
        if (!open_note()) {
            disconnect("chat open failed");
            turn_fail("CAN'T REACH MUSE");
            return;
        }
        s_turn.phase = P_LISTEN;
        return;
    }
    char path[64];
    snprintf(path, sizeof(path), "/api/voice/dictation?sample_rate_hz=%d", DICT_RATE);
    s_turn.dict_id = open_stream(K_DICT, "POST", path, nullptr, "application/x-ndjson", nullptr, false);
    if (!s_turn.dict_id) {
        disconnect("dictation open failed");
        turn_fail("CAN'T REACH MUSE");
        return;
    }
    s_turn.phase = P_LISTEN;
}

/* Moves mic audio to the dictation stream, paced like a live mic. */
static bool pump_mic(void)
{
    static int16_t in[MIC_RATE / 50];
    static int16_t up[MIC_RATE / 50 * DICT_RATE / MIC_RATE + 4];
    bool did = false;
    for (;;) {
        /* Never run more than a second ahead of real time: the ASR upstream drops floods. */
        double elapsed = (now_us() - s_turn.start_us) / 1e6;
        if ((s_turn.sent24 + DICT_CHUNK_BYTES / 2) / (double)DICT_RATE > 1.0 + 1.5 * elapsed) {
            return did;
        }
        size_t got = xStreamBufferReceive(s_in, in, sizeof(in), 0) / sizeof(int16_t);
        if (!got) {
            break;
        }
        did = true;
        size_t n = resample(&s_turn.up, in, got, up);
        const uint8_t *p = reinterpret_cast<const uint8_t *>(up);
        size_t bytes = n * sizeof(int16_t);
        while (bytes) {
            size_t take = DICT_CHUNK_BYTES - s_turn.chunk_len < bytes ? DICT_CHUNK_BYTES - s_turn.chunk_len : bytes;
            memcpy(s_turn.chunk + s_turn.chunk_len, p, take);
            s_turn.chunk_len += take;
            p += take;
            bytes -= take;
            if (s_turn.chunk_len == DICT_CHUNK_BYTES) {
                if (!send_body(s_turn.dict_id, s_turn.chunk, DICT_CHUNK_BYTES, false)) {
                    return false;
                }
                s_turn.sent24 += DICT_CHUNK_BYTES / 2;
                s_turn.chunk_len = 0;
            }
        }
    }
    if (s_turn.end_requested && !s_turn.end_sent) {
        if (s_turn.chunk_len && !send_body(s_turn.dict_id, s_turn.chunk, s_turn.chunk_len, false)) {
            return false;
        }
        s_turn.sent24 += s_turn.chunk_len / 2;
        s_turn.chunk_len = 0;
        if (!send_body(s_turn.dict_id, nullptr, 0, true)) {
            return false;
        }
        s_turn.end_sent = true;
        s_turn.end_sent_us = now_us();
        s_turn.phase = P_WAIT_FINAL;
        ESP_LOGI(TAG, "sent %.2fs of speech", (double)s_turn.sent24 / DICT_RATE);
    }
    return true;
}

/* ---- Turn: voice note ---- */

/* Base64-encodes the staged PCM into one body chunk; `last` pads and closes the request. */
static bool send_note_part(bool last)
{
    char *o = reinterpret_cast<char *>(s_turn.chunk);
    o += muse_hatch_base64(s_turn.note, s_turn.note_len, o);
    if (last) {
        memcpy(o, MUSE_HATCH_NOTE_TAIL, sizeof(MUSE_HATCH_NOTE_TAIL) - 1);
        o += sizeof(MUSE_HATCH_NOTE_TAIL) - 1;
    }
    size_t n = o - reinterpret_cast<char *>(s_turn.chunk);
    s_turn.body_sent += n;
    s_turn.note_len = 0;
    return send_body(s_turn.chat_id, s_turn.chunk, n, last);
}

/*
 * Opens the chat request at the press and streams the note while it's
 * recorded, so only the last chunk goes out after the release. The length
 * isn't known until the end, so the WAV header gives the streaming "unknown"
 * size and the server reads to the end of the data.
 */
static bool open_note(void)
{
    s_turn.chat_id = open_stream(K_CHAT, "POST", "/chat/stream", "application/json", nullptr, nullptr, false);
    if (!s_turn.chat_id) {
        return false;
    }
    s_turn.body_sent = sizeof(MUSE_HATCH_NOTE_HEAD) - 1;
    if (!send_body(s_turn.chat_id, reinterpret_cast<const uint8_t *>(MUSE_HATCH_NOTE_HEAD), sizeof(MUSE_HATCH_NOTE_HEAD) - 1, false)) {
        return false;
    }
    muse_hatch_wav_header(s_turn.note, MIC_RATE);
    s_turn.note_len = MUSE_HATCH_WAV_HEADER;
    return true;
}

/* Moves the mic into the note request; on release, sends the rest and waits for the reply. */
static bool record_note(void)
{
    for (;;) {
        size_t room = NOTE_PART_BYTES - s_turn.note_len;
        size_t left = NOTE_MAX_BYTES - s_turn.pcm_bytes;
        room = (room < left ? room : left) & ~(size_t)1;
        size_t got = room ? xStreamBufferReceive(s_in, s_turn.note + s_turn.note_len, room, 0) : 0;
        if (!got) {
            break;
        }
        s_turn.note_len += got;
        s_turn.pcm_bytes += got;
        if (s_turn.note_len == NOTE_PART_BYTES && !send_note_part(false)) {
            return false;
        }
    }
    if (!s_turn.end_requested && s_turn.pcm_bytes < NOTE_MAX_BYTES) {
        return true;
    }
    mark(M_RELEASE);
    double secs = (double)s_turn.pcm_bytes / (MIC_RATE * 2);
    if (secs < 0.3) {
        turn_fail("DIDN'T CATCH THAT");   /* resets the half-sent request */
        return true;
    }
    if (!send_note_part(true)) {
        return false;
    }
    ESP_LOGI(TAG, "voice note: %.2fs, %u byte request", secs, (unsigned)s_turn.body_sent);
    mark(M_SENT);
    s_turn.chat_posted = true;
    s_turn.chat_us = s_turn.last_event_us = now_us();
    s_turn.phase = P_WAIT_REPLY;
    return true;
}

/*
 * Posts `text` to the chat and waits for the reply. A long message goes up in
 * parts, since each frame has to fit SCRATCH.
 */
static void send_chat(const char *text, const char *modality)
{
    s_turn.chat_posted = true;
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "message", text);
    cJSON_AddStringToObject(body, "output_modality", modality);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    size_t len = json ? strlen(json) : 0;
    bool whole = len <= CHAT_PART;
    s_turn.chat_id = json ? open_stream(K_CHAT, "POST", "/chat/stream", "application/json", nullptr,
                                        whole ? json : nullptr, whole) : 0;
    bool ok = s_turn.chat_id != 0;
    for (size_t off = 0; ok && !whole && off < len; off += CHAT_PART) {
        size_t n = len - off < CHAT_PART ? len - off : CHAT_PART;
        ok = send_body(s_turn.chat_id, reinterpret_cast<const uint8_t *>(json) + off, n, off + n == len);
    }
    cJSON_free(json);
    if (!ok) {
        disconnect("chat/stream failed");
        turn_fail("CAN'T REACH MUSE");
        return;
    }
    s_turn.body_sent = len;
    s_turn.chat_us = s_turn.last_event_us = now_us();
    s_turn.phase = P_WAIT_REPLY;
}

static void post_chat(const char *text)
{
    if (s_turn.chat_posted) {
        return;
    }
    s_turn.chat_posted = true;
    send_reset(s_turn.dict_id);
    s_turn.dict_id = 0;
    while (*text == ' ') {
        text++;
    }
    if (!*text) {
        turn_fail("DIDN'T CATCH THAT");
        return;
    }
    ESP_LOGI(TAG, "heard: \"%s\"", text);
    emit(MUSE_HATCH_EV_HEARD, text);
    send_chat(text, "text");
}

/* A typed turn: the text goes straight to the chat. */
static void text_begin(const char *text)
{
    if (!turn_start(0, true)) {
        return;
    }
    ESP_LOGI(TAG, "typed turn: %u bytes", (unsigned)strlen(text));
    send_chat(text, "text");
    if (s_turn.phase == P_WAIT_REPLY) {
        mark(M_SENT);
        muse_hatch_console("sent", nullptr, "\"bytes\":%u", (unsigned)strlen(text));
    }
}

static void on_dictation_line(cJSON *line)
{
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(line, "type"));
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(line, "text"));
    if (!type) {
        return;
    }
    char heard[1024];
    if (!strcmp(type, "partial") && text) {
        strlcpy(s_turn.partial, text, sizeof(s_turn.partial));
        snprintf(heard, sizeof(heard), "%s%s%s", s_turn.committed, s_turn.committed[0] ? " " : "", text);
        emit(MUSE_HATCH_EV_HEARD, heard);
    } else if (!strcmp(type, "final")) {
        snprintf(heard, sizeof(heard), "%s%s%s", s_turn.committed, s_turn.committed[0] && text ? " " : "",
                 text ? text : "");
        s_turn.partial[0] = '\0';
        if (s_turn.end_sent) {
            post_chat(heard);
        } else {
            /* A mid-utterance segment; the rest follows. */
            strlcpy(s_turn.committed, heard, sizeof(s_turn.committed));
            emit(MUSE_HATCH_EV_HEARD, heard);
        }
    } else if (!strcmp(type, "error")) {
        char *s = cJSON_PrintUnformatted(line);
        ESP_LOGW(TAG, "dictation: %s", s ? s : "error");
        cJSON_free(s);
    }
}

static void on_dictation_end(bool ok)
{
    s_turn.dict_id = 0;
    if (s_turn.chat_posted || s_turn.phase == P_IDLE) {
        return;
    }
    if (!s_turn.end_sent) {
        turn_fail(ok ? "MUSE STOPPED LISTENING" : "MUSE COULDN'T LISTEN");
        return;
    }
    char heard[1024];
    snprintf(heard, sizeof(heard), "%s%s%s", s_turn.committed, s_turn.committed[0] ? " " : "", s_turn.partial);
    post_chat(heard);
}

/* ---- Turn: reply ---- */

static int find_msg(const char *id)
{
    for (int i = 0; i < s_turn.nmsgs; i++) {
        if (!strcmp(s_turn.msgs[i].id, id)) {
            return i;
        }
    }
    return -1;
}

static bool is_user_id(const char *id)
{
    return id && id[0] && (!strcmp(id, s_turn.user_ids[0]) || !strcmp(id, s_turn.user_ids[1]));
}

/* The message `id` if it belongs to this turn, binding it on first sight; else -1. */
static int bind_msg(const char *id, cJSON *payload)
{
    if (muse_chat_is_rejected(&s_turn.rejected, id)) {
        return -1;
    }
    int i = find_msg(id);
    if (i >= 0 || s_turn.phase != P_WAIT_REPLY) {
        return i;
    }
    const char *parent = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "reply_to_message_id"));
    if (!parent) {
        parent = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "parent_message_id"));
    }
    /* Once the ack names our message, replies to anything else are someone else's. */
    if (parent && parent[0] && s_turn.acked && !is_user_id(parent) && find_msg(parent) < 0) {
        muse_chat_reject(&s_turn.rejected, id);
        return -1;
    }
    if ((!parent || !parent[0]) && s_turn.rejected.overflow) {
        return -1;
    }
    if (s_turn.nmsgs == MAX_MSGS) {
        return -1;
    }
    msg_t &m = s_turn.msgs[s_turn.nmsgs];
    m = msg_t{};
    strlcpy(m.id, id, sizeof(m.id));
    return s_turn.nmsgs++;
}

static void append_text(msg_t &m, const char *text)
{
    size_t add = strlen(text);
    if (s_turn.texts) {
        char *full = s_turn.texts + (&m - s_turn.msgs) * TEXT_MAX;
        if (!m.len) {
            full[0] = '\0';
        }
        strlcat(full, text, TEXT_MAX);
    }
    m.len += add;
    size_t have = strlen(m.tail);
    if (add >= sizeof(m.tail) - 1) {
        strlcpy(m.tail, text + add - (sizeof(m.tail) - 1), sizeof(m.tail));
        return;
    }
    if (have + add >= sizeof(m.tail)) {
        size_t drop = have + add - (sizeof(m.tail) - 1);
        memmove(m.tail, m.tail + drop, have - drop + 1);
    }
    strlcat(m.tail, text, sizeof(m.tail));
}

/*
 * Shows a reply that hasn't started speaking: its opening lines, which the
 * speech starts with, so they can be read while the audio is on its way.
 * The spoken captions carry on from there.
 */
static void show_reply_start(const msg_t &m)
{
    char line[EV_TEXT];
    if (s_turn.texts) {
        if (!muse_hatch_caption_at(s_turn.texts + (&m - s_turn.msgs) * TEXT_MAX, 0, line, sizeof(line))) {
            return;
        }
    } else {
        muse_hatch_tail_words(m.tail, line, sizeof(line));
    }
    if (strcmp(line, s_reply_shown) != 0) {
        strlcpy(s_reply_shown, line, sizeof(s_reply_shown));
        emit(MUSE_HATCH_EV_REPLY, line);
    }
}

static void message_done(int i, const char *final_text)
{
    msg_t &m = s_turn.msgs[i];
    if (m.done) {
        return;
    }
    m.done = true;
    mark(M_DONE);
    if (s_turn.text) {
        /* The whole text if the pieces didn't add up to it (a line skipped, say): the reader uses it instead. */
        size_t n = m.len;
        if (final_text && final_text[0] && strlen(final_text) != m.len) {
            n = strlen(final_text);
            muse_hatch_console("final", final_text, "\"msg\":%d", i);
        }
        muse_hatch_console("message_done", nullptr, "\"msg\":%d,\"bytes\":%u", i, (unsigned)n);
        ESP_LOGI(TAG, "message %s done (%u chars)", m.id, (unsigned)n);
        return;
    }
    if (!m.len && final_text && final_text[0]) {
        append_text(m, final_text);
    }
    if (m.len && m.tts == TTS_NONE) {
        m.tts = TTS_QUEUED;
    }
    ESP_LOGI(TAG, "message %s done (%u chars)", m.id, (unsigned)m.len);
}

static const char *msg_id(cJSON *payload, cJSON *event)
{
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "message_id"));
    if (!id || !id[0]) {
        id = cJSON_GetStringValue(cJSON_GetObjectItem(event, "message_id"));
    }
    if (!id || !id[0]) {
        id = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "id"));
    }
    return id && id[0] ? id : nullptr;
}

static void on_event(cJSON *line)
{
    if (strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(line, "type")) ?: "", "event") != 0) {
        return;   /* the subscription ack */
    }
    cJSON *seq = cJSON_GetObjectItem(line, "seq");
    if (cJSON_IsNumber(seq)) {
        int64_t v = (int64_t)seq->valuedouble;
        if (v > 0 && v <= s_last_seq) {
            return;
        }
        s_last_seq = v > s_last_seq ? v : s_last_seq;
    }
    if (s_turn.phase != P_WAIT_REPLY) {
        return;
    }
    const char *event = cJSON_GetStringValue(cJSON_GetObjectItem(line, "event")) ?: "";
    cJSON *payload = cJSON_GetObjectItem(line, "payload");

    if (!strcmp(event, "agent.status") || !strcmp(event, "task.status")) {
        const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "activity_code"));
        const char *status = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "status"));
        bool was = s_turn.agent_busy;
        if (code) {
            s_turn.agent_busy = code[0] && strcmp(code, "online") && strcmp(code, "idle");
        } else if (status) {
            s_turn.agent_busy = status[0] && strcmp(status, "completed") && strcmp(status, "failed");
        }
        if (s_turn.text && s_turn.agent_busy != was) {
            muse_hatch_console("busy", nullptr, "\"on\":%s", s_turn.agent_busy ? "true" : "false");
        }
        s_turn.last_event_us = now_us();
        return;
    }
    bool start = !strcmp(event, "delta.message_start");
    bool append = !strcmp(event, "delta.text_append");
    bool done = !strcmp(event, "delta.message_done");
    bool full = !strcmp(event, "message.assistant");
    if (!start && !append && !done && !full) {
        return;
    }
    const char *id = msg_id(payload, line);
    int i = id ? bind_msg(id, payload) : -1;
    if (i < 0) {
        return;
    }
    s_turn.last_event_us = s_turn.last_content_us = now_us();
    msg_t &m = s_turn.msgs[i];
    if (append) {
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "text"));
        if (text && text[0] && s_turn.text) {
            mark(M_TEXT);
            m.len += strlen(text);
            muse_hatch_console("text", text, "\"msg\":%d", i);
        } else if (text && text[0]) {
            mark(M_TEXT);
            append_text(m, text);
            show_reply_start(m);   /* ignored once the speech starts */
        }
    } else if (done || full) {
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "display_text"));
        if (!text) {
            text = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "content"));
        }
        cJSON *ready = cJSON_GetObjectItem(payload, "display_text_ready");
        if (done || !cJSON_IsFalse(ready)) {
            message_done(i, text);
        }
    }
}

static void on_chat_ack(stream_t *s)
{
    s->line[s->len] = '\0';
    cJSON *root = cJSON_Parse(s->line);
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *obj = cJSON_IsObject(result) ? result : root;
    const char *keys[] = { "message_id", "reply_to_message_id" };
    for (int k = 0; k < 2; k++) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(obj, keys[k]));
        if (id) {
            strlcpy(s_turn.user_ids[k], id, sizeof(s_turn.user_ids[k]));
        }
    }
    s_turn.acked = true;
    mark(M_ACK);
    ESP_LOGI(TAG, "chat/stream ack: user message %s", s_turn.user_ids[0]);
    cJSON_Delete(root);
    emit(MUSE_HATCH_EV_SENT, nullptr);
}

/* ---- Turn: speech ---- */

static void tts_data(const uint8_t *data, size_t len);

/* ---- Speech: StepFun TTS (AIPI official build) ----
 *
 * Muse replies as text. To speak them, the message's text goes to StepFun's
 * /v1/audio/speech and the MP3 comes back whole: this blocks the session task
 * for a few seconds (well inside TURN_CAP_US and DEAD_US), then hands the
 * MP3 to decode() as a streamed one would. On any failure (offline, quota,
 * bad status) the turn falls back to reading-pace silence, so a broken TTS
 * never breaks the reply.
 */

/* Release builds ship these as placeholders: every owner brings their own TTS
 * service through ">tts.*=" over USB (FIRMWARE_CONTRACT.md §3); step_tts_fetch()
 * reads NVS first and falls back to these compiled-in values. */
#define STEP_TTS_URL ""
#define STEP_TTS_MODEL ""
/* No key ships in the firmware. Internal test builds fill this from a local,
 * uncommitted config; rotate the StepFun key if one ever leaks. */
#define STEP_TTS_KEY ""
#define STEP_TTS_VOICE ""
#define STEP_TTS_TIMEOUT_MS 20000

/* Last request's outcome, for ">tts?" and ">tts.test"; false/zero until one runs. */
static portMUX_TYPE s_tts_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_tts_last_ok;
static int32_t s_tts_last_ms;
static uint32_t s_tts_last_bytes;
static char s_tts_last_format[8];
static char s_tts_last_why[24];

/* One request at a time: a voice turn's fetch and a console probe would each
 * want a 512 KB reply buffer. The probe backs off ("busy"); a turn never does. */
static std::atomic<bool> s_tts_inflight{false};

struct tts_fail_t {
    const char *why;         /* "offline", "unconfigured", "connect", "http",
                                "empty", "format", "busy" */
    int status;              /* last HTTP status; 0 when none was received */
    char detail[128];        /* an HTTP error's body, or the reason to show */
};

/* The reply's Content-Type, captured as esp_http_client parses the headers.
 * This IDF only exposes response headers when they are saved (Kconfig), so an
 * event handler is the way to read just this one. */
struct tts_reply_t {
    char content_type[128];
};

static bool ci_equal(const char *a, const char *b)
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

static esp_err_t tts_on_http_event(esp_http_client_event_t *ev)
{
    auto *reply = static_cast<tts_reply_t *>(ev->user_data);
    if (reply && ev->event_id == HTTP_EVENT_ON_HEADER && ev->header_key && ev->header_value &&
        ci_equal(ev->header_key, "Content-Type")) {
        strlcpy(reply->content_type, ev->header_value, sizeof(reply->content_type));
    }
    return ESP_OK;
}

static const char *tts_format_name(muse_audio_kind_t kind)
{
    switch (kind) {
    case MUSE_AUDIO_MP3:
        return "mp3";
    case MUSE_AUDIO_WAV_PCM16:
        return "wav";
    case MUSE_AUDIO_PCM16:
        return "pcm";
    default:
        return "";
    }
}

/* What to remember as the failure reason: the probe's reason for a format we
 * can't play ("ogg", "wav-codec"), the short why otherwise. */
static const char *tts_reason(const tts_fail_t *fail)
{
    if (fail->why && !strcmp(fail->why, "format") && fail->detail[0]) {
        return fail->detail;
    }
    return fail->why ? fail->why : "";
}

static void record_tts(bool ok, int ms, size_t bytes, const char *format, const char *why)
{
    portENTER_CRITICAL(&s_tts_mux);
    s_tts_last_ok = ok;
    s_tts_last_ms = ms;
    s_tts_last_bytes = (uint32_t)bytes;
    strlcpy(s_tts_last_format, format ? format : "", sizeof(s_tts_last_format));
    strlcpy(s_tts_last_why, why ? why : "", sizeof(s_tts_last_why));
    portEXIT_CRITICAL(&s_tts_mux);
}

/* NVS value first, compiled-in macro second (FIRMWARE_CONTRACT.md §3). The
 * keys live in main/'s "homehub" namespace; see the note in muse_input.c. */
static void step_tts_field(const char *key, const char *fallback, char *out, size_t cap)
{
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open("homehub", NVS_READONLY, &h) == ESP_OK) {
        size_t len = cap;
        if (nvs_get_str(h, key, out, &len) != ESP_OK) {
            out[0] = '\0';
        }
        nvs_close(h);
    }
    if (!out[0]) {
        strlcpy(out, fallback, cap);
    }
}

/*
 * The request's working set: the URL, the key, the model and voice, the auth
 * value and the body. It is a few kilobytes, more than the console task's
 * stack can hold, so it lives in PSRAM for the length of the call.
 */
struct tts_work_t {
    char url[256];
    char key[256];
    char model[64];
    char voice[64];
    char api_name[16];
    char auth[264];   /* fits the 255-char key slot */
    char body[2048];
    char err[256];
};

/* A service that rejects a key often quotes it back. The key must not reach
 * the device's log or the setup page, so it is cut out of the error text. */
static void scrub_key(char *text, const char *key)
{
    if (!text || !key || !key[0]) {
        return;
    }
    size_t len = strlen(key);
    char *p = text;
    while ((p = strstr(p, key)) != nullptr) {
        memset(p, '*', len);
        p += len;
    }
}

/*
 * POSTs `text` to the configured TTS service and probes what came back. The
 * reply lands in `dst` (the caller's buffer, `dst_cap` bytes) and *out_len
 * says how much of it is audio; on failure *fail says why. The one request
 * path both the turn's fetch and the console's ">tts.test" use, so the two
 * can't drift apart.
 *
 * The service's shape is a setting ("tts_api"): the OpenAI-compatible
 * audio/speech shape, or Google's generateContent shape. The first asks for
 * mp3 and, when the service says it only speaks another format, asks again
 * without saying.
 */
static bool tts_request(const char *text, uint8_t *dst, size_t dst_cap, size_t *out_len,
                        muse_audio_info_t *info, tts_fail_t *fail, bool refuse_if_busy)
{
    if (!dst || !dst_cap || !out_len) {
        fail->why = "unconfigured";
        fail->status = 0;
        fail->detail[0] = '\0';
        return false;
    }
    *out_len = 0;
    *info = muse_audio_info_t{};
    fail->why = "connect";
    fail->status = 0;
    fail->detail[0] = '\0';

    bool owner = !s_tts_inflight.exchange(true);
    if (!owner && refuse_if_busy) {
        fail->why = "busy";
        strlcpy(fail->detail, "busy", sizeof(fail->detail));
        return false;
    }

    bool ok = false;
    uint8_t *buf = dst;
    size_t cap = dst_cap;
    esp_http_client_handle_t client = nullptr;
    tts_work_t *ws = (tts_work_t *)psram_alloc(sizeof(tts_work_t));
    if (!ws) {
        fail->why = "unconfigured";
        strlcpy(fail->detail, "no memory", sizeof(fail->detail));
        /* The flag was taken above. Leaving it set would make every later
         * request answer "busy" until the device is unplugged. */
        if (owner) {
            s_tts_inflight.store(false);
        }
        return false;
    }
    do {
        if (!muse_wifi_connected()) {
            fail->why = "offline";
            strlcpy(fail->detail, "offline", sizeof(fail->detail));
            break;
        }

        step_tts_field("tts_url", STEP_TTS_URL, ws->url, sizeof(ws->url));
        step_tts_field("tts_key", STEP_TTS_KEY, ws->key, sizeof(ws->key));
        step_tts_field("tts_model", STEP_TTS_MODEL, ws->model, sizeof(ws->model));
        step_tts_field("tts_voice", STEP_TTS_VOICE, ws->voice, sizeof(ws->voice));
        step_tts_field("tts_api", "", ws->api_name, sizeof(ws->api_name));
        if (!ws->url[0]) {
            fail->why = "unconfigured";
            strlcpy(fail->detail, "unconfigured", sizeof(fail->detail));
            break;
        }
        muse_tts_api_t api = muse_tts_api_parse(ws->api_name);

        if (!muse_tts_auth_value(api, ws->key, ws->auth, sizeof(ws->auth))) {
            fail->why = "unconfigured";
            strlcpy(fail->detail, "key too long", sizeof(fail->detail));
            break;
        }
        const char *auth_header = muse_tts_auth_header(api);

        /* The reply outlives the loop: the client is closed after the loop body
         * and its event handler still points at this struct. */
        tts_reply_t reply = {};
        for (int attempt = 0; attempt < 2; attempt++) {
            reply = {};
            /* mp3 first; a service that only speaks another format gets asked
             * again without the field, and answers in its own format. */
            const char *format = (api == MUSE_TTS_API_OPENAI && attempt == 0) ? "mp3" : nullptr;
            if (!muse_tts_build_body(api, ws->model, ws->voice, text, format, ws->body,
                                     sizeof(ws->body))) {
                fail->why = "unconfigured";
                strlcpy(fail->detail, "request too long", sizeof(fail->detail));
                break;
            }
            size_t body_len = strlen(ws->body);

            esp_http_client_config_t cfg = {};
            cfg.url = ws->url;
            cfg.method = HTTP_METHOD_POST;
            cfg.timeout_ms = STEP_TTS_TIMEOUT_MS;
            cfg.buffer_size = 4096;
            cfg.crt_bundle_attach = esp_crt_bundle_attach;
            cfg.event_handler = tts_on_http_event;
            cfg.user_data = &reply;
            client = esp_http_client_init(&cfg);
            if (!client) {
                strlcpy(fail->detail, "init", sizeof(fail->detail));
                break;
            }
            esp_http_client_set_header(client, auth_header, ws->auth);
            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_header(client, "Accept", "audio/*");
            if (esp_http_client_open(client, (int)body_len) != ESP_OK) {
                ESP_LOGW(TAG, "TTS: connect failed");
                strlcpy(fail->detail, "connect", sizeof(fail->detail));
                break;
            }
            int sent = esp_http_client_write(client, ws->body, body_len);
            if (sent != (int)body_len) {
                ESP_LOGW(TAG, "TTS: send failed (%d)", sent);
                strlcpy(fail->detail, "send", sizeof(fail->detail));
                esp_http_client_close(client);
                break;
            }
            esp_http_client_fetch_headers(client);
            int status = esp_http_client_get_status_code(client);
            fail->status = status;
            if (status != 200) {
                /* The whole body is read, not just the part that fits in the
                 * detail: "response_format" often sits past the first 120
                 * bytes, and that word is what asks for another try. */
                int n = esp_http_client_read(client, ws->err, sizeof(ws->err) - 1);
                if (n > 0) {
                    ws->err[n] = '\0';
                } else {
                    snprintf(ws->err, sizeof(ws->err), "HTTP %d", status);
                }
                /* The service may quote the key back in its error; it must not
                 * reach the device's log or the setup page. */
                scrub_key(ws->err, ws->key);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                client = nullptr;
                /* "response_format must be one of [wav]": the service speaks
                 * another format, so ask again without naming one. */
                if (attempt == 0 && api == MUSE_TTS_API_OPENAI &&
                    (status == 400 || status == 415 || status == 422) &&
                    strstr(ws->err, "response_format")) {
                    ESP_LOGW(TAG, "TTS: service wants another format, asking again");
                    continue;
                }
                fail->why = "http";
                ESP_LOGW(TAG, "TTS: HTTP %d %s", status, ws->err);
                strlcpy(fail->detail, ws->err, sizeof(fail->detail));
                break;
            }
            /* The reply lands in the caller's buffer: one copy, not two. */
            size_t got = 0;
            for (;;) {
                int n = esp_http_client_read(client, (char *)buf + got, (int)(cap - got));
                if (n <= 0) {
                    break;
                }
                got += (size_t)n;
                if (got >= cap) {
                    ESP_LOGW(TAG, "TTS: reply over %u bytes, truncated", (unsigned)cap);
                    break;
                }
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            client = nullptr;
            if (!got) {
                fail->why = "empty";
                strlcpy(fail->detail, "empty", sizeof(fail->detail));
                ESP_LOGW(TAG, "TTS: empty reply");
                break;
            }
            /* Google's shape wraps the audio in JSON, base64 encoded. The
             * unpacked audio replaces the JSON in the same buffer. */
            const char *ctype = reply.content_type[0] ? reply.content_type : nullptr;
            if (api == MUSE_TTS_API_GEMINI) {
                muse_tts_reply_t unpacked;
                if (!muse_tts_extract(api, buf, got, dst, dst_cap, &unpacked)) {
                    fail->why = "format";
                    strlcpy(fail->detail, unpacked.detail[0] ? unpacked.detail : "no-audio",
                            sizeof(fail->detail));
                    /* The service's own message may quote the key back. */
                    scrub_key(fail->detail, ws->key);
                    ESP_LOGW(TAG, "TTS: no audio in reply (%s)", fail->detail);
                    break;
                }
                got = unpacked.bytes;
                /* Google's own type says what the audio is; the HTTP type
                 * only says the body was JSON. */
                if (unpacked.mime[0]) {
                    ctype = unpacked.mime;
                }
            }
            /* Believe the bytes first, and the Content-Type only for bare PCM:
             * an HTTP 200 carrying a JSON error must never be played. */
            muse_audio_probe(buf, got, ctype, info);
            if (info->kind == MUSE_AUDIO_UNSUPPORTED) {
                fail->why = "format";
                strlcpy(fail->detail, info->reason ? info->reason : "unknown", sizeof(fail->detail));
                ESP_LOGW(TAG, "TTS: unsupported audio (%s)", info->reason ? info->reason : "unknown");
                break;
            }
            *out_len = got;
            ok = true;
            break;
        }
    } while (false);

    if (client) {
        esp_http_client_cleanup(client);
    }
    /* The key lives in this buffer: wipe it before it goes back to the heap. */
    memset(ws->key, 0, sizeof(ws->key));
    memset(ws->auth, 0, sizeof(ws->auth));
    free(ws);
    if (owner) {
        s_tts_inflight.store(false);
    }
    return ok;
}

static bool step_tts_fetch(const char *text, uint8_t *dst, size_t dst_cap,
                            size_t *out_len, muse_audio_info_t *info)
{
    tts_fail_t fail;
    int64_t t0 = now_us();
    bool ok = tts_request(text, dst, dst_cap, out_len, info, &fail, false);
    int ms = (int)((now_us() - t0) / 1000);
    if (!ok) {
        /* The reply still finishes at reading pace; this is why it has no voice. */
        ESP_LOGW(TAG, "TTS: %s (%s)", tts_reason(&fail), fail.detail);
        record_tts(false, ms, 0, "", tts_reason(&fail));
        return false;
    }
    mark(M_TTS);
    ESP_LOGI(TAG, "TTS: %u bytes, %s", (unsigned)*out_len, tts_format_name(info->kind));
    record_tts(true, ms, *out_len, tts_format_name(info->kind), "");
    return true;
}

/*
 * Console ">tts.test": one real request with the saved settings, with a fixed
 * short phrase, so the website can tell "saved" from "the service answers and
 * its audio is playable". Nothing is played and no turn state is touched.
 */
extern "C" bool muse_tts_probe(int *status_out, size_t *bytes_out, const char **format_out,
                               int *ms_out, char *detail, size_t detail_cap)
{
    if (status_out) {
        *status_out = 0;
    }
    if (bytes_out) {
        *bytes_out = 0;
    }
    if (format_out) {
        *format_out = "";
    }
    if (ms_out) {
        *ms_out = 0;
    }
    if (detail && detail_cap) {
        detail[0] = '\0';
    }

    uint8_t *audio = (uint8_t *)psram_alloc(TTS_PROBE_BUF);
    if (!audio) {
        if (detail && detail_cap) {
            strlcpy(detail, "no memory", detail_cap);
        }
        return false;
    }
    size_t len = 0;
    muse_audio_info_t info;
    tts_fail_t fail;
    int64_t t0 = now_us();
    bool ok = tts_request("Hi.", audio, TTS_PROBE_BUF, &len, &info, &fail, true);
    int ms = (int)((now_us() - t0) / 1000);
    free(audio);

    if (status_out) {
        *status_out = fail.status;
    }
    if (ms_out) {
        *ms_out = ms;
    }
    if (ok) {
        if (bytes_out) {
            *bytes_out = len;
        }
        if (format_out) {
            *format_out = tts_format_name(info.kind);
        }
        record_tts(true, ms, len, tts_format_name(info.kind), "");
        ESP_LOGI(TAG, "TTS probe: %u bytes, %s, %d ms", (unsigned)len, tts_format_name(info.kind), ms);
        return true;
    }
    /* An unplayable format reports the probe's reason as the format ("ogg"). */
    if (format_out && info.reason) {
        *format_out = info.reason;
    }
    if (detail && detail_cap) {
        strlcpy(detail, fail.detail[0] ? fail.detail : tts_reason(&fail), detail_cap);
    }
    record_tts(false, ms, 0, "", tts_reason(&fail));
    ESP_LOGW(TAG, "TTS probe: %s (%s), HTTP %d", tts_reason(&fail), fail.detail, fail.status);
    return false;
}

/* The last request's outcome, as one JSON object for ">tts?"'s "last". */
extern "C" void muse_tts_last_json(char *out, size_t cap)
{
    if (!out || !cap) {
        return;
    }
    bool ok;
    int32_t ms;
    uint32_t bytes;
    char format[sizeof(s_tts_last_format)];
    char why[sizeof(s_tts_last_why)];
    portENTER_CRITICAL(&s_tts_mux);
    ok = s_tts_last_ok;
    ms = s_tts_last_ms;
    bytes = s_tts_last_bytes;
    memcpy(format, s_tts_last_format, sizeof(format));
    memcpy(why, s_tts_last_why, sizeof(why));
    portEXIT_CRITICAL(&s_tts_mux);
    snprintf(out, cap, "{\"ok\":%s,\"format\":\"%s\",\"bytes\":%u,\"ms\":%d,\"why\":\"%s\"}",
             ok ? "true" : "false", format, (unsigned)bytes, (int)ms, why);
}

static void start_tts(void)
{
    if (s_turn.tts_msg >= 0) {
        return;
    }
    for (int i = 0; i < s_turn.nmsgs; i++) {
        msg_t &m = s_turn.msgs[i];
        if (m.tts != TTS_QUEUED) {
            continue;
        }
        /* Speak the reply: fetch its audio (above) before showing the message,
         * then play it exactly like a streamed one. Any fetch failure falls
         * back to reading-pace silence, so the reply always finishes. */
        char *text = s_turn.texts ? s_turn.texts + (size_t)i * TEXT_MAX : nullptr;
        /* The turn's own buffer takes the reply: one copy, not two. */
        s_turn.mp3_len = 0;
        s_turn.mp3_ended = false;
        s_turn.kbps = 0;
        s_turn.down_rate = 0;
        s_turn.src_pos = 0;
        muse_audio_info_t info = {};
        size_t reply_len = 0;
        bool speech = text && text[0] &&
                      step_tts_fetch(text, s_turn.mp3, TURN_AUDIO_BUF, &reply_len, &info);
        if (speech && info.kind != MUSE_AUDIO_MP3 && info.rate < 8000) {
            speech = false;   /* the probe guarantees a rate; never let 0 reach the resampler */
        }

        m.pcm_start = s_turn.pcm_out;
        m.pcm_frames = 0;
        m.tts = TTS_ACTIVE;
        s_turn.tts_msg = i;
        s_turn.silent = !speech;
        s_turn.src_kind = speech ? info.kind : MUSE_AUDIO_UNSUPPORTED;
        s_turn.src_rate = speech ? info.rate : 0;
        s_turn.src_channels = speech ? info.channels : 0;
        mp3dec_init(&s_turn.dec);
        if (speech) {
            /* The buffer holds the whole reply: the data chunk for WAV, the
             * whole body for bare PCM, from its first byte for MP3. */
            s_turn.mp3_len = info.offset + info.length;
            s_turn.src_pos = info.offset;
            s_turn.mp3_ended = true;
            ESP_LOGI(TAG, "speaking message %s (%u chars)", m.id, (unsigned)m.len);
        } else {
            m.pcm_frames = (uint32_t)(m.len * MIC_RATE / TEXT_CHARS_PER_S);
            ESP_LOGI(TAG, "showing message %s (%u chars)", m.id, (unsigned)m.len);
        }
        show_reply_start(m);
        return;
    }
}

static void tts_data(const uint8_t *data, size_t len)
{
    if (s_turn.mp3_len + len > TURN_AUDIO_BUF) {
        ESP_LOGW(TAG, "MP3 buffer full, dropping %u bytes", (unsigned)len);
        len = TURN_AUDIO_BUF - s_turn.mp3_len;
    }
    mark(M_MP3);
    memcpy(s_turn.mp3 + s_turn.mp3_len, data, len);
    s_turn.mp3_len += len;
}

static void tts_end(stream_t *s, bool ok)
{
    int i = s->msg;
    close_stream(s);
    if (i < 0 || i != s_turn.tts_msg) {
        return;
    }
    if (ok) {
        s_turn.mp3_ended = true;   /* decode() drains the rest, then finishes */
    } else {
        s_turn.msgs[i].tts = TTS_FINISHED;
        s_turn.tts_msg = -1;
    }
}

/* Speaker off: queues the shown message's silence while the reply buffer has room. */
static void pace_silently(void)
{
    static const int16_t zeros[256] = {};
    msg_t &m = s_turn.msgs[s_turn.tts_msg];
    uint32_t end = m.pcm_start + m.pcm_frames + TEXT_HOLD_S * MIC_RATE;
    while (s_turn.pcm_out < end && xStreamBufferSpacesAvailable(s_out) >= sizeof(zeros)) {
        uint32_t n = end - s_turn.pcm_out < 256 ? end - s_turn.pcm_out : 256;
        xStreamBufferSend(s_out, zeros, n * sizeof(int16_t), 0);
        s_turn.pcm_out += n;
    }
    if (s_turn.pcm_out >= end) {
        m.tts = TTS_FINISHED;
        s_turn.tts_msg = -1;
        s_turn.silent = false;
    }
}

/* Decodes buffered MP3 while the reply buffer has room. */
static void decode_pcm(void);

static void decode(void)
{
    if (s_turn.tts_msg < 0) {
        return;
    }
    if (s_turn.silent) {
        pace_silently();
        return;
    }
    if (s_turn.src_kind != MUSE_AUDIO_MP3) {
        decode_pcm();
        return;
    }
    /*
     * minimp3 only takes a frame once it can see the next one's header. Given
     * less, it resets and says to skip all of it, which drops speech and clicks.
     * So until the stream ends, leave the last MP3_HOLD bytes for more to arrive.
     */
    size_t hold = s_turn.mp3_ended ? 0 : MP3_HOLD;
    size_t off = 0;
    while (s_turn.mp3_len - off > hold &&
           xStreamBufferSpacesAvailable(s_out) >= (MINIMP3_MAX_SAMPLES_PER_FRAME / 2 + 8) * sizeof(int16_t)) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&s_turn.dec, s_turn.mp3 + off, s_turn.mp3_len - off, s_pcm, &info);
        if (!info.frame_bytes) {
            if (s_turn.mp3_ended) {
                off = s_turn.mp3_len;   /* trailing junk */
            }
            break;
        }
        off += info.frame_bytes;
        if (!samples) {
            continue;
        }
        if (info.channels == 2) {
            for (int k = 0; k < samples; k++) {
                s_pcm[k] = (s_pcm[2 * k] + s_pcm[2 * k + 1]) / 2;
            }
        }
        if (s_turn.down_rate != info.hz) {
            ESP_LOGI(TAG, "reply audio: %d Hz, %d ch, %d kbps", info.hz, info.channels, info.bitrate_kbps);
            s_turn.down_rate = info.hz;
            resampler_init(&s_turn.down, info.hz, MIC_RATE);
        }
        size_t n = resample(&s_turn.down, s_pcm, samples, s_pcm16);
        if (s_turn.gen == s_gen.load()) {
            mark(M_AUDIO);
            xStreamBufferSend(s_out, s_pcm16, n * sizeof(int16_t), 0);
        }
        s_turn.pcm_out += n;
        s_turn.kbps = info.bitrate_kbps;
    }
    if (off) {
        memmove(s_turn.mp3, s_turn.mp3 + off, s_turn.mp3_len - off);
        s_turn.mp3_len -= off;
    }
    msg_t &m = s_turn.msgs[s_turn.tts_msg];
    if (s_turn.mp3_ended && s_turn.kbps > 0) {
        /* All of it is here: what's decoded plus what the bitrate says the rest holds. */
        uint32_t rest = (uint32_t)((uint64_t)s_turn.mp3_len * 8 * MIC_RATE / (s_turn.kbps * 1000));
        m.pcm_frames = s_turn.pcm_out - m.pcm_start + rest;
    }
    if (s_turn.mp3_ended && !s_turn.mp3_len) {
        m.pcm_frames = s_turn.pcm_out - m.pcm_start;
        m.tts = TTS_FINISHED;
        s_turn.tts_msg = -1;
    }
}

/* Plays a buffered WAV/PCM16 reply: up to 1152 interleaved 16-bit samples a
 * pass, stereo folded to mono, resampled from the probed rate to MIC_RATE. The
 * whole reply is already in s_turn.mp3, so the byte cursor is all the state:
 * it starts at the data chunk and ends at the end of the reply.
 * The flow control is the MP3 path's: stop while the reply buffer is full. */
static void decode_pcm(void)
{
    size_t frame_bytes = (size_t)(s_turn.src_channels ? s_turn.src_channels : 1) * sizeof(int16_t);
    /* Upsampling writes more samples than it reads, so the chunk is limited to
     * what the output buffer holds: at most MIC_RATE/src_rate times the input,
     * so the resampler can never write past s_pcm16. */
    size_t max_mono = TTS_PCM_CHUNK_SAMPLES;
    if (s_turn.src_rate && s_turn.src_rate < MIC_RATE) {
        max_mono = (size_t)((uint64_t)TTS_PCM_CHUNK_SAMPLES * s_turn.src_rate / MIC_RATE);
        if (!max_mono) {
            max_mono = 1;
        }
    }
    while (s_turn.mp3_len - s_turn.src_pos >= frame_bytes &&
           xStreamBufferSpacesAvailable(s_out) >= (MINIMP3_MAX_SAMPLES_PER_FRAME + 8) * sizeof(int16_t)) {
        size_t avail = (s_turn.mp3_len - s_turn.src_pos) / sizeof(int16_t);
        size_t mono = (s_turn.src_channels == 2) ? avail / 2 : avail;
        if (mono > max_mono) {
            mono = max_mono;
        }
        size_t samples = (s_turn.src_channels == 2) ? mono * 2 : mono;
        if (!samples) {
            break;
        }
        /* A data chunk can start at any offset, so copy instead of casting. */
        memcpy(s_pcm, s_turn.mp3 + s_turn.src_pos, samples * sizeof(int16_t));
        if (s_turn.src_channels == 2) {
            for (size_t k = 0; k < mono; k++) {
                s_pcm[k] = (int16_t)((s_pcm[2 * k] + s_pcm[2 * k + 1]) / 2);
            }
        }
        if (s_turn.down_rate != (int)s_turn.src_rate) {
            ESP_LOGI(TAG, "reply audio: %u Hz, %u ch, PCM", (unsigned)s_turn.src_rate,
                     (unsigned)s_turn.src_channels);
            s_turn.down_rate = (int)s_turn.src_rate;
            resampler_init(&s_turn.down, (int)s_turn.src_rate, MIC_RATE);
        }
        size_t n = resample(&s_turn.down, s_pcm, mono, s_pcm16);
        if (s_turn.gen == s_gen.load()) {
            mark(M_AUDIO);
            xStreamBufferSend(s_out, s_pcm16, n * sizeof(int16_t), 0);
        }
        s_turn.pcm_out += (uint32_t)n;
        s_turn.src_pos += samples * sizeof(int16_t);
    }
    msg_t &m = s_turn.msgs[s_turn.tts_msg];
    m.pcm_frames = s_turn.pcm_out - m.pcm_start;   /* actual frames, no estimate */
    if (s_turn.mp3_len - s_turn.src_pos < frame_bytes) {
        s_turn.src_pos = s_turn.mp3_len;   /* a trailing partial frame is not playable */
    }
    if (s_turn.src_pos >= s_turn.mp3_len) {
        m.tts = TTS_FINISHED;
        s_turn.tts_msg = -1;
    }
}

/* Ends the turn once the reply is complete and spoken, or on timeouts. */
static void check_turn(void)
{
    int64_t t = now_us();
    if (s_turn.phase == P_WAIT_FINAL && t - s_turn.end_sent_us > FINAL_TIMEOUT_US) {
        on_dictation_end(true);
        return;
    }
    if (s_turn.phase != P_WAIT_REPLY) {
        return;
    }
    bool text = s_turn.text;
    if (t - s_turn.start_us > (text ? TEXT_TURN_CAP_US : TURN_CAP_US)) {
        ESP_LOGW(TAG, "turn hit the time cap");
        /* A voice turn that waited out the cap on a busy agent got no reply at all. */
        if (!text && !s_turn.nmsgs) {
            turn_fail("NO REPLY FROM MUSE");
            return;
        }
        turn_done(false);
        return;
    }
    if (!s_turn.nmsgs) {
        /* Wait as long as the agent says it's working; the turn cap still applies. */
        if (t - s_turn.chat_us > (text ? TEXT_REPLY_TIMEOUT_US : REPLY_TIMEOUT_US) && !s_turn.agent_busy) {
            turn_fail("NO REPLY FROM MUSE");
        }
        return;
    }
    for (int i = 0; i < s_turn.nmsgs; i++) {
        if (!s_turn.msgs[i].done || s_turn.msgs[i].tts == TTS_QUEUED || s_turn.msgs[i].tts == TTS_ACTIVE) {
            return;
        }
    }
    if (t - s_turn.last_event_us < SETTLE_US) {
        return;
    }
    if (s_turn.agent_busy && t - s_turn.last_content_us < (text ? TEXT_BUSY_HOLD_US : BUSY_HOLD_US)) {
        return;
    }
    ESP_LOGI(TAG, "turn done: %d message(s) in %.1fs", s_turn.nmsgs, (t - s_turn.start_us) / 1e6);
    log_marks();
    turn_done(true);
}

/* ---- Inbound dispatch ---- */

/* Splits NDJSON; calls fn for each complete line. */
static void feed_lines(stream_t *s, const uint8_t *data, size_t len, void (*fn)(cJSON *))
{
    int64_t id = s->id;
    for (size_t i = 0; i < len; i++) {
        char ch = data[i];
        if (ch != '\n') {
            if (s->len == s->cap - 1 && !s->overflow && s->cap < SUB_LINE_MAX) {
                /* A long reply's done event carries its whole text. */
                char *grown = static_cast<char *>(heap_caps_realloc(s->line, s->cap * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (grown) {
                    s->line = grown;
                    s->cap *= 2;
                }
            }
            if (s->len < s->cap - 1) {
                s->line[s->len++] = ch;
            } else {
                s->overflow = true;
            }
            continue;
        }
        if (s->overflow) {
            ESP_LOGW(TAG, "stream %lld: skipped a line over %u bytes", (long long)s->id, (unsigned)s->cap);
        } else if (s->len) {
            cJSON *j = cJSON_ParseWithLength(s->line, s->len);
            if (j) {
                fn(j);
                cJSON_Delete(j);
            }
        }
        if (!alive(s, id)) {
            return;   /* fn ended the stream */
        }
        s->len = 0;
        s->overflow = false;
    }
}

static void stream_data(stream_t *s, ConstByteSpan data)
{
    if (data.empty()) {
        return;
    }
    switch (s->kind) {
    case K_SUB:
        feed_lines(s, data.data(), data.size(), on_event);
        break;
    case K_DICT:
        feed_lines(s, data.data(), data.size(), on_dictation_line);
        break;
    case K_CHAT: {
        size_t take = data.size() < s->cap - 1 - s->len ? data.size() : s->cap - 1 - s->len;
        memcpy(s->line + s->len, data.data(), take);
        s->len += take;
        break;
    }
    case K_TTS:
        if (s->msg == s_turn.tts_msg) {
            tts_data(data.data(), data.size());
        }
        break;
    default:
        break;
    }
}

/* Returns false when the connection has to go. */
static bool stream_end(stream_t *s, bool ok)
{
    switch (s->kind) {
    case K_SUB:
        ESP_LOGW(TAG, "subscription ended");
        return false;
    case K_DICT:
        close_stream(s);
        on_dictation_end(ok);
        break;
    case K_CHAT:
        if (ok) {
            on_chat_ack(s);
        }
        close_stream(s);
        s_turn.chat_id = 0;
        if (!ok) {
            turn_fail("MUSE DIDN'T TAKE IT");
        }
        break;
    case K_TTS:
        tts_end(s, ok);
        break;
    default:
        break;
    }
    return true;
}

static bool on_http_error(stream_t *s, const ApplicationResponseView &resp)
{
    char body[160];
    size_t n = resp.body.size() < sizeof(body) - 1 ? resp.body.size() : sizeof(body) - 1;
    memcpy(body, resp.body.data(), n);
    body[n] = '\0';
    ESP_LOGW(TAG, "stream %lld: HTTP %d %s", (long long)s->id, (int)resp.status, body);
    return stream_end(s, false);
}

static bool on_frame(const DecodedServiceFrame &f)
{
    stream_t *s = find_stream(f.stream_id);
    if (!s) {
        return true;
    }
    switch (f.kind) {
    case ServiceFrameKind::Response:
        s->status = f.response.status;
        if (f.response.status >= 400) {
            return on_http_error(s, f.response);
        }
        stream_data(s, f.response.body);
        if (f.response.end_body && alive(s, f.stream_id)) {
            return stream_end(s, true);
        }
        break;
    case ServiceFrameKind::BodyChunk:
        stream_data(s, f.body_chunk.data);
        if (f.body_chunk.end_body && alive(s, f.stream_id)) {
            return stream_end(s, true);
        }
        break;
    case ServiceFrameKind::Reset:
        ESP_LOGW(TAG, "stream %lld reset: %.*s", (long long)f.stream_id, (int)f.reset.reason.size(),
                 f.reset.reason.data());
        return stream_end(s, false);
    default:
        break;
    }
    return true;
}

/* Handles whatever the server sent. Returns false if the connection failed. */
static bool poll_socket(void)
{
    static HeaderView hdrs[16];
    for (int budget = 0; budget < 8; budget++) {
        /* Hold off while the MP3 buffer is nearly full: TCP pushes back on the VM. */
        if (s_turn.tts_msg >= 0 && TURN_AUDIO_BUF - s_turn.mp3_len < MP3_POLL_ROOM) {
            return true;
        }
        ssize_t n = ws_recv(s_conn.tls, s_conn.rx, SCRATCH, false);
        if (n == -2) {
            return true;
        }
        if (n == 0 || n == -1) {
            ESP_LOGW(TAG, "%s", n ? "receive error" : "server closed the connection");
            return false;
        }
        s_conn.last_rx_us = now_us();
        if (n == -3) {
            continue;
        }
        auto in = s_conn.session->ProcessInboundWebSocketPayload(ConstByteSpan(s_conn.rx, n),
                                                                 ByteSpan(s_conn.tf, SCRATCH),
                                                                 ByteSpan(s_conn.sr, SCRATCH),
                                                                 Span<HeaderView>(hdrs, 16));
        if (!in.ok()) {
            ESP_LOGW(TAG, "inbound frame: %s", in.status.str());
            return false;
        }
        if (in.frame_status == InboundFrameStatus::Complete && !on_frame(in.frame)) {
            return false;
        }
        if (!s_connected) {
            return false;   /* a handler hit a send failure */
        }
    }
    return true;
}

/* ---- Task ---- */

static void drop_connection(const char *why)
{
    bool in_turn = s_turn.phase != P_IDLE;
    disconnect(why);
    s_auto_next_us = now_us() + s_auto_backoff_us;
    if (in_turn) {
        turn_fail("LOST CONNECTION TO MUSE");
    }
    muse_hatch_report(MUSE_HATCH_UNTESTED, "");
}

static void handle(const cmd_t &cmd)
{
    switch (cmd.type) {
    case CMD_CONNECT:
        if (s_turn.phase == P_IDLE) {
            disconnect("reconnect requested");
            ensure_connected();
        }
        break;
    case CMD_FORGET:
        if (s_turn.phase != P_IDLE) {
            turn_fail("SETTINGS CHANGED");
        }
        disconnect("settings changed");
        forget_vm();
        s_auto_next_us = 0;
        s_auto_backoff_us = AUTO_RETRY_MIN_US;
        break;
    case CMD_BEGIN:
        if (cmd.gen == s_gen.load()) {
            turn_begin(cmd.gen);
        }
        break;
    case CMD_END:
        if (cmd.gen == s_turn.gen && !s_turn.text && s_turn.phase == P_LISTEN) {
            s_turn.end_requested = true;
        }
        break;
    case CMD_CANCEL:
        if (cmd.gen == s_turn.gen && !s_turn.text && s_turn.phase != P_IDLE) {
            ESP_LOGI(TAG, "turn cancelled");
            turn_finish();
        }
        break;
    case CMD_TEXT:
        if (s_turn.phase != P_IDLE && !s_turn.text) {
            muse_hatch_console("error", "BUSY WITH A VOICE TURN", nullptr);
        } else {
            text_begin(cmd.text);
        }
        free(cmd.text);
        break;
    case CMD_TEXT_CANCEL:
        if (s_turn.text && s_turn.phase != P_IDLE) {
            turn_fail("CANCELLED");
        }
        break;
    case CMD_WAKE:   /* only ends hatch_task's resting wait */
        break;
    }
}

static void hatch_task(void *arg)
{
    (void)arg;
    for (;;) {
        cmd_t cmd;
        /* Resting, the socket's keepalive (PING_US) needs no quicker polls.
         * Resting unconnected, Wi-Fi may nap (muse_wifi_nap): wait for a
         * turn or for waking (CMD_WAKE) instead of watching it. */
        int wait_ms = !s_connected ? (s_resting ? -1 : 200) : s_turn.phase != P_IDLE ? 2 : s_resting ? 500 : 20;
        TickType_t wait = wait_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(wait_ms);
        if (xQueueReceive(s_cmds, &cmd, wait) == pdTRUE) {
            handle(cmd);
            while (xQueueReceive(s_cmds, &cmd, 0) == pdTRUE) {
                handle(cmd);
            }
        }
        if (!s_connected) {
            if (!muse_wifi_connected()) {
                s_auto_next_us = 0;
                s_auto_backoff_us = AUTO_RETRY_MIN_US;
            } else if (muse_hatch_configured() && now_us() >= s_auto_next_us) {
                if (ensure_connected()) {
                    s_auto_next_us = INT64_MAX;   /* until it drops */
                    s_auto_backoff_us = AUTO_RETRY_MIN_US;
                } else {
                    s_auto_next_us = now_us() + s_auto_backoff_us;
                    s_auto_backoff_us = s_auto_backoff_us * 2 < AUTO_RETRY_MAX_US ? s_auto_backoff_us * 2
                                                                                   : AUTO_RETRY_MAX_US;
                }
            }
            continue;
        }

        /* Resting, Wi-Fi may nap; don't wait for the server to go quiet. */
        if (s_resting && !muse_wifi_connected()) {
            drop_connection("Wi-Fi down");
            continue;
        }
        if (!poll_socket()) {
            drop_connection("receive failed");
            continue;
        }
        bool sent = true;
        if (s_turn.phase == P_LISTEN) {
            sent = VOICE_NOTE ? record_note() : pump_mic();
        }
        if (!sent) {
            drop_connection("send failed");
            continue;
        }
        if (s_turn.phase == P_WAIT_REPLY) {
            start_tts();
            decode();
        }
        if (!s_connected) {
            continue;
        }
        check_turn();

        int64_t t = now_us();
        if (t - s_conn.last_rx_us > DEAD_US) {
            drop_connection("server went quiet");
        } else if (s_turn.phase == P_IDLE && t - s_conn.last_use_us > IDLE_CLOSE_US) {
            disconnect("idle");
            muse_hatch_report(MUSE_HATCH_UNTESTED, "");
            s_auto_next_us = INT64_MAX;   /* the next turn connects */
        } else if (t - s_conn.last_ping_us > PING_US) {
            s_conn.last_ping_us = t;
            if (!ws_send_ping(s_conn.tls)) {
                drop_connection("ping failed");
            }
        }
    }
}

/* ---- Public API ---- */

static void post(cmd_type_t type, uint32_t gen)
{
    if (s_cmds) {
        cmd_t cmd{ type, gen, nullptr };
        xQueueSend(s_cmds, &cmd, pdMS_TO_TICKS(100));
    }
}

static void drain_out(void)
{
    static int16_t junk[256];
    while (xStreamBufferReceive(s_out, junk, sizeof(junk), 0)) {
    }
}

extern "C" void muse_hatch_start(void)
{
    if (s_cmds) {
        return;
    }
    cJSON_Hooks hooks = { json_alloc, heap_caps_free };
    cJSON_InitHooks(&hooks);
    /* Keep the command/event queues out of internal DRAM, which Wi-Fi, BLE and
     * mbedTLS can exhaust: when the allocation failed here hatch_task never
     * started and every turn was silently dropped. Fall back to internal RAM
     * on parts without usable PSRAM. */
    bool ok = true;
#define NEED(cond, what) do { if (!(cond)) { ESP_LOGE(TAG, "start failed: no room for %s", what); ok = false; } } while (0)
    s_cmds = xQueueCreateWithCaps(16, sizeof(cmd_t), MALLOC_CAP_SPIRAM);
    if (!s_cmds) {
        s_cmds = xQueueCreate(16, sizeof(cmd_t));
    }
    NEED(s_cmds, "the command queue");
    s_events = xQueueCreateWithCaps(16, sizeof(ev_t), MALLOC_CAP_SPIRAM);
    if (!s_events) {
        s_events = xQueueCreate(16, sizeof(ev_t));
    }
    NEED(s_events, "the event queue");
    s_in = xStreamBufferCreateWithCaps(IN_BYTES, 1, MALLOC_CAP_SPIRAM);
    NEED(s_in, "the audio input");
    s_out = xStreamBufferCreateWithCaps(OUT_BYTES, 1, MALLOC_CAP_SPIRAM);
    NEED(s_out, "the audio output");
    s_turn.chunk = static_cast<uint8_t *>(psram_alloc(DICT_CHUNK_BYTES + sizeof(MUSE_HATCH_NOTE_TAIL)));
    NEED(s_turn.chunk, "the reply buffer");
    s_turn.mp3 = static_cast<uint8_t *>(psram_alloc(TURN_AUDIO_BUF));
    NEED(s_turn.mp3, "the audio buffer");
    s_turn.note = VOICE_NOTE ? static_cast<uint8_t *>(psram_alloc(NOTE_PART_BYTES)) : nullptr;
    NEED(!VOICE_NOTE || s_turn.note, "the note buffer");
    s_turn.texts = static_cast<char *>(psram_alloc(MAX_MSGS * TEXT_MAX));   /* captions just stay untimed without it */
    s_pcm = static_cast<int16_t *>(psram_alloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t)));
    NEED(s_pcm, "the mp3 scratch");
    s_pcm16 = static_cast<int16_t *>(psram_alloc((MINIMP3_MAX_SAMPLES_PER_FRAME + 8) * sizeof(int16_t)));
    NEED(s_pcm16, "the pcm scratch");
    for (auto &s : s_streams) {
        s.line = static_cast<char *>(psram_alloc(NDJSON_LINE_MAX));
        s.cap = NDJSON_LINE_MAX;
    }
    s_turn.tts_msg = -1;
#undef NEED
    /* Half a session is worse than none: with a queue or a buffer missing, the
     * chat task would walk into a null pointer on its first turn. */
    if (!ok) {
        ESP_LOGE(TAG, "the chat session did not start");
        return;
    }
    /* Stack in PSRAM: TLS, Noise and the MP3 decoder (~16 KB of scratch) all run here. */
    if (xTaskCreatePinnedToCoreWithCaps(hatch_task, "muse_chat", 48 * 1024, nullptr, 5, nullptr, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "start failed: no room for the chat task");
    }
}

extern "C" void muse_hatch_chat_connect(void)
{
    post(CMD_CONNECT, 0);
}

extern "C" void muse_hatch_chat_forget(void)
{
    post(CMD_FORGET, 0);
}

extern "C" bool muse_hatch_ready(void)
{
    return s_cmds && muse_hatch_configured() && muse_wifi_connected();
}

extern "C" void muse_hatch_turn_begin(void)
{
    uint32_t gen = ++s_gen;
    xStreamBufferReset(s_in);
    drain_out();
    post(CMD_BEGIN, gen);
}

extern "C" void muse_hatch_turn_audio(const int16_t *pcm, size_t frames)
{
    size_t bytes = frames * sizeof(int16_t);
    if (xStreamBufferSend(s_in, pcm, bytes, 0) != bytes) {
        ESP_LOGW(TAG, "mic backlog full, dropped audio");
    }
}

/* Both ends move whole frames, so the buffer never splits one. */
extern "C" size_t muse_hatch_turn_audio_wait(const int16_t *pcm, size_t frames, int wait_ms)
{
    return xStreamBufferSend(s_in, pcm, frames * sizeof(int16_t), pdMS_TO_TICKS(wait_ms)) / sizeof(int16_t);
}

extern "C" void muse_hatch_turn_end(void)
{
    post(CMD_END, s_gen.load());
}

extern "C" void muse_hatch_turn_cancel(void)
{
    uint32_t gen = s_gen.load();
    ++s_gen;
    post(CMD_CANCEL, gen);
    drain_out();
}

extern "C" void muse_hatch_set_resting(bool resting)
{
    if (s_resting.exchange(resting) && !resting) {
        post(CMD_WAKE, 0);
    }
}

extern "C" void muse_hatch_text_turn(char *text)
{
    cmd_t cmd{ CMD_TEXT, 0, text };
    if (!s_cmds || !muse_hatch_configured()) {
        muse_hatch_console("error", "MUSE NOT SET UP", nullptr);
        free(text);
    } else if (xQueueSend(s_cmds, &cmd, pdMS_TO_TICKS(1000)) != pdTRUE) {
        muse_hatch_console("error", "BUSY", nullptr);
        free(text);
    }
}

extern "C" void muse_hatch_text_cancel(void)
{
    post(CMD_TEXT_CANCEL, 0);
}

extern "C" muse_hatch_ev_t muse_hatch_turn_event(char *text, size_t cap)
{
    ev_t ev;
    while (xQueueReceive(s_events, &ev, 0) == pdTRUE) {
        if (ev.gen == s_gen.load()) {
            strlcpy(text, ev.text, cap);
            return ev.type;
        }
    }
    return MUSE_HATCH_EV_NONE;
}

extern "C" bool muse_hatch_turn_caption(size_t played, char *out, size_t cap)
{
    /* The message being spoken: the last one whose speech has started. */
    const msg_t *m = nullptr;
    const char *text = nullptr;
    for (int i = 0; i < s_turn.nmsgs && s_turn.texts; i++) {
        const msg_t &c = s_turn.msgs[i];
        if (c.tts >= TTS_ACTIVE && c.len && c.pcm_start <= played) {
            m = &c;
            text = s_turn.texts + i * TEXT_MAX;
        }
    }
    if (!m) {
        /* Nothing said yet: the reply's opening page, to read while the speech is on its way. */
        for (int i = 0; i < s_turn.nmsgs && s_turn.texts; i++) {
            if (s_turn.msgs[i].len) {
                return muse_hatch_caption_at(s_turn.texts + i * TEXT_MAX, 0, out, cap);
            }
        }
        return false;
    }
    size_t len = strlen(text);
    uint32_t frames = m->pcm_frames ? m->pcm_frames : (uint32_t)(len * MIC_RATE / SPEECH_CHARS_PER_S);
    size_t at = frames ? (size_t)((uint64_t)(played - m->pcm_start) * len / frames) : 0;
    if (at >= len) {
        at = len ? len - 1 : 0;
    }

    return muse_hatch_caption_at(text, at, out, cap);
}

extern "C" size_t muse_hatch_turn_read(int16_t *pcm, size_t frames, int wait_ms)
{
    return xStreamBufferReceive(s_out, pcm, frames * sizeof(int16_t), pdMS_TO_TICKS(wait_ms)) / sizeof(int16_t);
}

/* Bench test: decodes the embedded test_reply.mp3 exactly as a reply is decoded. */
static size_t mp3_selftest(int16_t **pcm_out)
{
    extern const uint8_t mp3_start[] asm("_binary_test_reply_mp3_start");
    extern const uint8_t mp3_end[] asm("_binary_test_reply_mp3_end");
    size_t len = mp3_end - mp3_start;
    mp3dec_t *dec = (mp3dec_t *)psram_alloc(sizeof(mp3dec_t));
    int16_t *pcm = (int16_t *)psram_alloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
    size_t cap = MIC_RATE * 10;
    int16_t *out = (int16_t *)psram_alloc(cap * sizeof(int16_t));
    if (!dec || !pcm || !out) {
        free(dec);
        free(pcm);
        free(out);
        return 0;
    }
    mp3dec_init(dec);
    resampler_t rs;
    int rate = 0, frames = 0;
    size_t off = 0, n = 0;
    int64_t t0 = now_us();
    while (off < len) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(dec, mp3_start + off, len - off, pcm, &info);
        if (!info.frame_bytes) {
            break;
        }
        off += info.frame_bytes;
        if (!samples) {
            continue;
        }
        frames++;
        if (info.channels == 2) {
            for (int k = 0; k < samples; k++) {
                pcm[k] = (pcm[2 * k] + pcm[2 * k + 1]) / 2;
            }
        }
        if (rate != info.hz) {
            rate = info.hz;
            resampler_init(&rs, info.hz, MIC_RATE);
        }
        if (n + (size_t)samples * MIC_RATE / rate + 2 > cap) {
            break;
        }
        n += resample(&rs, pcm, samples, out + n);
    }
    ESP_LOGI(TAG, "mp3 selftest: %u bytes, %d frames at %d Hz -> %u samples (%.2f s) in %lld ms",
             (unsigned)len, frames, rate, (unsigned)n, n / (double)MIC_RATE, (now_us() - t0) / 1000);
    free(dec);
    free(pcm);
    *pcm_out = out;
    return n;
}

struct selftest_t {
    TaskHandle_t caller;
    int16_t *pcm;
    size_t n;
};

/* minimp3 wants ~16 KB of stack, more than the voice task has. */
extern "C" size_t muse_hatch_mp3_selftest(int16_t **pcm_out)
{
    selftest_t st = { xTaskGetCurrentTaskHandle(), nullptr, 0 };
    auto body = [](void *arg) {
        auto *st = (selftest_t *)arg;
        st->n = mp3_selftest(&st->pcm);
        xTaskNotifyGive(st->caller);
        vTaskSuspend(NULL);   /* the caller deletes it, which frees the PSRAM stack */
    };
    TaskHandle_t task;
    if (xTaskCreatePinnedToCoreWithCaps(body, "mp3_selftest", 32 * 1024, &st, 5, &task, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        return 0;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    vTaskDeleteWithCaps(task);
    *pcm_out = st.pcm;
    return st.n;
}

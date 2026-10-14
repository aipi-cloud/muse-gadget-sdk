// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/*
 * AIPI placeholder avatar (FIRMWARE_CONTRACT.md §5): an original geometric
 * pixel face - a rounded-square shell with a dark inner screen, block eyes
 * that blink, and a small mouth. It replaces Meta's Jollybot renderer, whose
 * Apache licence does not cover the character: nothing here derives from it.
 * Temporary art until AIPI's own character arrives; the renderer contract
 * (tools/muse/AVATAR_RECIPE.md, components/muse/muse_pixel.h) is unchanged:
 * a 64x64 indexed grid, blown up in nearest-neighbour blocks.
 */

#include "muse_pixel.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define W MUSE_PX_W
#define H MUSE_PX_H

/* ---------------------------------------------------------------------------
 * Palette: fixed ink + per-mode shell and accent.
 * ------------------------------------------------------------------------- */

enum {
    C_BG = 0,      /* backdrop */
    C_OUT,         /* shell outline */
    C_BODY,        /* shell, per-mode */
    C_BODY_D,      /* shell shade */
    C_SCREEN,      /* inner face screen */
    C_EYE,         /* eyes and mouth, per-mode accent */
    C_EYE_HI,      /* eye highlight */
    C_COUNT,
};

typedef struct {
    uint32_t shell;   /* 0xRRGGBB */
    uint32_t acc;     /* accent: eyes, mouth, and the surrounding UI */
} scheme_t;

static const scheme_t SCHEMES[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT]      = { 0xdfe6ff, 0xa9c0ff },
    [MUSE_MODE_IDLE]      = { 0xe9e4fb, 0xa77dff },
    [MUSE_MODE_LISTENING] = { 0xdff2ff, 0x5cb8ff },
    [MUSE_MODE_THINKING]  = { 0xf6e2ff, 0xe07bff },
    [MUSE_MODE_SPEAKING]  = { 0xddffef, 0x6ff0bf },
    [MUSE_MODE_ERROR]     = { 0xffe6e6, 0xff5c5c },
    [MUSE_MODE_OFF]       = { 0x8f8ab0, 0x7c72d0 },
};

static const uint32_t FIXED[C_COUNT] = {
    [C_BG] = 0x000000,
    [C_OUT] = 0x14141f,
    [C_BODY_D] = 0x000000,     /* derived from the shell in build_palette() */
    [C_SCREEN] = 0x0d1017,
    [C_EYE] = 0x000000,        /* accent, in build_palette() */
    [C_EYE_HI] = 0xffffff,
};

static uint16_t s_pal[C_COUNT];
static uint16_t s_pal_dim[C_COUNT];

static uint8_t s_fb[W * H];

uint32_t muse_pixel_accent(muse_mode_t mode)
{
    if ((unsigned)mode >= MUSE_MODE_COUNT) {
        mode = MUSE_MODE_IDLE;
    }
    return SCHEMES[mode].acc;
}

static uint16_t to565(uint32_t c)
{
    return (uint16_t)(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f));
}

/* Darkened copy for the last row/column of a grid cell (muse_pixel_scale). */
static uint16_t dim565(uint16_t c)
{
    uint32_t r = (uint32_t)(c >> 11 & 31) * 224 / 256;
    uint32_t g = (uint32_t)(c >> 5 & 63) * 224 / 256;
    uint32_t b = (uint32_t)(c & 31) * 224 / 256;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static uint32_t scale_hex(uint32_t c, int permille)
{
    uint32_t r = ((c >> 16) & 0xff) * permille / 1000;
    uint32_t g = ((c >> 8) & 0xff) * permille / 1000;
    uint32_t b = (c & 0xff) * permille / 1000;
    return (r << 16) | (g << 8) | b;
}

static void build_palette(muse_mode_t mode)
{
    const scheme_t *s = &SCHEMES[mode];
    uint32_t body[C_COUNT];
    memcpy(body, FIXED, sizeof(body));
    body[C_BODY] = s->shell;
    body[C_BODY_D] = scale_hex(s->shell, 780);
    body[C_EYE] = s->acc;
    for (int i = 0; i < C_COUNT; i++) {
        s_pal[i] = to565(body[i]);
        s_pal_dim[i] = dim565(s_pal[i]);
    }
}

/* ---------------------------------------------------------------------------
 * Drawing primitives (grid coordinates, clipped).
 * ------------------------------------------------------------------------- */

static void px(int x, int y, uint8_t c)
{
    if (x >= 0 && x < W && y >= 0 && y < H) {
        s_fb[y * W + x] = c;
    }
}

static void rect(int x0, int y0, int x1, int y1, uint8_t c)
{
    if (y0 < 0) y0 = 0;
    if (y1 >= H) y1 = H - 1;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            px(x, y, c);
        }
    }
}

/* Filled rounded rectangle; rows of a corner follow a quarter circle. */
static void rrect(int x0, int y0, int x1, int y1, int r, uint8_t c)
{
    if (r <= 0) {
        rect(x0, y0, x1, y1, c);
        return;
    }
    for (int y = y0; y <= y1; y++) {
        int dy = y < y0 + r ? y0 + r - y : y > y1 - r ? y - (y1 - r) : 0;
        int dx = 0;
        if (dy > 0 && dy < r) {
            dx = r - (int)sqrtf((float)(r * r - dy * dy));
        }
        rect(x0 + dx, y, x1 - dx, y, c);
    }
}

/* Integer line (the X eyes). */
static void line(int x0, int y0, int x1, int y1, uint8_t c)
{
    int dx = abs(x1 - x0), dy = abs(y1 - y0);
    int n = dx > dy ? dx : dy;
    if (n == 0) {
        px(x0, y0, c);
        return;
    }
    for (int i = 0; i <= n; i++) {
        px(x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, c);
    }
}

/* ---------------------------------------------------------------------------
 * The face
 * ------------------------------------------------------------------------- */

/* Eye boxes: 7 wide, 9 tall, centres at x 24 and 40, y 27 (before dy). */
#define EYE_L_X 21
#define EYE_R_X 37
#define EYE_Y  23

static void draw_eye_block(int x, int dy, int h)
{
    rect(x, EYE_Y + dy, x + 6, EYE_Y + dy + h - 1, C_EYE);
    px(x + 1, EYE_Y + dy + 1, C_EYE_HI);
}

static void draw_eye_happy(int x, int dy)
{
    /* ^ : two strokes meeting above centre, 2 px thick. */
    for (int i = 0; i < 4; i++) {
        px(x + i, EYE_Y + dy + 6 - i, C_EYE);
        px(x + i, EYE_Y + dy + 7 - i, C_EYE);
        px(x + 6 - i, EYE_Y + dy + 6 - i, C_EYE);
        px(x + 6 - i, EYE_Y + dy + 7 - i, C_EYE);
    }
}

static void draw_eye_closed(int x, int dy)
{
    rect(x, EYE_Y + dy + 4, x + 6, EYE_Y + dy + 5, C_EYE);
}

static void draw_eye_x(int x, int dy)
{
    line(x, EYE_Y + dy, x + 6, EYE_Y + dy + 8, C_EYE);
    line(x + 6, EYE_Y + dy, x, EYE_Y + dy + 8, C_EYE);
}

static void draw_mouth(int dy, muse_mode_t mode, float level, bool happy, bool talking)
{
    int cy = 38 + dy;
    if (happy > 0.2f && mode != MUSE_MODE_ERROR) {
        rrect(25, cy - 3, 38, cy + 2, 2, C_EYE);   /* wide open grin */
        rect(27, cy - 3, 36, cy - 2, C_EYE_HI);    /* teeth */
        return;
    }
    switch (mode) {
    case MUSE_MODE_LISTENING:
        rect(29, cy - 4, 34, cy + 2, C_EYE);       /* O */
        rect(31, cy - 2, 32, cy, C_SCREEN);
        break;
    case MUSE_MODE_SPEAKING: {
        int open = 1 + (int)(talking ? level * 4.0f + 0.9f : 1.0f);
        rect(27, cy - 1, 36, cy - 1 + open, C_EYE);
        break;
    }
    case MUSE_MODE_ERROR:
    case MUSE_MODE_OFF:
    case MUSE_MODE_THINKING:
        rect(27, cy, 36, cy, C_EYE);               /* flat */
        break;
    default:
        rect(26, cy, 37, cy, C_EYE);               /* smile */
        px(25, cy - 1, C_EYE);
        px(38, cy - 1, C_EYE);
        break;
    }
}

static void draw_face(int dy, muse_mode_t mode, float t, float level, float happy,
                      bool eyes_closed, bool eyes_happy, bool eyes_x)
{
    /* Shell: outline pass, then body, then the inner screen. */
    rrect(9, 11 + dy, 54, 52 + dy, 9, C_OUT);
    rrect(11, 13 + dy, 52, 50 + dy, 7, C_BODY);
    rrect(13, 15 + dy, 50, 48 + dy, 5, C_BODY_D);   /* bezel */
    rrect(15, 17 + dy, 48, 46 + dy, 4, C_SCREEN);

    /* Stand: a geometric foot under the shell. */
    rect(25, 53 + dy, 38, 56 + dy, C_BODY_D);
    rect(21, 57 + dy, 42, 59 + dy, C_OUT);
    rect(23, 57 + dy, 40, 58 + dy, C_BODY_D);

    /* Eyes. */
    if (eyes_x) {
        draw_eye_x(EYE_L_X, dy);
        draw_eye_x(EYE_R_X, dy);
    } else if (eyes_happy) {
        draw_eye_happy(EYE_L_X, dy);
        draw_eye_happy(EYE_R_X, dy);
    } else if (eyes_closed) {
        draw_eye_closed(EYE_L_X, dy);
        draw_eye_closed(EYE_R_X, dy);
    } else if (mode == MUSE_MODE_THINKING) {
        rect(EYE_L_X, EYE_Y + dy + 3, EYE_L_X + 6, EYE_Y + dy + 7, C_EYE);
        rect(EYE_R_X, EYE_Y + dy + 3, EYE_R_X + 6, EYE_Y + dy + 7, C_EYE);
        px(EYE_L_X + 1, EYE_Y + dy + 4, C_EYE_HI);
        px(EYE_R_X + 1, EYE_Y + dy + 4, C_EYE_HI);
    } else {
        int h = mode == MUSE_MODE_LISTENING ? 9 : 8;
        draw_eye_block(EYE_L_X, dy, h);
        draw_eye_block(EYE_R_X, dy, h);
    }

    draw_mouth(dy, mode, level, happy, true);

    /* Thinking: three squares lifting away to the upper right. */
    if (mode == MUSE_MODE_THINKING) {
        int phase = (int)(t * 3.0f) % 3;
        for (int i = 0; i < 3; i++) {
            if ((i + phase) % 3 == 2) {
                continue;   /* blink one out */
            }
            int x = 52 + i * 3, y = 12 + dy - i * 5;
            rect(x, y, x + 1 + i / 2, y + 1 + i / 2, C_EYE);
        }
    }
    /* Listening/speaking: two short sound ticks beside the shell. */
    if ((mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_SPEAKING) &&
        (int)(t * 4.0f) % 2 == 0) {
        rect(4, 26 + dy, 6, 32 + dy, C_EYE);
        rect(57, 26 + dy, 59, 32 + dy, C_EYE);
    }
}

/* ---------------------------------------------------------------------------
 * Frame: screen pixel -> grid cell, and the blown-up draw.
 * ------------------------------------------------------------------------- */

/*
 * Screen pixel -> grid cell, with 0x80 set on a cell's last screen pixel. When
 * cells are 3+ pixels, that edge is drawn dimmer so the pixel grid shows.
 */
#define MAP_MAX 512
static uint8_t s_map[MAP_MAX];
static int s_size;

void muse_pixel_set_size(int px)
{
    s_size = px < MAP_MAX ? px : MAP_MAX;
    bool grid = s_size >= 3 * W;
    for (int i = 0; i < s_size; i++) {
        int cell = i * W / s_size;
        bool edge = grid && (i + 1) * W / s_size != cell;
        s_map[i] = (uint8_t)(cell | (edge ? 0x80 : 0));
    }
}

void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    int n = x1 - x0 + 1;
    const uint8_t *xmap = &s_map[x0];
    const uint16_t *prev = NULL;
    uint8_t prev_m = 0;
    for (int y = y0; y <= y1; y++, dst += stride_px) {
        uint8_t m = s_map[y];
        if (prev && m == prev_m) {
            memcpy(dst, prev, n * sizeof(uint16_t));
            continue;
        }
        const uint8_t *row = &s_fb[(m & 0x7f) * W];
        if (m & 0x80) {
            for (int i = 0; i < n; i++) {
                dst[i] = s_pal_dim[row[xmap[i] & 0x7f]];
            }
        } else {
            for (int i = 0; i < n; i++) {
                uint8_t xm = xmap[i];
                uint8_t c = row[xm & 0x7f];
                dst[i] = xm & 0x80 ? s_pal_dim[c] : s_pal[c];
            }
        }
        prev = dst;
        prev_m = m;
    }
}

/* ---------------------------------------------------------------------------
 * Render
 * ------------------------------------------------------------------------- */

static uint32_t s_rng = 0x9e3779b9u;

static float frand(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (float)(s_rng & 0xffff) / 65535.0f;
}

void muse_pixel_render(const muse_pose_t *p)
{
    muse_mode_t mode = p->mode;
    if ((unsigned)mode >= MUSE_MODE_COUNT) {
        mode = MUSE_MODE_IDLE;
    }
    float t = p->t;
    float level = p->level;
    float happy = p->happy < 0.0f ? 0.0f : p->happy > 1.0f ? 1.0f : p->happy;

    /* Blink: a 120 ms close every 2.5-5.5 s. */
    static float next_blink = 2.0f, blink_end;
    if (t >= next_blink) {
        blink_end = t + 0.12f;
        next_blink = t + 2.5f + frand() * 3.0f;
    }

    build_palette(mode);
    memset(s_fb, C_BG, sizeof(s_fb));

    /* Body motion: a small integer bob, per mode. */
    int dy = 0;
    switch (mode) {
    case MUSE_MODE_LISTENING: dy = (int)lroundf(sinf(t * 3.0f) * 0.7f); break;
    case MUSE_MODE_THINKING:  dy = (int)lroundf(sinf(t * 2.4f) * 0.8f); break;
    case MUSE_MODE_SPEAKING:  dy = -(int)lroundf(level * 1.5f + sinf(t * 5.0f) * 0.4f); break;
    case MUSE_MODE_ERROR:     dy = p->mode_t < 0.6f ? (int)(sinf(t * 30.0f) * 1.4f) : 0; break;
    case MUSE_MODE_OFF:       dy = 2; break;
    default:                  dy = (int)lroundf(sinf(t * 1.8f)); break;
    }
    if (happy > 0.1f && mode != MUSE_MODE_ERROR) {
        dy -= (int)lroundf(fabsf(sinf(t * 9.0f)) * 3.0f * happy);
    }
    if (mode == MUSE_MODE_BOOT) {
        float rise = p->mode_t < 0.6f ? 1.0f - p->mode_t / 0.6f : 0.0f;
        dy += (int)(rise * 7.0f);
    }

    bool eyes_closed = false, eyes_happy = false, eyes_x = false;
    if (mode == MUSE_MODE_BOOT) {
        eyes_closed = p->mode_t < 0.9f;
    } else if (mode == MUSE_MODE_OFF) {
        eyes_closed = p->mode_t > 0.5f;
    } else if (t < blink_end) {
        eyes_closed = true;
    }
    if (happy > 0.2f && mode != MUSE_MODE_ERROR) {
        eyes_happy = true;
    }
    if (mode == MUSE_MODE_ERROR) {
        eyes_x = true;
    }

    draw_face(dy, mode, t, level, happy, eyes_closed, eyes_happy, eyes_x);
}

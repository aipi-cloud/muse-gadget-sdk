/*
 * Momo - the AIPI mascot.
 *
 * AIPI original character, not derived from Meta's Jollybot.
 *
 * Shape: a soft rice-cake / cloud blob, 35 px wide and 43 px tall on the 64x64
 * grid, centred at x = 32 with its hem at y ~ 56.5. The bottom edge closes in
 * three gentle scallops; there are no ears, horns, antennae or tail. Two round
 * nub paws sit at its sides, and a single teal cowlick (the only brand colour)
 * curls up from the crown and sways with the body.
 *
 * Colours: 1 px warm-brown ink outline #4A3428, cream body in three tones -
 * #D9C4A3 shade, #F2E3C9 mid, #FFF8EC light - deep-brown bean eyes #3A2418
 * with a white sparkle, coral blush #FF9E8A, a small dark mouth, and the teal
 * cowlick #2FB8A6.
 *
 * Personality: a calm, slightly clumsy little mochi that likes being petted.
 * It bobs when it breathes, leans in when it listens, taps its chin when it
 * thinks, bounces when it talks and squints with joy when it is petted.
 *
 * Rendering is the AIPI contract (components/muse/muse_pixel.h): a 64x64
 * indexed grid, integer/Q12 shading with a 4x4 Bayer dither, a part mask for
 * the 1 px silhouette, and a host-side nearest-neighbour blow-up. No dynamic
 * memory; every table is static and every per-pixel loop is pure integer.
 */

#include "muse_pixel.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define W MUSE_PX_W
#define H MUSE_PX_H

#define ONE 4096 /* Q12 unity */

/* ---------------------------------------------------------------------------
 * Palette: 23 roles, index 0 is the black backdrop.
 * ------------------------------------------------------------------------- */

enum {
    C_BG = 0,
    C_OUT,      /* warm brown ink */
    C_SHADOW,   /* floor contact shadow */
    C_BODY_D,   /* cream, shade */
    C_BODY_M,   /* cream, mid */
    C_BODY_L,   /* cream, light */
    C_EYE,      /* deep brown */
    C_EYE_HI,   /* white sparkle */
    C_BLUSH,
    C_BLUSH_D,
    C_MOUTH,
    C_TONGUE,
    C_TEAL,     /* cowlick */
    C_TEAL_D,
    C_GLOW0,    /* per-mode glow ramp (scheme) */
    C_GLOW1,
    C_GLOW2,
    C_GLOW3,
    C_ACCENT,   /* per-mode accent (scheme) */
    C_SPARK,
    C_HEART,
    C_WHITE,
    C_COUNT
};

typedef struct {
    uint32_t glow[4]; /* bright to deep, used for the aura and rings */
    uint32_t acc;     /* UI accent around the avatar */
} scheme_t;

static const scheme_t SCHEMES[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT]      = { { 0xffffff, 0xcfe0ff, 0x8fa8ff, 0x5a5fe0 }, 0xa9c0ff },
    [MUSE_MODE_IDLE]      = { { 0xf4e8ff, 0xc7a4ff, 0x9a6bff, 0x5b3fd9 }, 0xa77dff },
    [MUSE_MODE_LISTENING] = { { 0xe8faff, 0x8fdcff, 0x3fa2ff, 0x2a5bd7 }, 0x5cb8ff },
    [MUSE_MODE_THINKING]  = { { 0xffe6ff, 0xff9cf0, 0xd35bff, 0x7a2bd9 }, 0xe07bff },
    [MUSE_MODE_SPEAKING]  = { { 0xeafff4, 0x9ff5cf, 0x3fd9a0, 0x1f9a7a }, 0x6ff0bf },
    [MUSE_MODE_ERROR]     = { { 0xffd6d6, 0xff6b6b, 0xc7304a, 0x6b1a3a }, 0xff5c5c },
    [MUSE_MODE_OFF]       = { { 0xd8d4ff, 0x8f86d9, 0x5a4fb0, 0x2e2870 }, 0x7c72d0 },
};

static const uint32_t FIXED_HEX[C_COUNT] = {
    [C_BG] = 0x000000,
    [C_OUT] = 0x4a3428,
    [C_SHADOW] = 0x1c1310,
    [C_BODY_D] = 0xd9c4a3,
    [C_BODY_M] = 0xf2e3c9,
    [C_BODY_L] = 0xfff8ec,
    [C_EYE] = 0x3a2418,
    [C_EYE_HI] = 0xffffff,
    [C_BLUSH] = 0xff9e8a,
    [C_BLUSH_D] = 0xe07a70,
    [C_MOUTH] = 0x4a2a20,
    [C_TONGUE] = 0xe2696b,
    [C_TEAL] = 0x2fb8a6,
    [C_TEAL_D] = 0x1d8a7b,
    [C_SPARK] = 0xfff6e2,
    [C_HEART] = 0xff7e8c,
    [C_WHITE] = 0xffffff,
};

static uint8_t s_rgb[C_COUNT][3];
static uint16_t s_pal[C_COUNT];
static uint16_t s_pal_dim[C_COUNT];
static bool s_pal_init;

static const uint8_t BAYER[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

static uint8_t s_fb[W * H];
static uint8_t s_mask[W * H]; /* 1 body, 2 paw, 3 cowlick */

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

/*
 * Per-mode palette. The character's own colours never change; the scheme
 * colours (rim, glow ramp, accent) ease toward the new mode by 1-e^-7dt.
 */
static void palette_frame(muse_mode_t mode, float dt, int bright)
{
    uint32_t tgt[C_COUNT];
    memcpy(tgt, FIXED_HEX, sizeof(tgt));

    const scheme_t *s = &SCHEMES[mode];
    for (int i = 0; i < 4; i++) {
        tgt[C_GLOW0 + i] = s->glow[i];
    }
    tgt[C_ACCENT] = s->acc;

    int k = (int)(dt * 7.0f * 256.0f);
    if (k > 256) k = 256;
    if (k < 0) k = 0;

    for (int i = 0; i < C_COUNT; i++) {
        uint32_t a = tgt[i];
        int r = (int)((a >> 16) & 0xff);
        int g = (int)((a >> 8) & 0xff);
        int b = (int)(a & 0xff);
        if (!s_pal_init) {
            s_rgb[i][0] = (uint8_t)r;
            s_rgb[i][1] = (uint8_t)g;
            s_rgb[i][2] = (uint8_t)b;
        } else if (i >= C_GLOW0) {
            s_rgb[i][0] = (uint8_t)(s_rgb[i][0] + (((r - s_rgb[i][0]) * k) >> 8));
            s_rgb[i][1] = (uint8_t)(s_rgb[i][1] + (((g - s_rgb[i][1]) * k) >> 8));
            s_rgb[i][2] = (uint8_t)(s_rgb[i][2] + (((b - s_rgb[i][2]) * k) >> 8));
        }
        int rr = s_rgb[i][0] * bright >> 8;
        int gg = s_rgb[i][1] * bright >> 8;
        int bb = s_rgb[i][2] * bright >> 8;
        s_pal[i] = to565(((uint32_t)rr << 16) | ((uint32_t)gg << 8) | (uint32_t)bb);
        s_pal_dim[i] = dim565(s_pal[i]);
    }
    s_pal_init = true;
}

uint32_t muse_pixel_accent(muse_mode_t mode)
{
    if ((unsigned)mode >= MUSE_MODE_COUNT) {
        mode = MUSE_MODE_IDLE;
    }
    return SCHEMES[mode].acc;
}

/* ---------------------------------------------------------------------------
 * Small integer tables and primitives.
 * ------------------------------------------------------------------------- */

/* sin(2*pi*i/64) in Q15, with the wrap sample for linear interpolation. */
static const int16_t SIN_Q15[65] = {
         0,   3212,   6393,   9512,  12539,  15446,  18204,  20787,
     23170,  25329,  27245,  28898,  30273,  31356,  32137,  32609,
     32767,  32609,  32137,  31356,  30273,  28898,  27245,  25329,
     23170,  20787,  18204,  15446,  12539,   9512,   6393,   3212,
         0,  -3212,  -6393,  -9512, -12539, -15446, -18204, -20787,
    -23170, -25329, -27245, -28898, -30273, -31356, -32137, -32609,
    -32767, -32609, -32137, -31356, -30273, -28898, -27245, -25329,
    -23170, -20787, -18204, -15446, -12539,  -9512,  -6393,  -3212,
         0,
};

/* Half width of a filled disc of radius r at each row dy. */
static const int8_t DISC_HALF[6][7] = {
    { 0, 0, 0, 0, 0, 0, 0 },
    { 1, 0, 0, 0, 0, 0, 0 },
    { 2, 1, 0, 0, 0, 0, 0 },
    { 3, 2, 2, 0, 0, 0, 0 },
    { 4, 3, 3, 2, 0, 0, 0 },
    { 5, 4, 4, 4, 3, 0, 0 },
};

/* One turn of a sine at Q16 phase, result -32767..32767. */
static int qsin16(int ph)
{
    ph &= 0xffff;
    int i = ph >> 10;
    int f = ph & 1023;
    int a = SIN_Q15[i];
    int b = SIN_Q15[i + 1];
    return a + (((b - a) * f) >> 10);
}

/* Sine of `cycles` turns, scaled to `amp` pixels, rounded. */
static int osc_px(float cycles, int amp)
{
    float whole = (float)(int)cycles;
    int ph = (int)((cycles - whole) * 65536.0f) & 0xffff;
    return (qsin16(ph) * amp) >> 15;
}

static void px(int x, int y, uint8_t c)
{
    if (x >= 0 && x < W && y >= 0 && y < H) {
        s_fb[y * W + x] = c;
    }
}

static void hline(int x0, int x1, int y, uint8_t c)
{
    if (y < 0 || y >= H) {
        return;
    }
    if (x0 < 0) x0 = 0;
    if (x1 >= W) x1 = W - 1;
    for (int x = x0; x <= x1; x++) {
        s_fb[y * W + x] = c;
    }
}

static void mask_disc(int cx, int cy, int r, uint8_t id)
{
    if (r < 0 || r > 5) {
        return;
    }
    for (int j = -r; j <= r; j++) {
        int dy = j < 0 ? -j : j;
        int half = DISC_HALF[r][dy];
        int y = cy + j;
        if (y < 0 || y >= H) {
            continue;
        }
        for (int i = -half; i <= half; i++) {
            int x = cx + i;
            if (x >= 0 && x < W) {
                s_mask[y * W + x] = id;
            }
        }
    }
}

static void stamp(int x, int y, const uint8_t *rows, int n, uint8_t c)
{
    for (int j = 0; j < n; j++) {
        uint8_t m = rows[j];
        for (int i = 0; m; i++, m = (uint8_t)(m >> 1)) {
            if (m & 1) {
                px(x + i, y + j, c);
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * The body: one superellipse-ish blob with a scalloped hem, shaded from a
 * fake surface normal with a Bayer dither, plus the mode's dithered aura.
 * ------------------------------------------------------------------------- */

#define BODY_CX 32
#define BODY_CY 36
#define BODY_RX 16
#define BODY_RY 18
#define BODY_FLOOR 54 /* ellipse bottom; the scallops hang 2 px below */

static int s_hem[W]; /* extra pixels added under the ellipse, per column */
static int s_lx[W];
static int s_ly[H];
static int s_u2[W]; /* (dx/rx)^2 in Q12, per column */

static void build_hem(int cx)
{
    for (int x = 0; x < W; x++) {
        int ax = x - cx;
        if (ax < 0) ax = -ax;
        if (ax >= 14) {
            s_hem[x] = 0;
            continue;
        }
        int ph = ((int)((x - cx) * (65536.0f / 10.0f)) + 16384) & 0xffff;
        int w = 256 + ((qsin16(ph) * 460) >> 15); /* Q8 px, 0..2 */
        if (w < 0) w = 0;
        int taper = 14 - ax;
        if (taper > 5) taper = 5;
        s_hem[x] = (w * taper / 5) >> 8;
    }
}

static void draw_body(int cx, int cy, int rx, int ry, int aura)
{
    build_hem(cx);

    int kx = (ONE * ONE) / (rx * rx);
    int ky = (ONE * ONE) / (ry * ry);

    for (int x = 0; x < W; x++) {
        int dx = x - cx;
        s_lx[x] = (dx * ONE) / rx;
        s_u2[x] = (dx * dx * kx) >> 12;
    }
    for (int y = 0; y < H; y++) {
        s_ly[y] = ((y - cy) * ONE) / ry;
    }

    for (int y = 0; y < H; y++) {
        int dy = y - cy;
        int ly = s_ly[y];
        uint8_t *row = &s_fb[y * W];
        uint8_t *mrow = &s_mask[y * W];
        int ymax = ry + 3;
        if (dy < -ymax || dy > ymax + 2) {
            continue;
        }
        for (int x = 0; x < W; x++) {
            int dx = x - cx;
            int adx = dx < 0 ? -dx : dx;
            if (adx > rx + 8) {
                continue;
            }
            int dye = dy > 0 ? dy - s_hem[x] : dy;
            int u2 = s_u2[x];
            int v2 = (dye * dye * ky) >> 12;
            int f;
            if (dy > 0) {
                /* Flat, rice-cake underside (superellipse n=4) so the wavy
                   hem runs along the floor instead of round a dome. */
                f = ((u2 * u2) >> 12) + ((v2 * v2) >> 12);
            } else {
                /* Soft dome on top (n slightly above 2). */
                f = u2 + v2 - ((u2 * v2 >> 12) * 3 >> 3);
            }

            if (f < ONE) {
                /* Cream, lit from the top left, dithered between tones. */
                int lx = s_lx[x];
                int lit = -((lx * 2 + ly * 5) >> 2);
                lit += (((x * 37 + y * 17) & 31) - 16) * 3;
                int d = (BAYER[y & 3][x & 3] - 8) * 58;
                int l = lit + d;

                uint8_t c;
                if (l > 520) {
                    c = C_BODY_L;
                } else if (l > -3900) {
                    c = C_BODY_M;
                } else {
                    c = C_BODY_D;
                }
                row[x] = c;
                mrow[x] = 1;
            } else if (f < ONE + 1400 && aura > 0) {
                int t = f - ONE;
                uint8_t c;
                int cov;
                if (t < 260) {
                    c = C_GLOW1;
                    cov = 14;
                } else if (t < 560) {
                    c = C_GLOW2;
                    cov = 9;
                } else if (t < 900) {
                    c = C_GLOW2;
                    cov = 4;
                } else {
                    c = C_GLOW3;
                    cov = 2;
                }
                cov = cov * aura >> 8;
                if (BAYER[y & 3][x & 3] < cov) {
                    row[x] = c;
                }
            }
        }
    }
}

/* 1 px ink around every part, drawn on background pixels only. */
static void outline_pass(void)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (s_mask[y * W + x]) {
                continue;
            }
            bool edge = false;
            if (x > 0 && s_mask[y * W + x - 1]) edge = true;
            if (x < W - 1 && s_mask[y * W + x + 1]) edge = true;
            if (y > 0 && s_mask[(y - 1) * W + x]) edge = true;
            if (y < H - 1 && s_mask[(y + 1) * W + x]) edge = true;
            if (edge) {
                s_fb[y * W + x] = C_OUT;
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Paws and cowlick.
 * ------------------------------------------------------------------------- */

static void draw_paw(int cx, int cy, int r)
{
    /*
     * A seam ring: ink where the paw breaks the silhouette, a soft crease
     * where it rests against the body, so it reads as attached.
     */
    int rr = r + 1;
    /* Fully inside the body, a paw reads better as a soft crease than as a
       stray ink ring (a monocle on the cheek, say). */
    bool inside = true;
    const int ox[4] = { 0, 0, -rr, rr };
    const int oy[4] = { -rr, rr, 0, 0 };
    for (int k = 0; k < 4; k++) {
        int x = cx + ox[k];
        int y = cy + oy[k];
        if (x < 0 || x >= W || y < 0 || y >= H || s_mask[y * W + x] != 1) {
            inside = false;
        }
    }
    for (int j = -rr; j <= rr; j++) {
        int dy = j < 0 ? -j : j;
        int h_out = DISC_HALF[rr][dy];
        int h_in = (dy <= r) ? DISC_HALF[r][dy] : -1;
        for (int i = -h_out; i <= h_out; i++) {
            if (h_in >= 0 && i >= -h_in && i <= h_in) {
                continue;
            }
            int x = cx + i;
            int y = cy + j;
            if (x < 0 || x >= W || y < 0 || y >= H) {
                continue;
            }
            px(x, y, inside ? C_BODY_D : C_OUT);
        }
    }
    for (int j = -r; j <= r; j++) {
        int dy = j < 0 ? -j : j;
        int half = DISC_HALF[r][dy];
        for (int i = -half; i <= half; i++) {
            uint8_t c = C_BODY_M;
            if (i + j < -1) {
                c = C_BODY_L;
            } else if (j == r || i + j > 2) {
                c = C_BODY_D;
            }
            px(cx + i, cy + j, c);
        }
    }
    mask_disc(cx, cy, r, 2);
}

/* A paw held in front of the face: drawn after the outline pass so the ink
   ring survives on top of the body and reads as a separate limb. */
static void draw_paw_front(int cx, int cy, int r)
{
    int rr = r + 1;
    for (int j = -rr; j <= rr; j++) {
        int dy = j < 0 ? -j : j;
        int h_out = DISC_HALF[rr][dy];
        int h_in = (dy <= r) ? DISC_HALF[r][dy] : -1;
        for (int i = -h_out; i <= h_out; i++) {
            if (h_in >= 0 && i >= -h_in && i <= h_in) {
                continue;
            }
            px(cx + i, cy + j, C_OUT);
        }
    }
    for (int j = -r; j <= r; j++) {
        int dy = j < 0 ? -j : j;
        int half = DISC_HALF[r][dy];
        for (int i = -half; i <= half; i++) {
            uint8_t c = C_BODY_M;
            if (i + j < -1) {
                c = C_BODY_L;
            } else if (j == r || i + j > 2) {
                c = C_BODY_D;
            }
            px(cx + i, cy + j, c);
        }
    }
    mask_disc(cx, cy, r, 2);
}

/* A four-pixel teal cowlick that curls to the right; `sway` is in pixels. */
static void draw_cowlick(int bx, int top, int sway)
{
    static const uint8_t L[8] = { 4, 3, 2, 2, 1, 1, 1, 1 };
    static const uint8_t R[8] = { 5, 5, 4, 3, 2, 2, 2, 2 };

    for (int j = 0; j < 8; j++) {
        int sh = (sway * (7 - j)) / 7;
        int y = top + j;
        for (int i = L[j]; i <= R[j]; i++) {
            int x = bx + i + sh;
            if (x < 0 || x >= W || y < 0 || y >= H) {
                continue;
            }
            uint8_t c = (i == R[j] && j < 5) ? C_TEAL_D : C_TEAL;
            px(x, y, c);
            s_mask[y * W + x] = 3;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Face: stamped eyes and mouths, 4-5 px wide, exaggerated for 64x64.
 * ------------------------------------------------------------------------- */

enum { EYE_BEAN, EYE_WIDE, EYE_HAPPY, EYE_CLOSED, EYE_X };
enum { M_SMILE, M_GRIN, M_O, M_FLAT, M_WAVE, M_TALK };

static void eye_bean(int x, int y, bool big, int gx, int gy)
{
    static const uint8_t SMALL[5] = { 6, 15, 15, 15, 6 };
    static const uint8_t BIG[6] = { 14, 31, 31, 31, 31, 14 };
    x += gx;
    y += gy;
    if (big) {
        stamp(x, y, BIG, 6, C_EYE);
        px(x + 1, y + 1, C_EYE_HI);
        px(x + 2, y + 1, C_EYE_HI);
    } else {
        stamp(x, y, SMALL, 5, C_EYE);
        px(x + 1, y + 1, C_EYE_HI);
    }
}

static void eye_happy(int x, int y)
{
    hline(x + 1, x + 3, y, C_EYE);
    px(x, y + 1, C_EYE);
    px(x + 4, y + 1, C_EYE);
    px(x, y + 2, C_EYE);
    px(x + 4, y + 2, C_EYE);
}

static void eye_closed(int x, int y)
{
    px(x, y, C_EYE);
    px(x + 4, y, C_EYE);
    hline(x + 1, x + 3, y + 1, C_EYE);
}

static void eye_x(int x, int y)
{
    for (int i = 0; i < 5; i++) {
        px(x + i, y + i, C_EYE);
        px(x + 4 - i, y + i, C_EYE);
    }
    px(x + 2, y + 2, C_EYE);
}

static void draw_eye(int x, int y, int style, bool big, int gx, int gy)
{
    switch (style) {
    case EYE_HAPPY:
        eye_happy(x, y);
        break;
    case EYE_CLOSED:
        eye_closed(x, y);
        break;
    case EYE_X:
        eye_x(x, y);
        break;
    default:
        eye_bean(x, y, big || style == EYE_WIDE, gx, gy);
        break;
    }
}

static void mouth_smile(int cx, int y)
{
    px(cx - 3, y, C_MOUTH);
    px(cx + 3, y, C_MOUTH);
    hline(cx - 2, cx + 2, y + 1, C_MOUTH);
}

static void mouth_grin(int cx, int y)
{
    hline(cx - 2, cx + 2, y, C_MOUTH);
    hline(cx - 3, cx + 3, y + 1, C_MOUTH);
    hline(cx - 3, cx + 3, y + 2, C_MOUTH);
    hline(cx - 2, cx + 2, y + 3, C_MOUTH);
    hline(cx - 1, cx + 1, y + 2, C_TONGUE);
    px(cx - 3, y, C_MOUTH);
    px(cx + 3, y, C_MOUTH);
}

static void mouth_o(int cx, int y)
{
    hline(cx - 1, cx + 1, y, C_MOUTH);
    px(cx - 2, y + 1, C_MOUTH);
    px(cx + 2, y + 1, C_MOUTH);
    px(cx - 2, y + 2, C_MOUTH);
    px(cx + 2, y + 2, C_MOUTH);
    hline(cx - 1, cx + 1, y + 3, C_MOUTH);
}

static void mouth_wave(int cx, int y)
{
    px(cx - 2, y, C_MOUTH);
    px(cx - 1, y - 1, C_MOUTH);
    px(cx, y, C_MOUTH);
    px(cx + 1, y - 1, C_MOUTH);
    px(cx + 2, y, C_MOUTH);
}

static void mouth_talk(int cx, int ytop, int h)
{
    if (h < 1) h = 1;
    if (h > 6) h = 6;
    for (int j = 0; j < h; j++) {
        int w = 2;
        if ((j == 0 || j == h - 1) && h > 3) w = 1;
        hline(cx - w, cx + w, ytop + j, C_MOUTH);
    }
    if (h >= 4) {
        hline(cx - 1, cx + 1, ytop + h - 1, C_TONGUE);
    }
}

static void blush_blob(int x, int y, int wide, uint8_t c)
{
    hline(x + 1, x + wide - 2, y, c);
    hline(x, x + wide - 1, y + 1, c);
    hline(x + 1, x + wide - 2, y + 2, C_BLUSH_D);
}

/* ---------------------------------------------------------------------------
 * Effects: sparkles, rings, hearts.
 * ------------------------------------------------------------------------- */

static void sparkle(int x, int y, int size, uint8_t c, uint8_t core)
{
    px(x, y, core);
    if (size <= 0) {
        return;
    }
    hline(x - size, x + size, y, c);
    for (int j = 1; j <= size; j++) {
        if (x - size + 1 >= 0 && y > 0 && y < H) {
            px(x, y - j, c);
            px(x, y + j, c);
        }
    }
}

static void dotted_ring(int cx, int cy, int r, uint8_t c, int cov)
{
    int rr = r * 16;
    int lo = (rr - 10) * (rr - 10);
    int hi = (rr + 10) * (rr + 10);
    for (int y = cy - r - 2; y <= cy + r + 2; y++) {
        if (y < 0 || y >= H) continue;
        for (int x = cx - r - 2; x <= cx + r + 2; x++) {
            if (x < 0 || x >= W) continue;
            int dx = (x - cx) * 16;
            int dy = (y - cy) * 16;
            int d = dx * dx + dy * dy;
            if (d < lo || d > hi) continue;
            if (((x + 2 * y) & 1) != 0) continue;
            if (BAYER[y & 3][x & 3] >= cov) continue;
            px(x, y, c);
        }
    }
}

static void heart(int x, int y, uint8_t c)
{
    static const uint8_t P[5] = { 0x0a, 0x1f, 0x1f, 0x0e, 0x04 };
    stamp(x, y, P, 5, c);
    px(x + 1, y + 1, C_WHITE);
}

/* ---------------------------------------------------------------------------
 * Frame: screen pixel -> grid cell, and the blown-up draw.
 * ------------------------------------------------------------------------- */

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
            memcpy(dst, prev, (size_t)n * sizeof(uint16_t));
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
 * Persistent animation state (cold-start safe: all zero / identity).
 * ------------------------------------------------------------------------- */

static struct {
    bool ready;
    float last_t;
    float next_blink;
    float blink_end;
    float blink_end2;
    float next_gaze;
    float gx;
    float gy;
    float gx_t;
    float gy_t;
    uint32_t rng;
} st = { .next_blink = 1.6f, .blink_end2 = -100.0f, .next_gaze = 0.9f,
         .rng = 0x9e3779b9u };

static float frand(void)
{
    st.rng ^= st.rng << 13;
    st.rng ^= st.rng >> 17;
    st.rng ^= st.rng << 5;
    return (float)(st.rng & 0xffff) / 65535.0f;
}

/* ---------------------------------------------------------------------------
 * Render
 * ------------------------------------------------------------------------- */

void muse_pixel_render(const muse_pose_t *p)
{
    muse_mode_t mode = p->mode;
    if ((unsigned)mode >= MUSE_MODE_COUNT) {
        mode = MUSE_MODE_IDLE;
    }

    float t = p->t;
    float mt = p->mode_t < 0.0f ? 0.0f : p->mode_t;
    float level = p->level < 0.0f ? 0.0f : (p->level > 1.0f ? 1.0f : p->level);
    float happy = p->happy < 0.0f ? 0.0f : (p->happy > 1.0f ? 1.0f : p->happy);
    if (mode == MUSE_MODE_ERROR) {
        happy = 0.0f; /* the alert face never turns joyful */
    }

    float dt;
    if (!st.ready) {
        dt = 0.04f;
        st.ready = true;
    } else {
        dt = t - st.last_t;
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 0.2f) dt = 0.2f;
    }
    st.last_t = t;

    /* ---- timers: blinks and gaze -------------------------------------- */
    if (t >= st.next_blink) {
        st.blink_end = t + 0.13f;
        st.blink_end2 = frand() < 0.35f ? t + 0.31f : -100.0f;
        st.next_blink = t + 2.2f + frand() * 3.0f;
    }
    bool blinking = t < st.blink_end || (t >= st.blink_end2 && t < st.blink_end2 + 0.13f);

    if (t >= st.next_gaze) {
        st.gx_t = frand() * 2.0f - 1.0f;
        st.gy_t = frand() * 2.0f - 1.0f;
        st.next_gaze = t + 1.2f + frand() * 2.4f;
    }
    float kg = dt * 14.0f;
    if (kg > 1.0f) kg = 1.0f;
    st.gx += (st.gx_t - st.gx) * kg;
    st.gy += (st.gy_t - st.gy) * kg;
    int gaze_x = (int)(st.gx + (st.gx >= 0.0f ? 0.5f : -0.5f));
    int gaze_y = (int)(st.gy + (st.gy >= 0.0f ? 0.5f : -0.5f));
    if (gaze_x > 1) gaze_x = 1;
    if (gaze_x < -1) gaze_x = -1;
    if (gaze_y > 1) gaze_y = 1;
    if (gaze_y < -1) gaze_y = -1;

    /* ---- palette ------------------------------------------------------ */
    int bright = 256;
    if (mode == MUSE_MODE_OFF) {
        float fade = mt < 1.0f ? mt : 1.0f;
        bright = (int)((1.0f - 0.10f * fade) * 256.0f);
    }
    palette_frame(mode, dt, bright);

    memset(s_fb, C_BG, sizeof(s_fb));
    memset(s_mask, 0, sizeof(s_mask));

    /* ---- rig: squash, breathe, bob ------------------------------------ */
    int sy = 1000;
    int sx = 1000;
    if (mode == MUSE_MODE_BOOT && mt < 0.62f) {
        int u = (int)(mt * 1000.0f / 0.62f);
        if (u < 720) {
            sy = 600 + 520 * u / 720;
        } else {
            sy = 1120 - 120 * (u - 720) / 280;
        }
        sx = 2000 - sy;
        if (sx > 1440) sx = 1440;
        if (sx < 880) sx = 880;
    }
    int breathe = 1000 + ((qsin16((int)(t * 900.0f * 65536.0f / 1000.0f)) * 28) >> 15);
    sy = sy * breathe / 1000;
    sx = sx * breathe / 1000;

    int rx = BODY_RX * sx / 1000;
    int ry = BODY_RY * sy / 1000;
    if (rx < 10) rx = 10;
    if (ry < 9) ry = 9;

    int cx = BODY_CX;
    int cy = BODY_FLOOR - ry;

    int hop = 0;
    if (happy > 0.15f) {
        int h = (qsin16((int)(t * 2.2f * 65536.0f)) * 3) >> 15;
        hop = (h < 0 ? -h : h) * (int)(happy * 1000.0f) / 1000;
    }

    if (mode == MUSE_MODE_LISTENING) {
        int dy = osc_px(t * 1.35f, 1);
        cy -= dy < 0 ? -dy : dy;
    } else if (mode == MUSE_MODE_THINKING) {
        cx += osc_px(t * 0.5f, 1);
        cy -= osc_px(t * 0.8f, 1);
    } else if (mode == MUSE_MODE_SPEAKING) {
        cy -= (int)(level * 2.0f + 0.4f);
    } else if (mode == MUSE_MODE_IDLE) {
        cy -= osc_px(t * 0.55f, 1) < 0 ? -osc_px(t * 0.55f, 1) : osc_px(t * 0.55f, 1);
    } else if (mode == MUSE_MODE_ERROR && mt < 0.6f) {
        cx += osc_px(t * 6.0f, 2);
    } else if (mode == MUSE_MODE_OFF) {
        cy += 1;
    }
    cy -= hop;

    /* ---- expression --------------------------------------------------- */
    int eye_style = EYE_BEAN;
    int mouth = M_SMILE;
    bool big_eyes = false;
    bool brows = false;
    bool angry_brows = false;
    int blush_w = 4;
    int mouth_open = 0;
    int gx = gaze_x;
    int gy = gaze_y;

    switch (mode) {
    case MUSE_MODE_BOOT:
        eye_style = mt < 0.9f ? EYE_CLOSED : EYE_BEAN;
        mouth = M_SMILE;
        break;
    case MUSE_MODE_LISTENING:
        eye_style = EYE_WIDE;
        big_eyes = true;
        mouth = M_O;
        brows = true;
        gx = 0;
        gy = 0;
        break;
    case MUSE_MODE_THINKING:
        mouth = M_WAVE;
        gx = 1;
        gy = -1;
        if (((int)(t * 0.7f)) & 1) {
            gx = -1;
        }
        break;
    case MUSE_MODE_SPEAKING:
        mouth = M_TALK;
        mouth_open = 1 + (int)(level * 5.0f + 0.7f);
        blush_w = 5;
        break;
    case MUSE_MODE_ERROR:
        eye_style = EYE_X;
        mouth = M_FLAT;
        angry_brows = true;
        gx = 0;
        gy = 0;
        break;
    case MUSE_MODE_OFF:
        eye_style = mt > 0.55f ? EYE_CLOSED : EYE_BEAN;
        mouth = mt > 0.55f ? M_FLAT : M_SMILE;
        gx = 0;
        gy = 0;
        break;
    default:
        if (blinking) {
            eye_style = EYE_CLOSED;
        }
        break;
    }

    if (happy > 0.12f) {
        eye_style = EYE_HAPPY;
        mouth = M_GRIN;
        blush_w = 5;
        gx = 0;
        gy = 0;
    }
    if (happy <= 0.12f && mode != MUSE_MODE_BOOT && mode != MUSE_MODE_LISTENING &&
        mode != MUSE_MODE_ERROR && mode != MUSE_MODE_OFF && blinking) {
        eye_style = EYE_CLOSED;
    }

    int face_cy = cy + (32 - BODY_CY) * sy / 1000;
    int mouth_cy = cy + (40 - BODY_CY) * sy / 1000;
    int blush_cy = cy + (37 - BODY_CY) * sy / 1000;
    int eye_dx = 7 * sx / 1000;

    /* ---- shadow, back sparkles, rings --------------------------------- */
    int sh_rx = 11 * sx / 1000;
    for (int y = 53; y <= 59; y++) {
        for (int x = cx - sh_rx - 3; x <= cx + sh_rx + 3; x++) {
            int dx = x - cx;
            int dy = y - 56;
            int f = (dx * dx * ONE) / (sh_rx * sh_rx) + (dy * dy * ONE) / 9;
            if (f < 2600 || (f < ONE && BAYER[y & 3][x & 3] < 9)) {
                px(x, y, C_SHADOW);
            }
        }
    }

    if (mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_SPEAKING) {
        int per = 1500; /* ms per ring */
        int ph = (int)(mt * 1000.0f) % per;
        for (int k = 0; k < 2; k++) {
            int r = 18 + (ph + k * per / 2) * 13 / per;
            int fade = 10 - (r - 18) * 10 / 14;
            if (fade < 2) fade = 2;
            dotted_ring(cx, cy + 1, r, C_ACCENT, fade);
        }
        if (level > 0.05f) {
            int len = 1 + (int)(level * 4.0f);
            for (int k = 0; k < 3; k++) {
                int l = len - k;
                if (l < 1) l = 1;
                int y = cy - 3 + k * 3;
                hline(cx - 13 - k * 2 - l, cx - 13 - k * 2, y, C_ACCENT);
                hline(cx + 13 + k * 2, cx + 13 + k * 2 + l, y, C_ACCENT);
            }
        }
    }

    /* Three sparkles orbiting: two behind the body. */
    int spark_on = mode == MUSE_MODE_OFF ? 0 : 3;
    if (mode == MUSE_MODE_BOOT) {
        spark_on = 1 + (int)(mt / 0.18f);
        if (spark_on > 3) spark_on = 3;
    }
    for (int k = 0; k < spark_on; k++) {
        int ph = (int)((t * 0.11f + (float)k * 0.333f) * 65536.0f);
        int sx_ = cx + (qsin16(ph) * 26 >> 15);
        int sy_ = 33 + (qsin16(ph + 16384) * 21 >> 15);
        int tw = (qsin16((int)(t * 2.1f * 65536.0f) + k * 20000) > 0) ? 1 : 0;
        if (k < 2) {
            sparkle(sx_, sy_, tw, C_GLOW1, C_SPARK);
        }
    }

    /* ---- body --------------------------------------------------------- */
    int aura = 200;
    if (mode == MUSE_MODE_LISTENING) aura = 230 + (int)(level * 26.0f);
    if (mode == MUSE_MODE_SPEAKING) aura = 235;
    if (mode == MUSE_MODE_ERROR) aura = 256;
    if (mode == MUSE_MODE_BOOT) aura = 256;
    if (mode == MUSE_MODE_OFF) {
        float fade = mt < 1.0f ? 1.0f - mt : 0.0f;
        aura = (int)(fade * 150.0f);
    }
    if (happy > 0.12f) aura = 255;

    draw_body(cx, cy, rx, ry, aura);

    /* ---- paws --------------------------------------------------------- */
    int paw_r = 2;
    int paw_y = cy + 4;
    int paw_x = rx - 1;
    int plx = cx - paw_x, ply = paw_y;
    int prx = cx + paw_x, pry = paw_y;

    switch (mode) {
    case MUSE_MODE_LISTENING:
        ply = pry = cy - 3;
        plx = cx - rx + 1;
        prx = cx + rx - 1;
        break;
    case MUSE_MODE_THINKING:
        /* paw held in front of the jaw, drawn on top of the body below */
        prx = cx + 8;
        pry = cy + 5;
        break;
    case MUSE_MODE_SPEAKING:
        ply = cy + 4 - (int)(level * 5.0f);
        pry = cy + 4 + (int)(level * 2.0f);
        break;
    case MUSE_MODE_ERROR:
        ply = pry = cy + 7;
        break;
    case MUSE_MODE_OFF:
        if (mt < 0.55f) {
            prx = cx + rx - 1 + osc_px(mt * 1.6f, 2);
            pry = cy - 5;
        } else {
            ply = pry = cy + 7;
        }
        break;
    default:
        if (happy > 0.12f) {
            ply = pry = cy - 7 - (int)(happy * 2.0f);
            plx = cx - rx + 2;
            prx = cx + rx - 2;
            int w = osc_px(t * 3.0f, 1);
            plx += w;
            prx -= w;
        } else {
            ply += osc_px(t * 0.8f, 1);
            pry += osc_px(t * 0.8f + 0.5f, 1);
        }
        break;
    }
    if (mode == MUSE_MODE_BOOT) {
        ply = pry = cy + 6;
    }

    draw_paw(plx, ply, paw_r);
    if (!(mode == MUSE_MODE_OFF && mt >= 0.55f && mt < 0.56f) &&
        mode != MUSE_MODE_THINKING) {
        draw_paw(prx, pry, paw_r);
    }

    /* ---- cowlick ------------------------------------------------------ */
    int sway = osc_px(t * 0.75f, 1);
    if (mode == MUSE_MODE_SPEAKING) {
        sway += level > 0.5f ? 1 : 0;
    }
    if (happy > 0.12f) {
        sway += osc_px(t * 2.4f, 1);
    }
    int tuft_top = cy - ry - 6;
    draw_cowlick(cx + 1, tuft_top, sway);

    outline_pass();

    if (mode == MUSE_MODE_THINKING && happy <= 0.12f) {
        draw_paw_front(prx, pry, paw_r);
    }

    /* ---- face --------------------------------------------------------- */
    int ey = face_cy - 2;
    int exl = cx - eye_dx - 1;
    int exr = cx + eye_dx - 1;

    if (brows) {
        hline(exl - 1, exl + 2, ey - 4, C_EYE);
        hline(exr + 1, exr + 4, ey - 4, C_EYE);
    }
    if (angry_brows) {
        hline(exl - 1, exl + 2, ey - 3, C_EYE);
        hline(exr + 1, exr + 4, ey - 3, C_EYE);
        px(exl - 1, ey - 4, C_EYE);
        px(exr + 4, ey - 4, C_EYE);
    }

    draw_eye(exl, ey, eye_style, big_eyes, gx, gy);
    draw_eye(exr, ey, eye_style, big_eyes, gx, gy);

    blush_blob(cx - eye_dx - 4, blush_cy, blush_w, C_BLUSH);
    blush_blob(cx + eye_dx - blush_w + 1, blush_cy, blush_w, C_BLUSH);

    switch (mouth) {
    case M_GRIN:
        mouth_grin(cx, mouth_cy - 1);
        break;
    case M_O:
        mouth_o(cx, mouth_cy - 2);
        break;
    case M_FLAT:
        hline(cx - 2, cx + 2, mouth_cy, C_MOUTH);
        break;
    case M_WAVE:
        mouth_wave(cx, mouth_cy);
        break;
    case M_TALK:
        mouth_talk(cx, mouth_cy - mouth_open / 2, mouth_open);
        break;
    default:
        mouth_smile(cx, mouth_cy - 1);
        break;
    }

    /* ---- front sparkle, thoughts, alert, hearts ----------------------- */
    for (int k = 2; k < spark_on; k++) {
        int ph = (int)((t * 0.11f + 0.666f) * 65536.0f);
        int sx_ = cx + (qsin16(ph) * 27 >> 15);
        int sy_ = 33 + (qsin16(ph + 16384) * 22 >> 15);
        int tw = (qsin16((int)(t * 2.6f * 65536.0f) + 30000) > 0) ? 1 : 0;
        if (!s_mask[sy_ * W + sx_]) {
            sparkle(sx_, sy_, tw, C_GLOW1, C_SPARK);
        }
    }

    if (mode == MUSE_MODE_THINKING) {
        int ph = (int)(t * 3.0f) % 3;
        for (int i = 0; i < 3; i++) {
            if ((i + ph) % 3 == 2) {
                continue;
            }
            int x = cx + 12 + i * 3;
            int y = cy - 15 - i * 4;
            px(x, y, C_ACCENT);
            px(x + 1, y, C_ACCENT);
            px(x, y + 1, C_ACCENT);
            px(x + 1, y + 1, C_ACCENT);
        }
    }

    if (mode == MUSE_MODE_ERROR) {
        int x = cx + 16;
        int y = cy - 13;
        hline(x, x + 1, y, C_ACCENT);
        hline(x, x + 1, y + 1, C_ACCENT);
        hline(x, x + 1, y + 2, C_ACCENT);
        hline(x, x + 1, y + 4, C_ACCENT);
    }

    if (happy > 0.12f && mode != MUSE_MODE_ERROR) {
        int rise = (int)((1.0f - happy) * 5.0f);
        int hx = cx - 13;
        int hy = cy - 17 - rise + osc_px(t * 1.1f, 1);
        if (happy > 0.3f) {
            heart(hx, hy, C_HEART);
            heart(hx + 21, hy - 3, C_HEART);
        }
    }
}

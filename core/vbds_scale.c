/* vbds_scale.c - 2/3 area-average downscale.
 *
 * Every 3x3 block of source pixels becomes 2x2 output pixels.  Each output
 * is half the area average of the source pixels it covers and half their
 * maximum.  The average alone keeps 1-pixel lines but makes them dim; the
 * maximum keeps them at full brightness.  Values are small integers turned
 * into colours by a palette.
 */
#include "vbds_scale.h"
#include "vbds_vip.h"

#define SRC_H 224

/* Unpack one framebuffer column (64 bytes, 4 rows per byte). */
static void unpack_column(const uint8_t* col, uint8_t* out) {
    for (int i = 0; i < SRC_H / 4; i++) {
        uint8_t b = col[i];
        out[0] = b & 3; out[1] = (b >> 2) & 3; out[2] = (b >> 4) & 3; out[3] = b >> 6;
        out += 4;
    }
}

static inline uint8_t max2(uint8_t x, uint8_t y) { return x > y ? x : y; }

void vbds_downscale(int eye, uint8_t* dst, int stride) {
    const uint8_t* fb = vbds_vip_display_fb(eye);
    uint8_t a[SRC_H + 1], b[SRC_H + 1], c[SRC_H + 1];
    uint8_t lavg[SRC_H + 1], ravg[SRC_H + 1], lmax[SRC_H + 1], rmax[SRC_H + 1];

    for (int cx = 0; cx < 128; cx++) {
        unpack_column(fb + (cx * 3 + 0) * 64, a);
        unpack_column(fb + (cx * 3 + 1) * 64, b);
        unpack_column(fb + (cx * 3 + 2) * 64, c);
        for (int y = 0; y < SRC_H; y++) {
            lavg[y] = (uint8_t)(2 * a[y] + b[y]);      /* 0..9 */
            ravg[y] = (uint8_t)(b[y] + 2 * c[y]);
            lmax[y] = max2(a[y], b[y]);                /* 0..3 */
            rmax[y] = max2(b[y], c[y]);
        }
        lavg[SRC_H] = ravg[SRC_H] = lmax[SRC_H] = rmax[SRC_H] = 0;  /* 224 % 3 */

        uint8_t* out = dst + cx * 2;
        for (int y = 0, oy = 0; oy < VBDS_SCALED_H; y += 3, oy += 2) {
            /* average (0..27) + 9 * max (0..27) = 0..54 */
            out[0] = (uint8_t)(2 * lavg[y] + lavg[y + 1] + 9 * max2(lmax[y], lmax[y + 1]));
            out[1] = (uint8_t)(2 * ravg[y] + ravg[y + 1] + 9 * max2(rmax[y], rmax[y + 1]));
            out += stride;
            if (oy + 1 >= VBDS_SCALED_H) break;
            out[0] = (uint8_t)(lavg[y + 1] + 2 * lavg[y + 2] + 9 * max2(lmax[y + 1], lmax[y + 2]));
            out[1] = (uint8_t)(ravg[y + 1] + 2 * ravg[y + 2] + 9 * max2(rmax[y + 1], rmax[y + 2]));
            out += stride;
        }
    }
}

/* Shade k is k/18 of a framebuffer level; interpolate between levels. */
int vbds_scaled_level(int shade) {
    int lo = shade / 18, frac = shade % 18;
    if (lo >= 3) return vbds_vip_level(3);
    int l0 = vbds_vip_level(lo), l1 = vbds_vip_level(lo + 1);
    return l0 + (l1 - l0) * frac / 18;
}

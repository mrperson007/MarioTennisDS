/* vbds_scale.h - 2/3 area-average downscale of the Virtual Boy picture. */
#ifndef VBDS_SCALE_H
#define VBDS_SCALE_H
#include <stdint.h>

#define VBDS_SCALED_W 256
#define VBDS_SCALED_H 149
#define VBDS_SCALE_SHADES 55        /* output values 0..54 */

/* Downscale one eye's displayed picture (384x224) to 256x149, one byte per
 * pixel, row-major with `stride` bytes per row.  Output values are in
 * units of 1/18 of a framebuffer level (0..54). */
void vbds_downscale(int eye, uint8_t* dst, int stride);

/* Red intensity 0..255 for output value 0..54, from the current
 * brightness settings. */
int vbds_scaled_level(int shade);

#endif

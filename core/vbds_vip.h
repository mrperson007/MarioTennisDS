/* vbds_vip.h - compact Virtual Image Processor for small targets.
 *
 * Same register behaviour, timing state machine and drawing rules as
 * vbrecomp's runtime/src/vip.c (itself a port of Beetle VB), with the
 * desktop-only layers removed (attribution, capture, recolour, rings).
 */
#ifndef VBDS_VIP_H
#define VBDS_VIP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VB_VIP_INT_SCANERR     0x0001u
#define VB_VIP_INT_LFB_END     0x0002u
#define VB_VIP_INT_RFB_END     0x0004u
#define VB_VIP_INT_GAME_START  0x0008u
#define VB_VIP_INT_FRAME_START 0x0010u
#define VB_VIP_INT_SB_HIT      0x2000u
#define VB_VIP_INT_XP_END      0x4000u
#define VB_VIP_INT_TIME_ERR    0x8000u
#define VB_VIP_INT_MASK_VALID  0xE01Fu

void vb_vip_init(void);
void vb_vip_tick(uint64_t cycles);
int32_t vb_vip_cycles_to_next_event(void);

uint8_t  vb_vip_read8 (uint32_t addr);
uint16_t vb_vip_read16(uint32_t addr);
uint32_t vb_vip_read32(uint32_t addr);
void     vb_vip_write8 (uint32_t addr, uint8_t  v);
void     vb_vip_write16(uint32_t addr, uint16_t v);
void     vb_vip_write32(uint32_t addr, uint32_t v);

/* Displayed framebuffer for one eye (0 = left, 1 = right): 384 columns of
 * 64 bytes, 2 bits per pixel, four rows per byte, low bits first. */
const uint8_t* vbds_vip_display_fb(int eye);

/* Red intensity 0..255 for framebuffer pixel value 0..3. */
int vbds_vip_level(int v);

/* Non-zero when the displayed picture or its brightness may have changed
 * since the last call; clears the flag. */
int vbds_vip_take_dirty(void);

/* Draw the right eye as well as the left (default 0: left only). */
void vbds_vip_set_right_eye(int enabled);

#ifdef __cplusplus
}
#endif

#endif

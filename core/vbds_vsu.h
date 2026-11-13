/* vbds_vsu.h - sound chip interface (silent placeholder for now). */
#ifndef VBDS_VSU_H
#define VBDS_VSU_H
#include <stdint.h>
void vb_vsu_init(void);
uint8_t  vb_vsu_read8 (uint32_t addr);
uint16_t vb_vsu_read16(uint32_t addr);
uint32_t vb_vsu_read32(uint32_t addr);
void vb_vsu_write8(uint32_t addr, uint8_t v);
void vb_vsu_tick(uint64_t cpu_cycles);
void vb_vsu_end_frame(void);
/* Last value written to each register slot (0x000-0x5FF, one per 4 bytes). */
extern uint8_t vbds_vsu_regs[0x180];
#endif

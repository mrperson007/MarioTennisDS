/* vbds_vsu.c - sound chip placeholder.
 *
 * The sound registers are write-only, so the game cannot tell that nothing
 * is listening.  Writes are kept so a later mixer can read the state.
 */
#include "vbds_vsu.h"
#include <string.h>

uint8_t vbds_vsu_regs[0x180];

void vb_vsu_init(void) { memset(vbds_vsu_regs, 0, sizeof(vbds_vsu_regs)); }
uint8_t  vb_vsu_read8 (uint32_t addr) { (void)addr; return 0; }
uint16_t vb_vsu_read16(uint32_t addr) { (void)addr; return 0; }
uint32_t vb_vsu_read32(uint32_t addr) { (void)addr; return 0; }
void vb_vsu_write8(uint32_t addr, uint8_t v) {
    uint32_t off = addr & 0x7FFu;
    if (!(off & 3u) && off < 0x600u) vbds_vsu_regs[off >> 2] = v;
}
void vb_vsu_tick(uint64_t cpu_cycles) { (void)cpu_cycles; }
void vb_vsu_end_frame(void) {}

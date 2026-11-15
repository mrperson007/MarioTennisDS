/* vbds_core.c - CPU state and the per-frame scheduler.
 *
 * The scheduler follows the main loop of vbrecomp runtime/src/main.cpp:
 * deliver a pending interrupt, then either idle to the next device event
 * (CPU halted) or run recompiled code up to it.
 */
#include "vbds.h"

#include <string.h>

#include "interrupts.h"
#include "stub_abort.h"
#include "vb_crc32.h"

CPUState vbds_cpu;
uint32_t (*vbds_tick_source)(void);
uint32_t vbds_prof_draw_ticks, vbds_prof_blocks_drawn, vbds_prof_blocks_skipped;
uint32_t vbds_prof_passes, vbds_prof_game_ticks;
static uint64_t s_frame_start;

void vbds_bus_init(const uint8_t* rom, uint32_t rom_size);
int32_t vbds_cycles_to_next_event(uint64_t cycle);
void vbds_devices_sync(uint64_t cycle);
void vbds_devices_end_frame(uint64_t cycle);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void vbds_on_game_frame(void) {}

void vb_stub_abort(const char* what, uint32_t pc, uint32_t addr) {
    vbds_fatal(what, pc ? pc : vbds_cpu.pc, addr);
}
void vb_stub_abort_simple(const char* what, uint32_t pc) {
    vbds_fatal(what, pc, 0xFFFFFFFFu);
}

int vbds_init(const uint8_t* rom, uint32_t rom_size) {
    if (!rom || !rom_size || (rom_size & (rom_size - 1u))) return -1;
    uint32_t want = vb_game_expected_crc32();
    if (want && vb_crc32_compute(rom, rom_size) != want) return -1;

    memset(&vbds_cpu, 0, sizeof(vbds_cpu));
    vbds_cpu.read8 = vb_read8;   vbds_cpu.read16 = vb_read16;   vbds_cpu.read32 = vb_read32;
    vbds_cpu.write8 = vb_write8; vbds_cpu.write16 = vb_write16; vbds_cpu.write32 = vb_write32;
    vbds_bus_init(rom, rom_size);
    vb_cpu_reset(&vbds_cpu);
    s_frame_start = 0;
    return 0;
}

static int irq_acceptable(const CPUState* c) {
    int level = vb_irq_highest_pending_level();
    return level >= 0 && !c->psw_np && !c->psw_ep && !c->psw_id
        && level >= (int)c->psw_int_level;
}

void vbds_run_frame(void) {
    CPUState* c = &vbds_cpu;
    const uint64_t frame_end = s_frame_start + VBDS_CYCLES_PER_FRAME;

    while (c->cycles < frame_end) {
        vb_irq_check_and_deliver(c);
        vbds_prof_passes++;

        if (c->halted) {
            while (c->cycles < frame_end) {
                int32_t step = vbds_cycles_to_next_event(c->cycles);
                if (step < 1) step = 1;
                uint64_t remaining = frame_end - c->cycles;
                if ((uint64_t)step > remaining) step = (int32_t)remaining;
                c->cycles += (uint64_t)step;
                vbds_devices_sync(c->cycles);
                if (irq_acceptable(c)) break;
            }
            continue;
        }

        int32_t next = vbds_cycles_to_next_event(c->cycles);
        if (next < 1) next = 1;
        c->cycle_deadline = c->cycles + (uint64_t)next;
        if (c->cycle_deadline > frame_end) c->cycle_deadline = frame_end;
        c->step_budget = 250000;
        c->yielded = 0;
        /* Game time, not counting video drawn from inside the game's
         * memory accesses (that is counted as drawing). */
        uint32_t t0 = vbds_tick_source ? vbds_tick_source() : 0;
        uint32_t d0 = vbds_prof_draw_ticks;
        vb_dispatch(c, c->pc);
        if (vbds_tick_source)
            vbds_prof_game_ticks += vbds_tick_source() - t0 - (vbds_prof_draw_ticks - d0);
        vbds_devices_sync(c->cycles);
        if (!c->yielded && !c->halted)
            vbds_fatal("dispatcher returned without yielding or halting", c->pc, 0);
    }

    s_frame_start = frame_end;
    vbds_devices_end_frame(c->cycles);
    c->frame++;
}

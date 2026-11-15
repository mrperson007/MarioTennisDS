/* vbds_bus.c - guest memory map and device scheduling.
 *
 * Derived from vbrecomp runtime/src/memory.c (MIT, see vbrecomp/LICENSE).
 * Same map and the same synchronise-on-access rules; trace rings, write
 * observers and file loading are removed.
 */
#include "vbds.h"

#include <string.h>

#include "input.h"
#include "interrupts.h"
#include "timer.h"
#include "vbds_vip.h"
#include "vbds_vsu.h"

static const uint8_t* s_rom;
static uint32_t s_rom_mask;
static uint32_t s_wram_store[VBDS_WRAM_SIZE / 4];
static uint8_t  s_cart_ram[65536];
static uint8_t  s_misc_wcr;
#define s_wram ((uint8_t*)s_wram_store)

uint8_t*       vbds_wram_ptr = (uint8_t*)s_wram_store;
const uint8_t* vbds_rom_ptr;
uint32_t       vbds_rom_mask;

static uint64_t s_timer_cycle, s_vip_cycle, s_vsu_cycle, s_input_cycle;

const uint8_t* vbds_wram(void) { return s_wram; }

void vbds_bus_init(const uint8_t* rom, uint32_t rom_size) {
    s_rom = rom;
    s_rom_mask = rom_size - 1u;
    vbds_rom_ptr = rom;
    vbds_rom_mask = rom_size - 1u;
    s_timer_cycle = s_vip_cycle = s_vsu_cycle = s_input_cycle = 0;
    s_misc_wcr = 0;
    memset(s_wram_store, 0, sizeof(s_wram_store));
    memset(s_cart_ram, 0, sizeof(s_cart_ram));
    vb_vip_init();
    vb_vsu_init();
    vb_input_init();
    vb_irq_init();
    vb_timer_init();
}

/* Cycles until the timer can next raise its interrupt.
 *
 * The reference runtime stops at every timer divider tick (400 or 2,000
 * cycles).  Counter and status reads are brought up to date on access, so
 * the only thing a tick can do behind the CPU's back is assert the
 * interrupt: when the counter reaches zero, or on the very next tick while
 * the zero flag is still latched.  The estimate may be early, never late. */
static int32_t timer_horizon(void) {
#if VBDS_COARSE_EVENTS
    uint8_t tcr = vb_timer_control();
    if (!(tcr & VB_TCR_TENABLE) || !(tcr & VB_TCR_TIMZINT)) return 0x3FFFFFFF;
    int32_t divider = vb_timer_divider();
    if (divider < 1) divider = 1;
    if (vb_timer_read8(0x02000020u) & VB_TCR_ZSTAT) return divider;
    uint32_t ticks = vb_timer_counter(), reload = vb_timer_reload();
    if (reload < 1) reload = 1;
    if (ticks > reload) ticks = reload;     /* a pending reload may shorten it */
    if (ticks < 1) ticks = 1;
    uint32_t period = (tcr & VB_TCR_TCLKSEL) ? 400u : 2000u;
    uint64_t n = (uint64_t)divider + (uint64_t)(ticks - 1) * period;
    return n > 0x3FFFFFFF ? 0x3FFFFFFF : (int32_t)n;
#else
    return vb_timer_cycles_to_next_event();
#endif
}

uint64_t vbds_access_cycle(void) {
    return vbds_cpu.cycles - vbds_cpu.bus_tail_cycles;
}

int32_t vbds_cycles_to_next_event(uint64_t cycle) {
    uint64_t next = s_vip_cycle + (uint64_t)vb_vip_cycles_to_next_event();
    uint64_t timer = s_timer_cycle + (uint64_t)timer_horizon();
    if (timer < next) next = timer;
    uint64_t input = s_input_cycle + (uint64_t)vb_input_cycles_to_next_event();
    if (input < next) next = input;
    return next > cycle ? (int32_t)(next - cycle) : 1;
}

void vbds_devices_sync(uint64_t cycle) {
    if (cycle > s_timer_cycle) { vb_timer_tick((uint32_t)(cycle - s_timer_cycle)); s_timer_cycle = cycle; }
    if (cycle > s_input_cycle) { vb_input_tick((uint32_t)(cycle - s_input_cycle)); s_input_cycle = cycle; }
    if (cycle >= s_vip_cycle + (uint64_t)vb_vip_cycles_to_next_event()) {
        vb_vip_tick(cycle - s_vip_cycle); s_vip_cycle = cycle;
    }
}

void vbds_devices_end_frame(uint64_t cycle) {
    vb_vsu_tick(cycle - s_vsu_cycle); s_vsu_cycle = cycle;
    vb_vsu_end_frame();
}

/* Timer, pad and sound registers are brought up to the access cycle
 * before they are touched. */
static void io_begin(unsigned region, uint32_t addr, int writing) {
    uint64_t cycle = vbds_cpu.cycles - vbds_cpu.bus_tail_cycles;
    if (region == 2) {
        if (cycle > s_timer_cycle) { vb_timer_tick((uint32_t)(cycle - s_timer_cycle)); s_timer_cycle = cycle; }
        if (cycle > s_input_cycle) { vb_input_tick((uint32_t)(cycle - s_input_cycle)); s_input_cycle = cycle; }
    } else if (region == 1 && writing && !(addr & 3) && cycle >= s_vsu_cycle) {
        vb_vsu_tick(cycle - s_vsu_cycle); s_vsu_cycle = cycle;
    }
}

/* A register access may have armed a timer or unmasked an interrupt, so
 * the CPU's deadline is pulled in to the next device event. */
static void io_end(void) {
    CPUState* c = &vbds_cpu;
    int32_t next = vb_vip_cycles_to_next_event(), timer = timer_horizon();
    uint64_t deadline = s_vip_cycle + (uint64_t)(next > 0 ? next : 1);
    uint64_t timer_deadline = s_timer_cycle + (uint64_t)(timer > 0 ? timer : 1);
    if (timer_deadline < deadline) deadline = timer_deadline;
    uint64_t input_deadline = s_input_cycle + (uint64_t)vb_input_cycles_to_next_event();
    if (input_deadline < deadline) deadline = input_deadline;
    int level = vb_irq_highest_pending_level();
    if (level >= 0 && !c->psw_id && !c->psw_ep && !c->psw_np && level >= c->psw_int_level)
        deadline = c->cycles;
    if (deadline < c->cycle_deadline) c->cycle_deadline = deadline;
}

static inline int misc_is_input(uint32_t lo) { return lo == 0x10 || lo == 0x14 || lo == 0x28; }
static inline int misc_is_timer(uint32_t lo) { return lo == 0x18 || lo == 0x1C || lo == 0x20; }

static uint32_t io_read(unsigned region, uint32_t p, unsigned width) {
    uint32_t r = 0;
    io_begin(region, p, 0);
    switch (region) {
        case 0:
            r = width == 1 ? vb_vip_read8(p) : width == 2 ? vb_vip_read16(p) : vb_vip_read32(p);
            break;
        case 1:
            r = width == 1 ? vb_vsu_read8(p) : width == 2 ? vb_vsu_read16(p) : vb_vsu_read32(p);
            break;
        case 2: {
            uint32_t lo = p & 0xFFu;
            if (lo & 0x3u) break;
            if (misc_is_timer(lo))
                r = width == 1 ? vb_timer_read8(p) : width == 2 ? vb_timer_read16(p) : vb_timer_read32(p);
            else if (misc_is_input(lo))
                r = width == 1 ? vb_input_read8(p) : width == 2 ? vb_input_read16(p) : vb_input_read32(p);
            else if (lo == 0x24)
                r = (uint32_t)(s_misc_wcr | 0xFCu);
            break;
        }
        case 3: vbds_fatal("read from reserved region 0x03000000", vbds_cpu.pc, p);
        default: vbds_fatal("cartridge expansion read", vbds_cpu.pc, p);
    }
    io_end();
    return r;
}

static void io_write(unsigned region, uint32_t p, uint32_t v, unsigned width) {
    io_begin(region, p, 1);
    if (region == 1) { vb_vsu_write8(p, (uint8_t)v); return; }
    switch (region) {
        case 0:
            if (width == 1) vb_vip_write8(p, (uint8_t)v);
            else if (width == 2) vb_vip_write16(p, (uint16_t)v);
            else vb_vip_write32(p, v);
            break;
        case 1:
            vb_vsu_write8(p, (uint8_t)v);
            break;
        case 2: {
            uint32_t lo = p & 0xFFu;
            if (lo & 0x3u) break;
            if (misc_is_timer(lo)) vb_timer_write8(p, (uint8_t)v);
            else if (misc_is_input(lo)) vb_input_write8(p, (uint8_t)v);
            else if (lo == 0x24) s_misc_wcr = (uint8_t)(v & 0x3u);
            break;
        }
        case 3: vbds_fatal("write to reserved region 0x03000000", vbds_cpu.pc, p);
        default: vbds_fatal("cartridge expansion write", vbds_cpu.pc, p);
    }
    io_end();
}

#define REGION(a) (((a) >> 24) & 7u)

/* Video memory (not registers) has no timing side effects, so it skips the
 * device synchronisation that register accesses need. */
static inline int vip_is_reg(uint32_t addr) {
    uint32_t o = addr & 0x7FFFFu;
    return o >= 0x5F800u && o < 0x5F900u;
}

VBDS_FAST uint8_t vb_read8(uint32_t addr) {
    unsigned region = REGION(addr);
    if (region == 5) return s_wram[addr & 0xFFFFu];
    if (region == 7) return s_rom[addr & s_rom_mask];
    if (region == 6) return s_cart_ram[addr & 0xFFFFu];
    if (region == 0 && !vip_is_reg(addr)) return vb_vip_read8(addr);
    return (uint8_t)io_read(region, addr & 0x07FFFFFFu, 1);
}

VBDS_FAST uint16_t vb_read16(uint32_t addr) {
    unsigned region = REGION(addr);
    addr &= ~1u;
    if (region == 5) { const uint8_t* p = &s_wram[addr & 0xFFFFu]; return (uint16_t)(p[0] | (p[1] << 8)); }
    if (region == 7) { const uint8_t* p = &s_rom[addr & s_rom_mask]; return (uint16_t)(p[0] | (p[1] << 8)); }
    if (region == 6) return (uint16_t)(s_cart_ram[addr & 0xFFFFu] | (s_cart_ram[(addr + 1) & 0xFFFFu] << 8));
    if (region == 0 && !vip_is_reg(addr)) return vb_vip_read16(addr);
    return (uint16_t)io_read(region, addr & 0x07FFFFFFu, 2);
}

VBDS_FAST uint32_t vb_read32(uint32_t addr) {
    unsigned region = REGION(addr);
    addr &= ~3u;
    if (region == 5) { uint32_t v; memcpy(&v, &s_wram[addr & 0xFFFFu], 4); return v; }
    if (region == 7) { uint32_t v; memcpy(&v, &s_rom[addr & s_rom_mask], 4); return v; }
    if (region == 6) { uint32_t v; memcpy(&v, &s_cart_ram[addr & 0xFFFFu], 4); return v; }
    if (region == 0 && !vip_is_reg(addr)) return vb_vip_read32(addr);
    return io_read(region, addr & 0x07FFFFFFu, 4);
}

VBDS_FAST void vb_write8(uint32_t addr, uint8_t v) {
    unsigned region = REGION(addr);
    if (region == 5) { s_wram[addr & 0xFFFFu] = v; return; }
    if (region == 6) { s_cart_ram[addr & 0xFFFFu] = v; return; }
    if (region == 7) vbds_fatal("write to cartridge ROM", vbds_cpu.pc, addr);
    if (region == 0 && !vip_is_reg(addr)) { vb_vip_write8(addr, v); return; }
    io_write(region, addr & 0x07FFFFFFu, v, 1);
}

VBDS_FAST void vb_write16(uint32_t addr, uint16_t v) {
    unsigned region = REGION(addr);
    addr &= ~1u;
    if (region == 5) { uint8_t* p = &s_wram[addr & 0xFFFFu]; p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); return; }
    if (region == 6) { s_cart_ram[addr & 0xFFFFu] = (uint8_t)v; s_cart_ram[(addr + 1) & 0xFFFFu] = (uint8_t)(v >> 8); return; }
    if (region == 7) vbds_fatal("write to cartridge ROM", vbds_cpu.pc, addr);
    if (region == 0 && !vip_is_reg(addr)) { vb_vip_write16(addr, v); return; }
    io_write(region, addr & 0x07FFFFFFu, v, 2);
}

VBDS_FAST void vb_write32(uint32_t addr, uint32_t v) {
    unsigned region = REGION(addr);
    addr &= ~3u;
    if (region == 5) { memcpy(&s_wram[addr & 0xFFFFu], &v, 4); return; }
    if (region == 6) { memcpy(&s_cart_ram[addr & 0xFFFFu], &v, 4); return; }
    if (region == 7) vbds_fatal("write to cartridge ROM", vbds_cpu.pc, addr);
    if (region == 0 && !vip_is_reg(addr)) { vb_vip_write32(addr, v); return; }
    io_write(region, addr & 0x07FFFFFFu, v, 4);
}

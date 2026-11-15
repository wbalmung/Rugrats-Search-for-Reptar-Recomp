/* Shared state for recompiled code and the native runtime. */
#pragma once
#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t r[32];
    uint32_t hi, lo;
    uint32_t cycles;      /* estimated R3000 cycles, wraps */
    uint32_t next_event;  /* cycle stamp at which rt_event() must run */
    uint32_t sr, cause, epc;
    uint32_t last_loop;   /* debug: last backward-branch target */
} Cpu;

extern Cpu cpu;
extern uint8_t ram[0x800000];   /* 2 MB PS1 RAM + 6 MB extension (hooks.c: the draw packet ring lives at 0x80200000) */
extern uint8_t scratch[0x400];

typedef void (*rt_func)(void);
typedef struct { uint32_t addr; rt_func fn; } rt_func_entry;
extern const rt_func_entry rt_func_table[];
extern const int rt_func_count;

/* runtime entry points called by generated code */
void rt_event(void);
void rt_dispatch(uint32_t target);
void rt_syscall(uint32_t pc);
void rt_break(uint32_t pc);
void rt_invalid(uint32_t pc);
void rt_fallthrough_error(uint32_t pc);
void bios_call(uint32_t table, uint32_t fn);
void rt_div(uint32_t a, uint32_t b);
void rt_divu(uint32_t a, uint32_t b);

uint32_t io_read(uint32_t addr, int size);
void io_write(uint32_t addr, uint32_t v, int size);

uint32_t gte_read(int reg);
void gte_swc2_sxy(uint32_t addr, int reg);
void gte_lwc2_sxy(int reg, uint32_t addr);
void gte_mfc2_sxy(int rt, int reg);
void gte_mtc2_sxy(int reg, uint32_t v, uint32_t src);
extern uint32_t tag_ld[32];   /* address each register was last loaded from (lw), see subpx_tag_cp */
int subpx_tag_cp(uint32_t dst, uint32_t src, uint32_t val);
extern uint32_t tag_lpc[32];
void tag_site(uint32_t lw_pc, uint32_t sw_pc);
void rt_watch(uint32_t a, uint32_t pc, uint32_t v, int rt, uint32_t src);
extern uint8_t tag_pg[2048];   /* 4 KB RAM pages that have held a tagged screen position (gpu.c subpx_tag) */
/* worth a tag lookup: a register from mfc2 (0xFFFFFF00 | rt), the scratchpad, or a page that held tags */
static inline int tag_src(uint32_t a) {
    return a >= 0xFFFFFF00u || (a & 0x7FFFFC00u) == 0x1F800000u || ((a & 0x1F800000u) == 0 && tag_pg[(a >> 12) & 2047]);
}
void gte_write(int reg, uint32_t v);
void gte_op(uint32_t op);
uint32_t cop0_read(int reg);
void cop0_write(int reg, uint32_t v);
void cop0_rfe(void);

#define RT_ENTER() do { cpu.cycles += 4; if ((int32_t)(cpu.cycles - cpu.next_event) >= 0) rt_event(); } while (0)
#define RT_POLL(a)  do { cpu.last_loop = (a); cpu.cycles += 8; if ((int32_t)(cpu.cycles - cpu.next_event) >= 0) rt_event(); } while (0)
#define RT_RETURN(ra) ((void)0)

/* ---- memory --------------------------------------------------------- */
/* KUSEG/KSEG0/KSEG1 all mirror the same physical space. The PS1 has 2 MB mirrored x4 in 8 MB; here all 8 MB are
   distinct memory (2026-11-14, bigger draw packet ring, hooks.c). The game itself only uses the first 2 MB. */
static inline int is_ram(uint32_t a) { return (a & 0x1FFFFFFF) < 0x00800000; }
static inline int is_scratch(uint32_t a) { return (a & 0x7FFFFC00) == 0x1F800000 && (a >> 29) != 5; }

static inline uint32_t mem_r32(uint32_t a) {
    if (is_ram(a)) { uint32_t v; memcpy(&v, ram + (a & 0x7FFFFC), 4); return v; }
    if (is_scratch(a)) { uint32_t v; memcpy(&v, scratch + (a & 0x3FC), 4); return v; }
    return io_read(a & ~3u, 4);
}
static inline uint32_t mem_r16(uint32_t a) {
    if (is_ram(a)) { uint16_t v; memcpy(&v, ram + (a & 0x7FFFFE), 2); return v; }
    if (is_scratch(a)) { uint16_t v; memcpy(&v, scratch + (a & 0x3FE), 2); return v; }
    return io_read(a & ~1u, 2) & 0xFFFF;
}
static inline uint32_t mem_r8(uint32_t a) {
    if (is_ram(a)) return ram[a & 0x7FFFFF];
    if (is_scratch(a)) return scratch[a & 0x3FF];
    return io_read(a, 1) & 0xFF;
}

extern int cache_isolated; /* SR bit 16: writes go to I-cache, not memory */

static inline void mem_w32(uint32_t a, uint32_t v) {
    if (cache_isolated) return;
    if (is_ram(a)) { memcpy(ram + (a & 0x7FFFFC), &v, 4); return; }
    if (is_scratch(a)) { memcpy(scratch + (a & 0x3FC), &v, 4); return; }
    io_write(a & ~3u, v, 4);
}
static inline void mem_w16(uint32_t a, uint32_t v) {
    if (cache_isolated) return;
    uint16_t h = (uint16_t)v;
    if (is_ram(a)) { memcpy(ram + (a & 0x7FFFFE), &h, 2); return; }
    if (is_scratch(a)) { memcpy(scratch + (a & 0x3FE), &h, 2); return; }
    io_write(a & ~1u, h, 2);
}
static inline void mem_w8(uint32_t a, uint32_t v) {
    if (cache_isolated) return;
    if (is_ram(a)) { ram[a & 0x7FFFFF] = (uint8_t)v; return; }
    if (is_scratch(a)) { scratch[a & 0x3FF] = (uint8_t)v; return; }
    io_write(a, v & 0xFF, 1);
}

/* unaligned word pieces (little endian R3000 semantics) */
static inline uint32_t mem_lwl(uint32_t cur, uint32_t a) {
    uint32_t w = mem_r32(a & ~3u);
    switch (a & 3) {
    case 0: return (cur & 0x00FFFFFF) | (w << 24);
    case 1: return (cur & 0x0000FFFF) | (w << 16);
    case 2: return (cur & 0x000000FF) | (w << 8);
    default: return w;
    }
}
static inline uint32_t mem_lwr(uint32_t cur, uint32_t a) {
    uint32_t w = mem_r32(a & ~3u);
    switch (a & 3) {
    case 0: return w;
    case 1: return (cur & 0xFF000000) | (w >> 8);
    case 2: return (cur & 0xFFFF0000) | (w >> 16);
    default: return (cur & 0xFFFFFF00) | (w >> 24);
    }
}
static inline void mem_swl(uint32_t a, uint32_t v) {
    uint32_t al = a & ~3u, w = mem_r32(al);
    switch (a & 3) {
    case 0: w = (w & 0xFFFFFF00) | (v >> 24); break;
    case 1: w = (w & 0xFFFF0000) | (v >> 16); break;
    case 2: w = (w & 0xFF000000) | (v >> 8); break;
    default: w = v; break;
    }
    mem_w32(al, w);
}
static inline void mem_swr(uint32_t a, uint32_t v) {
    uint32_t al = a & ~3u, w = mem_r32(al);
    switch (a & 3) {
    case 0: w = v; break;
    case 1: w = (w & 0x000000FF) | (v << 8); break;
    case 2: w = (w & 0x0000FFFF) | (v << 16); break;
    default: w = (w & 0x00FFFFFF) | (v << 24); break;
    }
    mem_w32(al, w);
}

/*
 * MChessProUCI - 32-bit UCI Chess Engine
 * Wraps the unpacked 16-bit MChess Pro v3.5 engine + embedded opening book
 * inside a self-contained 80186 real-mode emulator with extended-memory
 * hash table traps and full UCI protocol support.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <stdarg.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#include <pthread.h>
#include <sys/select.h>
#endif

#include "mcpro_data.h"
#include "mcpro_eval.h"

/* =========================================================================
 * Memory & CPU State
 * ========================================================================= */

#define MEM_SIZE        0x100000u   /* 1 MB conventional RAM */
#define EXT_MEM_SIZE    0x1000000u  /* 16 MB extended RAM (for hash tables) */
#define PSP_SEG         0x0FF0u
#define ENV_SEG         0x0FE0u
#define LOAD_SEG        0x1000u
#define DGROUP_SEG      (LOAD_SEG + 0x51BFu)
#define TRAP_SEG        0xF000u     /* Synthetic far-call return trap */

static uint8_t  mem[MEM_SIZE];
static uint8_t  ext_mem[EXT_MEM_SIZE];

typedef struct {
    uint16_t ax, cx, dx, bx, sp, bp, si, di;
    uint16_t es, cs, ss, ds;
    uint16_t ip;
    uint16_t flags;
    int halted;
    int exit_code;
    int stop_on_idle;
    int idle_hit;
} CPU;

static CPU cpu;

/* Segment registers index: 0=ES, 1=CS, 2=SS, 3=DS */
static inline uint16_t *seg_ptr(int s) {
    switch (s & 3) {
        case 0: return &cpu.es;
        case 1: return &cpu.cs;
        case 2: return &cpu.ss;
        default: return &cpu.ds;
    }
}

/* 16-bit register index: 0=AX,1=CX,2=DX,3=BX,4=SP,5=BP,6=SI,7=DI */
static inline uint16_t *reg16_ptr(int r) {
    switch (r & 7) {
        case 0: return &cpu.ax;
        case 1: return &cpu.cx;
        case 2: return &cpu.dx;
        case 3: return &cpu.bx;
        case 4: return &cpu.sp;
        case 5: return &cpu.bp;
        case 6: return &cpu.si;
        default: return &cpu.di;
    }
}

static inline uint8_t get_reg8(int r) {
    switch (r & 7) {
        case 0: return (uint8_t)(cpu.ax & 0xFF);
        case 1: return (uint8_t)(cpu.cx & 0xFF);
        case 2: return (uint8_t)(cpu.dx & 0xFF);
        case 3: return (uint8_t)(cpu.bx & 0xFF);
        case 4: return (uint8_t)(cpu.ax >> 8);
        case 5: return (uint8_t)(cpu.cx >> 8);
        case 6: return (uint8_t)(cpu.dx >> 8);
        default: return (uint8_t)(cpu.bx >> 8);
    }
}

static inline void set_reg8(int r, uint8_t v) {
    switch (r & 7) {
        case 0: cpu.ax = (cpu.ax & 0xFF00) | v; break;
        case 1: cpu.cx = (cpu.cx & 0xFF00) | v; break;
        case 2: cpu.dx = (cpu.dx & 0xFF00) | v; break;
        case 3: cpu.bx = (cpu.bx & 0xFF00) | v; break;
        case 4: cpu.ax = (cpu.ax & 0x00FF) | ((uint16_t)v << 8); break;
        case 5: cpu.cx = (cpu.cx & 0x00FF) | ((uint16_t)v << 8); break;
        case 6: cpu.dx = (cpu.dx & 0x00FF) | ((uint16_t)v << 8); break;
        default: cpu.bx = (cpu.bx & 0x00FF) | ((uint16_t)v << 8); break;
    }
}

static inline uint32_t phys(uint16_t seg, uint16_t off) {
    return (((uint32_t)seg << 4) + off) & 0xFFFFFu;
}

static inline uint8_t rb(uint32_t addr) {
    return mem[addr & 0xFFFFFu];
}

static inline uint16_t rw(uint32_t addr) {
    uint32_t a = addr & 0xFFFFFu;
    return (uint16_t)mem[a] | ((uint16_t)mem[(a + 1) & 0xFFFFFu] << 8);
}

static inline void wb(uint32_t addr, uint8_t v) {
    mem[addr & 0xFFFFFu] = v;
}

static inline void ww(uint32_t addr, uint16_t v) {
    uint32_t a = addr & 0xFFFFFu;
    mem[a] = (uint8_t)(v & 0xFF);
    mem[(a + 1) & 0xFFFFFu] = (uint8_t)(v >> 8);
}

static inline uint8_t ds_rb(uint16_t off) { return rb(phys(DGROUP_SEG, off)); }
static inline uint16_t ds_rw(uint16_t off) { return rw(phys(DGROUP_SEG, off)); }
static inline void ds_wb(uint16_t off, uint8_t v) { wb(phys(DGROUP_SEG, off), v); }
static inline void ds_ww(uint16_t off, uint16_t v) { ww(phys(DGROUP_SEG, off), v); }

static inline void push16(uint16_t v) {
    cpu.sp -= 2;
    ww(phys(cpu.ss, cpu.sp), v);
}

static inline uint16_t pop16(void) {
    uint16_t v = rw(phys(cpu.ss, cpu.sp));
    cpu.sp += 2;
    return v;
}

/* Flags */
#define F_CF 0x0001u
#define F_PF 0x0004u
#define F_AF 0x0010u
#define F_ZF 0x0040u
#define F_SF 0x0080u
#define F_TF 0x0100u
#define F_IF 0x0200u
#define F_DF 0x0400u
#define F_OF 0x0800u

static uint8_t parity_table[256];
static void init_parity(void) {
    for (int i = 0; i < 256; i++) {
        int p = 0;
        for (int b = 0; b < 8; b++) if (i & (1 << b)) p ^= 1;
        parity_table[i] = p ? 0 : F_PF;
    }
}

static inline void set_szp8(uint8_t r) {
    cpu.flags = (cpu.flags & ~(F_SF | F_ZF | F_PF)) |
                (r == 0 ? F_ZF : 0) |
                ((r & 0x80) ? F_SF : 0) |
                parity_table[r];
}

static inline void set_szp16(uint16_t r) {
    cpu.flags = (cpu.flags & ~(F_SF | F_ZF | F_PF)) |
                (r == 0 ? F_ZF : 0) |
                ((r & 0x8000) ? F_SF : 0) |
                parity_table[r & 0xFF];
}

static inline void set_cf_of(int cf, int of) {
    cpu.flags = (cpu.flags & ~(F_CF | F_OF)) | (cf ? F_CF : 0) | (of ? F_OF : 0);
}

/* =========================================================================
 * Timing & Input Polling
 * ========================================================================= */

static uint64_t get_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
#endif
}

static uint64_t boot_ms = 0;
static uint64_t search_start_ms = 0;
static uint64_t search_deadline_ms = 0;
static int search_active = 0;
static uint16_t search_initial_ply = 0;
static volatile int stop_requested = 0;
static volatile int ponder_hit = 0;

static void update_bda_ticks(void) {
    uint64_t elapsed = get_ms() - boot_ms;
    uint32_t ticks = (uint32_t)((elapsed * 182065ULL) / 10000000ULL);
    ww(0x046Cu, (uint16_t)(ticks & 0xFFFF));
    ww(0x046Eu, (uint16_t)(ticks >> 16));
}

/* =========================================================================
 * Virtual File System & DOS/BIOS Interrupts
 * ========================================================================= */

#define MAX_VFILES 32
typedef struct {
    int in_use;
    const uint8_t *data;
    uint32_t size;
    uint32_t pos;
    int writable;
} VFile;

static VFile vfiles[MAX_VFILES];
static uint16_t dta_seg = PSP_SEG;
static uint16_t dta_off = 0x0080;

/* Simple DOS MCB heap */
#define HEAP_START_SEG 0x7200u
#define HEAP_END_SEG   0x9FF0u
typedef struct {
    uint16_t seg;
    uint16_t paras;
    int used;
} MemBlock;
static MemBlock mblocks[64];
static int mblock_count = 0;

static void init_dos_heap(void) {
    mblock_count = 1;
    mblocks[0].seg = HEAP_START_SEG;
    mblocks[0].paras = HEAP_END_SEG - HEAP_START_SEG;
    mblocks[0].used = 0;
}

static uint16_t dos_alloc(uint16_t paras, uint16_t *max_avail) {
    uint16_t best_free = 0;
    for (int i = 0; i < mblock_count; i++) {
        if (!mblocks[i].used) {
            if (mblocks[i].paras > best_free) best_free = mblocks[i].paras;
            if (mblocks[i].paras >= paras) {
                uint16_t seg = mblocks[i].seg;
                if (mblocks[i].paras > paras && mblock_count < 64) {
                    for (int j = mblock_count; j > i + 1; j--) mblocks[j] = mblocks[j - 1];
                    mblocks[i + 1].seg = seg + paras;
                    mblocks[i + 1].paras = mblocks[i].paras - paras;
                    mblocks[i + 1].used = 0;
                    mblocks[i].paras = paras;
                    mblock_count++;
                }
                mblocks[i].used = 1;
                return seg;
            }
        }
    }
    if (max_avail) *max_avail = best_free;
    return 0;
}

static int dos_free(uint16_t seg) {
    for (int i = 0; i < mblock_count; i++) {
        if (mblocks[i].seg == seg && mblocks[i].used) {
            mblocks[i].used = 0;
            /* Coalesce */
            for (int j = 0; j + 1 < mblock_count; j++) {
                if (!mblocks[j].used && !mblocks[j + 1].used) {
                    mblocks[j].paras += mblocks[j + 1].paras;
                    for (int k = j + 1; k + 1 < mblock_count; k++) mblocks[k] = mblocks[k + 1];
                    mblock_count--;
                    j--;
                }
            }
            return 1;
        }
    }
    return 0;
}

static int dos_resize(uint16_t seg, uint16_t new_paras, uint16_t *max_avail) {
    if (seg == PSP_SEG) {
        /* Main program PSP resize: always succeed if <= HEAP_START_SEG - PSP_SEG */
        if (new_paras <= HEAP_START_SEG - PSP_SEG) return 1;
        if (max_avail) *max_avail = HEAP_START_SEG - PSP_SEG;
        return 0;
    }
    for (int i = 0; i < mblock_count; i++) {
        if (mblocks[i].seg == seg) {
            if (new_paras <= mblocks[i].paras) {
                if (new_paras < mblocks[i].paras && mblock_count < 64) {
                    for (int j = mblock_count; j > i + 1; j--) mblocks[j] = mblocks[j - 1];
                    mblocks[i + 1].seg = seg + new_paras;
                    mblocks[i + 1].paras = mblocks[i].paras - new_paras;
                    mblocks[i + 1].used = 0;
                    mblocks[i].paras = new_paras;
                    mblock_count++;
                }
                return 1;
            }
            if (i + 1 < mblock_count && !mblocks[i + 1].used &&
                mblocks[i].paras + mblocks[i + 1].paras >= new_paras) {
                uint16_t total = mblocks[i].paras + mblocks[i + 1].paras;
                mblocks[i].paras = new_paras;
                mblocks[i + 1].seg = seg + new_paras;
                mblocks[i + 1].paras = total - new_paras;
                if (mblocks[i + 1].paras == 0) {
                    for (int k = i + 1; k + 1 < mblock_count; k++) mblocks[k] = mblocks[k + 1];
                    mblock_count--;
                }
                return 1;
            }
            if (max_avail) *max_avail = mblocks[i].paras;
            return 0;
        }
    }
    if (max_avail) *max_avail = 0;
    return 0;
}

static const char *str_basename_ci(const char *path) {
    const char *p = path;
    for (const char *s = path; *s; s++) {
        if (*s == '\\' || *s == '/') p = s + 1;
    }
    return p;
}

static int ci_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int ci_starts_with(const char *s, const char *prefix) {
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return 0;
        s++; prefix++;
    }
    return 1;
}

static int vfs_open(const char *path) {
    const char *base = str_basename_ci(path);
    const uint8_t *data = NULL;
    uint32_t size = 0;

    static const uint8_t lang_english[] = "ENGLISH";
    static const uint8_t ubooks_empty[] = "   0  1\r\n";

    if (ci_eq(base, "PRBOOK.M35")) {
        data = mcpro_prbook;
        size = sizeof(mcpro_prbook);
    } else if (ci_starts_with(base, "M35KEY.") && strlen(base) == 10) {
        char *end = NULL;
        unsigned long idx = strtoul(base + 7, &end, 16);
        if (end && *end == '\0' && idx < 16) {
            data = mcpro_m35keys[idx];
            size = mcpro_m35key_sizes[idx];
        }
    } else if (ci_eq(base, "EGADTA.M35")) {
        data = mcpro_egadta_m35;
        size = sizeof(mcpro_egadta_m35);
    } else if (ci_eq(base, "ENGLISH.T35")) {
        data = mcpro_english_t35;
        size = sizeof(mcpro_english_t35);
    } else if (ci_eq(base, "LANGUAGE.M35")) {
        data = lang_english;
        size = sizeof(lang_english) - 1;
    } else if (ci_eq(base, "UBOOKS.M35")) {
        data = ubooks_empty;
        size = sizeof(ubooks_empty) - 1;
    }

    if (!data) return -1;

    for (int h = 5; h < MAX_VFILES; h++) {
        if (!vfiles[h].in_use) {
            vfiles[h].in_use = 1;
            vfiles[h].data = data;
            vfiles[h].size = size;
            vfiles[h].pos = 0;
            vfiles[h].writable = 0;
            return h;
        }
    }
    return -1;
}

static void read_asciiz(uint16_t seg, uint16_t off, char *buf, int maxlen) {
    int i = 0;
    while (i < maxlen - 1) {
        uint8_t c = rb(phys(seg, off + i));
        if (c == 0) break;
        buf[i++] = (char)c;
    }
    buf[i] = '\0';
}

static void handle_int(uint8_t vec) {
    if (vec == 0x10) {
        /* Video BIOS: stub cleanly */
        uint8_t ah = cpu.ax >> 8;
        if (ah == 0x00) {
            mem[0x0449] = cpu.ax & 0x7F;
        } else if (ah == 0x0E) {
            /* suppress teletype output */
        } else if (ah == 0x0F) {
            cpu.ax = (80 << 8) | mem[0x0449];
            cpu.bx = 0;
        } else if (ah == 0x1A) {
            cpu.ax = (cpu.ax & 0xFF00) | 0x1A;
            cpu.bx = 0x0008; /* VGA color */
        } else if (ah == 0x1B) {
            cpu.ax = (cpu.ax & 0xFF00) | 0x1B;
            uint32_t buf = phys(cpu.es, cpu.di);
            memset(&mem[buf], 0, 64);
            ww(buf + 0, 0x0000); ww(buf + 2, 0xE000);
            mem[buf + 4] = mem[0x0449];
            ww(buf + 5, 80);
            mem[buf + 0x22] = 25;
            ww(buf + 0x27, 16);
        } else if (ah == 0x12) {
            if ((cpu.bx & 0xFF) == 0x10) {
                cpu.bx = 0x0003; /* Color, 256K EGA/VGA */
                cpu.cx = 0x0009; /* VGA switch settings */
            }
        } else if (ah == 0x11 && (cpu.ax & 0xFF) == 0x30) {
            cpu.cx = 16;
            cpu.dx = (cpu.dx & 0xFF00) | 24;
        }
        return;
    }
    if (vec == 0x11) {
        cpu.ax = 0x0021; /* Equipment list */
        return;
    }
    if (vec == 0x12) {
        cpu.ax = 640;
        return;
    }
    if (vec == 0x15) {
        uint8_t ah = cpu.ax >> 8;
        if (ah == 0x88) {
            cpu.ax = 15360; /* 15 MB extended memory */
            cpu.flags &= ~F_CF;
            return;
        }
        cpu.flags |= F_CF;
        return;
    }
    if (vec == 0x16) {
        uint8_t ah = cpu.ax >> 8;
        if (ah == 0x01 || ah == 0x11) {
            cpu.flags |= F_ZF; /* No key available */
        } else if (ah == 0x00 || ah == 0x10) {
            cpu.ax = 0x1C0D; /* Enter */
        } else if (ah == 0x02 || ah == 0x12) {
            cpu.ax = 0;
        }
        return;
    }
    if (vec == 0x1A) {
        uint8_t ah = cpu.ax >> 8;
        if (ah == 0x00) {
            update_bda_ticks();
            cpu.dx = rw(0x046Cu);
            cpu.cx = rw(0x046Eu);
            cpu.ax &= 0xFF00;
            return;
        }
        cpu.flags &= ~F_CF;
        return;
    }
    if (vec == 0x2F) {
        /* XMS / DPMI / Multiplex: report not installed */
        if ((cpu.ax >> 8) == 0x43) {
            cpu.ax &= 0xFF00;
        }
        return;
    }
    if (vec == 0x33) {
        /* Mouse driver: not installed */
        if (cpu.ax == 0x0000) {
            cpu.ax = 0;
            cpu.bx = 0;
        } else if (cpu.ax == 0x0003) {
            cpu.bx = 0; cpu.cx = 0; cpu.dx = 0;
        }
        return;
    }
    if (vec == 0x21) {
        uint8_t ah = cpu.ax >> 8;
        cpu.flags &= ~F_CF;
        switch (ah) {
            case 0x02:
            case 0x06:
                if (ah == 0x06 && (cpu.dx & 0xFF) == 0xFF) {
                    cpu.flags |= F_ZF;
                }
                break;
            case 0x09:
                break;
            case 0x0B:
                cpu.ax = (cpu.ax & 0xFF00) | 0x00;
                break;
            case 0x19:
                cpu.ax = (cpu.ax & 0xFF00) | 0x02; /* C: */
                break;
            case 0x1A:
                dta_seg = cpu.ds;
                dta_off = cpu.dx;
                break;
            case 0x25: {
                uint8_t v = cpu.ax & 0xFF;
                ww((uint32_t)v * 4, cpu.dx);
                ww((uint32_t)v * 4 + 2, cpu.ds);
                break;
            }
            case 0x2A:
                cpu.cx = 1994; cpu.dx = 0x0101; cpu.ax = (cpu.ax & 0xFF00) | 6;
                break;
            case 0x2C:
                cpu.cx = 0x0C00; cpu.dx = 0x0000;
                break;
            case 0x30:
                cpu.ax = 0x0005; /* DOS 5.0 */
                cpu.bx = 0xFF00;
                cpu.cx = 0;
                break;
            case 0x33:
                cpu.dx = 0;
                break;
            case 0x35: {
                uint8_t v = cpu.ax & 0xFF;
                cpu.bx = rw((uint32_t)v * 4);
                cpu.es = rw((uint32_t)v * 4 + 2);
                break;
            }
            case 0x3C:
            case 0x3D: {
                char path[260];
                read_asciiz(cpu.ds, cpu.dx, path, sizeof(path));
                int h = (ah == 0x3D) ? vfs_open(path) : -1;
                if (h < 0) {
                    cpu.flags |= F_CF;
                    cpu.ax = 2; /* File not found */
                } else {
                    cpu.ax = (uint16_t)h;
                }
                break;
            }
            case 0x3E: {
                uint16_t h = cpu.bx;
                if (h >= 5 && h < MAX_VFILES && vfiles[h].in_use) {
                    vfiles[h].in_use = 0;
                }
                break;
            }
            case 0x3F: {
                uint16_t h = cpu.bx;
                uint16_t cnt = cpu.cx;
                if (h >= 5 && h < MAX_VFILES && vfiles[h].in_use) {
                    uint32_t rem = (vfiles[h].pos < vfiles[h].size) ? (vfiles[h].size - vfiles[h].pos) : 0;
                    uint32_t n = (cnt < rem) ? cnt : rem;
                    for (uint32_t i = 0; i < n; i++) {
                        wb(phys(cpu.ds, cpu.dx + i), vfiles[h].data[vfiles[h].pos + i]);
                    }
                    vfiles[h].pos += n;
                    cpu.ax = (uint16_t)n;
                } else {
                    cpu.ax = 0;
                }
                break;
            }
            case 0x40:
                /* Write to stdout/stderr/file: pretend succeeded */
                cpu.ax = cpu.cx;
                break;
            case 0x42: {
                uint16_t h = cpu.bx;
                int32_t off = (int32_t)(((uint32_t)cpu.cx << 16) | cpu.dx);
                uint8_t method = cpu.ax & 0xFF;
                if (h >= 5 && h < MAX_VFILES && vfiles[h].in_use) {
                    int64_t np = 0;
                    if (method == 0) np = off;
                    else if (method == 1) np = (int64_t)vfiles[h].pos + off;
                    else if (method == 2) np = (int64_t)vfiles[h].size + off;
                    if (np < 0) np = 0;
                    vfiles[h].pos = (uint32_t)np;
                    cpu.dx = (uint16_t)(vfiles[h].pos >> 16);
                    cpu.ax = (uint16_t)(vfiles[h].pos & 0xFFFF);
                } else {
                    cpu.flags |= F_CF;
                    cpu.ax = 6;
                }
                break;
            }
            case 0x44: {
                uint8_t al = cpu.ax & 0xFF;
                if (al == 0x00) {
                    cpu.dx = (cpu.bx <= 4) ? 0x80D3 : 0x0002;
                } else if (al == 0x08) {
                    cpu.ax = 1;
                }
                break;
            }
            case 0x47: {
                const char *dir = "MCPRO";
                for (int i = 0; i <= 5; i++) wb(phys(cpu.ds, cpu.si + i), (uint8_t)dir[i]);
                cpu.ax = 0x0100;
                break;
            }
            case 0x48: {
                uint16_t max_av = 0;
                uint16_t seg = dos_alloc(cpu.bx, &max_av);
                if (seg == 0) {
                    cpu.flags |= F_CF;
                    cpu.ax = 8;
                    cpu.bx = max_av;
                } else {
                    cpu.ax = seg;
                }
                break;
            }
            case 0x49:
                if (!dos_free(cpu.es)) {
                    cpu.flags |= F_CF;
                    cpu.ax = 9;
                }
                break;
            case 0x4A: {
                uint16_t max_av = 0;
                if (!dos_resize(cpu.es, cpu.bx, &max_av)) {
                    cpu.flags |= F_CF;
                    cpu.ax = 8;
                    cpu.bx = max_av;
                }
                break;
            }
            case 0x4C:
                fprintf(stderr, "DOS exit(%u) at %04x:%04x stack=%04x %04x %04x %04x\n",
                        cpu.ax & 0xFF, cpu.cs - LOAD_SEG, cpu.ip,
                        rw(phys(cpu.ss, cpu.sp)), rw(phys(cpu.ss, cpu.sp + 2)),
                        rw(phys(cpu.ss, cpu.sp + 4)), rw(phys(cpu.ss, cpu.sp + 6)));
                cpu.halted = 1;
                cpu.exit_code = cpu.ax & 0xFF;
                break;
            case 0x4E:
            case 0x4F:
                cpu.flags |= F_CF;
                cpu.ax = 18;
                break;
            case 0x51:
            case 0x62:
                cpu.bx = PSP_SEG;
                break;
            default:
                break;
        }
        return;
    }
}

/* =========================================================================
 * MChessPro Engine Hooks (Unreal-Mode + Search PV + Timer/Abort)
 * ========================================================================= */

static int pv_hook_enabled = 0;

/* UCI Engine Options */
static int opt_hash_mb = 10;
static int opt_own_book = 1;
static int opt_ponder = 0;
static int opt_playing_style = 1; /* 0=Cautious, 1=Normal, 2=Aggressive */
static int opt_random_play = 0;   /* 0=Optimal, 1=SlightlyRandom, 3=Randomized */
static int opt_easier_play = 0;
static int opt_passive_moves = 0;
static int opt_preset_level = 2;

static void apply_engine_options(void) {
    ds_wb(0x4cc4, opt_own_book ? 0 : 1);
    ds_wb(0xda5d, opt_ponder ? 1 : 0);
    ds_wb(0x1775, (uint8_t)opt_playing_style);
    ds_ww(0x4cd2, (uint16_t)opt_random_play);
    ds_wb(0x2c96, opt_easier_play ? 1 : 0);
    ds_wb(0xe1c9, opt_passive_moves ? 1 : 0);
    ds_wb(0xde80, (uint8_t)opt_preset_level);
    /* Enable Show Analysis bit (0x02) on 0xdc6a so 0x2f95:0xce3a fires for every PV */
    ds_wb(0xdc6a, ds_rb(0xdc6a) | 0x02);
}

/* Convert 10x12 square (21..98) to UCI string */
static void sq_to_str(uint8_t sq, char *out) {
    int f = (sq % 10) - 1;
    int r = (sq / 10) - 2;
    if (f >= 0 && f < 8 && r >= 0 && r < 8) {
        out[0] = (char)('a' + f);
        out[1] = (char)('1' + r);
    } else {
        out[0] = '?';
        out[1] = '?';
    }
}

static void move3_to_uci(const uint8_t *m, char *out) {
    sq_to_str(m[0] & 0x7F, out);
    sq_to_str(m[1] & 0x7F, out + 2);
    uint8_t fl = m[2];
    /* If promotion flag or promoted piece differs from pawn, emit promotion char.
     * Note: in 0xce4c/0xd9ea, fl is 2..5 (N/B/R/Q) for promotions when encoded as flag,
     * or 9..12 for black promoted piece if stored as piece code. Handle both! */
    if (fl >= 2 && fl <= 5) {
        /* Check if source square had a pawn (or just check if it's a 1st/8th rank move) */
        int to_r = ((m[1] & 0x7F) / 10) - 2;
        int from_r = ((m[0] & 0x7F) / 10) - 2;
        if ((to_r == 7 && from_r == 6) || (to_r == 0 && from_r == 1)) {
            const char *prom = "  nbrq";
            out[4] = prom[fl];
            out[5] = '\0';
            return;
        }
    } else if (fl >= 9 && fl <= 12) {
        int to_r = ((m[1] & 0x7F) / 10) - 2;
        int from_r = ((m[0] & 0x7F) / 10) - 2;
        if (to_r == 0 && from_r == 1) {
            const char *prom = "nbrq";
            out[4] = prom[fl - 9];
            out[5] = '\0';
            return;
        }
    }
    out[4] = '\0';
}

static void emit_uci_pv(int16_t score) {
    if (!pv_hook_enabled) return;
    if (score >= 20000 || score <= -20000) return;

    uint32_t nodes = ds_rw(0x8fe8) | ((uint32_t)ds_rw(0x8fea) << 16);
    int depth = ds_rb(0xda5a);
    if (depth <= 0) depth = 1;
    uint64_t elapsed = get_ms() - search_start_ms;
    if (elapsed == 0) elapsed = 1;
    uint64_t nps = ((uint64_t)nodes * 1000ULL) / elapsed;

    char score_str[32];
    if (score > 19950) {
        int mate_moves = (20001 - score) / 2;
        if (mate_moves < 1) mate_moves = 1;
        snprintf(score_str, sizeof(score_str), "mate %d", mate_moves);
    } else if (score < -19950) {
        int mate_moves = (20001 + score) / 2;
        if (mate_moves < 1) mate_moves = 1;
        snprintf(score_str, sizeof(score_str), "mate -%d", mate_moves);
    } else {
        snprintf(score_str, sizeof(score_str), "cp %d", (int)(score / 2));
    }

    int pv_len = ds_rb(0xe6a9);
    if (pv_len < 0) pv_len = 0;
    if (pv_len > 32) pv_len = 32;

    printf("info depth %d score %s nodes %u nps %llu time %llu",
           depth, score_str, nodes, (unsigned long long)nps, (unsigned long long)elapsed);
    if (pv_len > 0) {
        printf(" pv");
        for (int i = 0; i < pv_len; i++) {
            uint8_t m[3] = {
                (uint8_t)(ds_rb((uint16_t)(0xce8c + i * 2)) & 0x7F),
                (uint8_t)(ds_rb((uint16_t)(0xce8c + i * 2 + 1)) & 0x7F),
                0
            };
            if (m[0] == 0 || m[1] == 0) break;
            char uci[8];
            move3_to_uci(m, uci);
            printf(" %s", uci);
        }
    }
    printf("\n");
    fflush(stdout);
}

static void run_until_trap(uint64_t max_steps);

static int check_far_traps(uint16_t target_seg, uint16_t target_off) {
    /* Return 1 if handled (so caller does not jump into target) */
    uint16_t rel_seg = target_seg - LOAD_SEG;

    /* Unreal-Mode extended memory helpers in seg 0x0000 */
    if (rel_seg == 0x0000) {
        if (target_off == 0x9226) {
            /* enable_unreal_mode */
            cpu.ax = 1;
            return 1;
        }
        if (target_off == 0x9247) {
            /* uint16_t ext_read8(uint32_t phys) */
            uint32_t p = rw(phys(cpu.ss, cpu.sp)) | ((uint32_t)rw(phys(cpu.ss, cpu.sp + 2)) << 16);
            cpu.ax = (p < EXT_MEM_SIZE) ? ext_mem[p] : 0;
            return 1;
        }
        if (target_off == 0x925a) {
            /* void ext_write8(uint32_t phys, uint16_t val) */
            uint32_t p = rw(phys(cpu.ss, cpu.sp)) | ((uint32_t)rw(phys(cpu.ss, cpu.sp + 2)) << 16);
            uint8_t v = (uint8_t)(rw(phys(cpu.ss, cpu.sp + 4)) & 0xFF);
            if (p < EXT_MEM_SIZE) ext_mem[p] = v;
            return 1;
        }
        if (target_off == 0x926e) {
            /* void ext_memcpy(uint32_t src, uint32_t dst, uint32_t len) */
            uint32_t src = rw(phys(cpu.ss, cpu.sp)) | ((uint32_t)rw(phys(cpu.ss, cpu.sp + 2)) << 16);
            uint32_t dst = rw(phys(cpu.ss, cpu.sp + 4)) | ((uint32_t)rw(phys(cpu.ss, cpu.sp + 6)) << 16);
            uint32_t len = rw(phys(cpu.ss, cpu.sp + 8)) | ((uint32_t)rw(phys(cpu.ss, cpu.sp + 10)) << 16);
            for (uint32_t i = 0; i < len; i++) {
                uint8_t b = (src + i < MEM_SIZE) ? mem[src + i] :
                            (src + i < EXT_MEM_SIZE) ? ext_mem[src + i] : 0;
                if (dst + i < MEM_SIZE) mem[dst + i] = b;
                else if (dst + i < EXT_MEM_SIZE) ext_mem[dst + i] = b;
            }
            return 1;
        }
    }

    /* Native C evaluation traps in seg 0x0fcb */
    if (rel_seg == 0x0fcb) {
        if (target_off == 0xd048) {
            cpu.ax = mcpro_eval_black_pieces(&mem[DGROUP_SEG << 4]);
            return 1;
        }
    }

    /* PV / screen output hooks in seg 0x2f95 */
    if (rel_seg == 0x2f95) {
        if (target_off == 0x65be) {
            if (cpu.stop_on_idle) {
                cpu.idle_hit = 1;
                cpu.halted = 1;
                return 1;
            }
        }
        if (target_off == 0xca20) {
            cpu.sp += 10; /* lret $0xa */
            return 1;
        }
        if (target_off == 0xce3a) {
            int16_t score = (int16_t)rw(phys(cpu.ss, cpu.sp));
            emit_uci_pv(score);
            return 1;
        }
    }
    return 0;
}

/* =========================================================================
 * 80186 CPU Interpreter
 * ========================================================================= */

static uint8_t fetch8(void) {
    uint8_t v = rb(phys(cpu.cs, cpu.ip));
    cpu.ip++;
    return v;
}

static uint16_t fetch16(void) {
    uint16_t v = rw(phys(cpu.cs, cpu.ip));
    cpu.ip += 2;
    return v;
}

static uint16_t eff_seg(int seg_ovr, uint16_t def_seg) {
    return (seg_ovr >= 0) ? *seg_ptr(seg_ovr) : def_seg;
}

static void decode_modrm(uint8_t modrm, int seg_ovr, uint16_t *out_seg, uint16_t *out_off, int *is_reg) {
    uint8_t mod = modrm >> 6;
    uint8_t rm = modrm & 7;
    if (mod == 3) {
        *is_reg = 1;
        *out_off = rm;
        return;
    }
    *is_reg = 0;
    if (mod == 0 && rm == 6) {
        *out_seg = eff_seg(seg_ovr, cpu.ds);
        *out_off = fetch16();
        return;
    }
    uint16_t def = cpu.ds;
    uint16_t ea = 0;
    switch (rm) {
        case 0: ea = cpu.bx + cpu.si; break;
        case 1: ea = cpu.bx + cpu.di; break;
        case 2: ea = cpu.bp + cpu.si; def = cpu.ss; break;
        case 3: ea = cpu.bp + cpu.di; def = cpu.ss; break;
        case 4: ea = cpu.si; break;
        case 5: ea = cpu.di; break;
        case 6: ea = cpu.bp; def = cpu.ss; break;
        case 7: ea = cpu.bx; break;
    }
    if (mod == 1) {
        int8_t d = (int8_t)fetch8();
        ea = (uint16_t)(ea + d);
    } else if (mod == 2) {
        uint16_t d = fetch16();
        ea = (uint16_t)(ea + d);
    }
    *out_seg = eff_seg(seg_ovr, def);
    *out_off = ea;
}

static uint8_t read_rm8(uint8_t modrm, int seg_ovr, uint16_t *s_out, uint16_t *o_out, int *is_reg) {
    decode_modrm(modrm, seg_ovr, s_out, o_out, is_reg);
    if (*is_reg) return get_reg8(*o_out);
    return rb(phys(*s_out, *o_out));
}

static void write_rm8(int is_reg, uint16_t s, uint16_t o, uint8_t v) {
    if (is_reg) set_reg8(o, v);
    else wb(phys(s, o), v);
}

static uint16_t read_rm16(uint8_t modrm, int seg_ovr, uint16_t *s_out, uint16_t *o_out, int *is_reg) {
    decode_modrm(modrm, seg_ovr, s_out, o_out, is_reg);
    if (*is_reg) return *reg16_ptr(*o_out);
    return rw(phys(*s_out, *o_out));
}

static void write_rm16(int is_reg, uint16_t s, uint16_t o, uint16_t v) {
    if (is_reg) *reg16_ptr(o) = v;
    else ww(phys(s, o), v);
}

static uint8_t alu8(int op, uint8_t a, uint8_t b, int *writeback) {
    uint16_t res = 0;
    int cf = 0, of = 0, af = 0;
    *writeback = 1;
    switch (op & 7) {
        case 0: /* ADD */
            res = (uint16_t)a + b;
            cf = res > 0xFF;
            of = ((~(a ^ b)) & (a ^ res) & 0x80) != 0;
            af = ((a ^ b ^ res) & 0x10) != 0;
            break;
        case 1: /* OR */
            res = a | b;
            break;
        case 2: /* ADC */ {
            uint8_t c = (cpu.flags & F_CF) ? 1 : 0;
            res = (uint16_t)a + b + c;
            cf = res > 0xFF;
            of = ((~(a ^ b)) & (a ^ res) & 0x80) != 0;
            af = ((a ^ b ^ res) & 0x10) != 0;
            break;
        }
        case 3: /* SBB */ {
            uint8_t c = (cpu.flags & F_CF) ? 1 : 0;
            res = (uint16_t)a - b - c;
            cf = (uint16_t)a < (uint16_t)b + c;
            of = (((a ^ b)) & (a ^ res) & 0x80) != 0;
            af = ((a ^ b ^ res) & 0x10) != 0;
            break;
        }
        case 4: /* AND */
            res = a & b;
            break;
        case 5: /* SUB */
            res = (uint16_t)a - b;
            cf = a < b;
            of = (((a ^ b)) & (a ^ res) & 0x80) != 0;
            af = ((a ^ b ^ res) & 0x10) != 0;
            break;
        case 6: /* XOR */
            res = a ^ b;
            break;
        case 7: /* CMP */
            *writeback = 0;
            res = (uint16_t)a - b;
            cf = a < b;
            of = (((a ^ b)) & (a ^ res) & 0x80) != 0;
            af = ((a ^ b ^ res) & 0x10) != 0;
            break;
    }
    set_szp8((uint8_t)res);
    set_cf_of(cf, of);
    if (af) cpu.flags |= F_AF; else cpu.flags &= ~F_AF;
    return (uint8_t)res;
}

static uint16_t alu16(int op, uint16_t a, uint16_t b, int *writeback) {
    uint32_t res = 0;
    int cf = 0, of = 0, af = 0;
    *writeback = 1;
    switch (op & 7) {
        case 0: /* ADD */
            res = (uint32_t)a + b;
            cf = res > 0xFFFF;
            of = ((~(a ^ b)) & (a ^ res) & 0x8000) != 0;
            af = ((a ^ b ^ res) & 0x10) != 0;
            break;
        case 1: /* OR */
            res = a | b;
            break;
        case 2: /* ADC */ {
            uint16_t c = (cpu.flags & F_CF) ? 1 : 0;
            res = (uint32_t)a + b + c;
            cf = res > 0xFFFF;
            of = ((~(a ^ b)) & (a ^ res) & 0x8000) != 0;
            af = ((a ^ b ^ res) & 0x10) != 0;
            break;
        }
        case 3: /* SBB */ {
            uint16_t c = (cpu.flags & F_CF) ? 1 : 0;
            res = (uint32_t)a - b - c;
            cf = (uint32_t)a < (uint32_t)b + c;
            of = (((a ^ b)) & (a ^ res) & 0x8000) != 0;
            af = ((a ^ b ^ res) & 0x10) != 0;
            break;
        }
        case 4: /* AND */
            res = a & b;
            break;
        case 5: /* SUB */
            res = (uint32_t)a - b;
            cf = a < b;
            of = (((a ^ b)) & (a ^ res) & 0x8000) != 0;
            af = ((a ^ b ^ res) & 0x10) != 0;
            break;
        case 6: /* XOR */
            res = a ^ b;
            break;
        case 7: /* CMP */
            *writeback = 0;
            res = (uint32_t)a - b;
            cf = a < b;
            of = (((a ^ b)) & (a ^ res) & 0x8000) != 0;
            af = ((a ^ b ^ res) & 0x10) != 0;
            break;
    }
    set_szp16((uint16_t)res);
    set_cf_of(cf, of);
    if (af) cpu.flags |= F_AF; else cpu.flags &= ~F_AF;
    return (uint16_t)res;
}

static uint8_t shift8(int op, uint8_t v, uint8_t cnt) {
    cnt &= 0x1F;
    if (cnt == 0) return v;
    int cf = (cpu.flags & F_CF) ? 1 : 0;
    int of = (cpu.flags & F_OF) ? 1 : 0;
    uint8_t r = v;
    for (uint8_t i = 0; i < cnt; i++) {
        switch (op & 7) {
            case 0: /* ROL */
                cf = (r >> 7) & 1;
                r = (uint8_t)((r << 1) | cf);
                of = ((r >> 7) ^ cf) & 1;
                break;
            case 1: /* ROR */
                cf = r & 1;
                r = (uint8_t)((r >> 1) | (cf << 7));
                of = ((r >> 7) ^ ((r >> 6) & 1)) & 1;
                break;
            case 2: /* RCL */ {
                int nc = (r >> 7) & 1;
                r = (uint8_t)((r << 1) | cf);
                cf = nc;
                of = ((r >> 7) ^ cf) & 1;
                break;
            }
            case 3: /* RCR */ {
                int nc = r & 1;
                r = (uint8_t)((r >> 1) | (cf << 7));
                cf = nc;
                of = ((r >> 7) ^ ((r >> 6) & 1)) & 1;
                break;
            }
            case 4: /* SHL */
            case 6:
                cf = (r >> 7) & 1;
                r = (uint8_t)(r << 1);
                of = ((r >> 7) ^ cf) & 1;
                break;
            case 5: /* SHR */
                cf = r & 1;
                of = (r >> 7) & 1;
                r = (uint8_t)(r >> 1);
                break;
            case 7: /* SAR */
                cf = r & 1;
                of = 0;
                r = (uint8_t)((int8_t)r >> 1);
                break;
        }
    }
    if ((op & 7) >= 4) set_szp8(r);
    set_cf_of(cf, of);
    return r;
}

static uint16_t shift16(int op, uint16_t v, uint8_t cnt) {
    cnt &= 0x1F;
    if (cnt == 0) return v;
    int cf = (cpu.flags & F_CF) ? 1 : 0;
    int of = (cpu.flags & F_OF) ? 1 : 0;
    uint16_t r = v;
    for (uint8_t i = 0; i < cnt; i++) {
        switch (op & 7) {
            case 0: /* ROL */
                cf = (r >> 15) & 1;
                r = (uint16_t)((r << 1) | cf);
                of = ((r >> 15) ^ cf) & 1;
                break;
            case 1: /* ROR */
                cf = r & 1;
                r = (uint16_t)((r >> 1) | (cf << 15));
                of = ((r >> 15) ^ ((r >> 14) & 1)) & 1;
                break;
            case 2: /* RCL */ {
                int nc = (r >> 15) & 1;
                r = (uint16_t)((r << 1) | cf);
                cf = nc;
                of = ((r >> 15) ^ cf) & 1;
                break;
            }
            case 3: /* RCR */ {
                int nc = r & 1;
                r = (uint16_t)((r >> 1) | (cf << 15));
                cf = nc;
                of = ((r >> 15) ^ ((r >> 14) & 1)) & 1;
                break;
            }
            case 4: /* SHL */
            case 6:
                cf = (r >> 15) & 1;
                r = (uint16_t)(r << 1);
                of = ((r >> 15) ^ cf) & 1;
                break;
            case 5: /* SHR */
                cf = r & 1;
                of = (r >> 15) & 1;
                r = (uint16_t)(r >> 1);
                break;
            case 7: /* SAR */
                cf = r & 1;
                of = 0;
                r = (uint16_t)((int16_t)r >> 1);
                break;
        }
    }
    if ((op & 7) >= 4) set_szp16(r);
    set_cf_of(cf, of);
    return r;
}

static int cond_true(int cc) {
    int cf = (cpu.flags & F_CF) != 0;
    int zf = (cpu.flags & F_ZF) != 0;
    int sf = (cpu.flags & F_SF) != 0;
    int of = (cpu.flags & F_OF) != 0;
    int pf = (cpu.flags & F_PF) != 0;
    switch (cc & 15) {
        case 0: return of;
        case 1: return !of;
        case 2: return cf;
        case 3: return !cf;
        case 4: return zf;
        case 5: return !zf;
        case 6: return cf || zf;
        case 7: return !(cf || zf);
        case 8: return sf;
        case 9: return !sf;
        case 10: return pf;
        case 11: return !pf;
        case 12: return sf != of;
        case 13: return sf == of;
        case 14: return zf || (sf != of);
        default: return !zf && (sf == of);
    }
}

static void step_cpu(void) {
    int seg_ovr = -1;
    int rep = 0; /* 1=REPNE, 2=REP/REPE */

prefix_loop:;
    uint8_t op = fetch8();
    switch (op) {
        case 0x26: seg_ovr = 0; goto prefix_loop;
        case 0x2E: seg_ovr = 1; goto prefix_loop;
        case 0x36: seg_ovr = 2; goto prefix_loop;
        case 0x3E: seg_ovr = 3; goto prefix_loop;
        case 0xF2: rep = 1; goto prefix_loop;
        case 0xF3: rep = 2; goto prefix_loop;
        case 0xF0: goto prefix_loop; /* LOCK */

        /* ALU rm8, r8 / r8, rm8 / AL, imm8 / rm16, r16 / r16, rm16 / AX, imm16 */
        case 0x00: case 0x08: case 0x10: case 0x18: case 0x20: case 0x28: case 0x30: case 0x38: {
            uint8_t m = fetch8(); uint16_t s, o; int ir, wb_flag;
            uint8_t d = read_rm8(m, seg_ovr, &s, &o, &ir);
            uint8_t r = alu8(op >> 3, d, get_reg8(m >> 3), &wb_flag);
            if (wb_flag) write_rm8(ir, s, o, r);
            break;
        }
        case 0x01: case 0x09: case 0x11: case 0x19: case 0x21: case 0x29: case 0x31: case 0x39: {
            uint8_t m = fetch8(); uint16_t s, o; int ir, wb_flag;
            uint16_t d = read_rm16(m, seg_ovr, &s, &o, &ir);
            uint16_t r = alu16(op >> 3, d, *reg16_ptr(m >> 3), &wb_flag);
            if (wb_flag) write_rm16(ir, s, o, r);
            break;
        }
        case 0x02: case 0x0A: case 0x12: case 0x1A: case 0x22: case 0x2A: case 0x32: case 0x3A: {
            uint8_t m = fetch8(); uint16_t s, o; int ir, wb_flag;
            uint8_t src = read_rm8(m, seg_ovr, &s, &o, &ir);
            uint8_t r = alu8(op >> 3, get_reg8(m >> 3), src, &wb_flag);
            if (wb_flag) set_reg8(m >> 3, r);
            break;
        }
        case 0x03: case 0x0B: case 0x13: case 0x1B: case 0x23: case 0x2B: case 0x33: case 0x3B: {
            uint8_t m = fetch8(); uint16_t s, o; int ir, wb_flag;
            uint16_t src = read_rm16(m, seg_ovr, &s, &o, &ir);
            uint16_t r = alu16(op >> 3, *reg16_ptr(m >> 3), src, &wb_flag);
            if (wb_flag) *reg16_ptr(m >> 3) = r;
            break;
        }
        case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C: {
            uint8_t imm = fetch8(); int wb_flag;
            uint8_t r = alu8(op >> 3, (uint8_t)(cpu.ax & 0xFF), imm, &wb_flag);
            if (wb_flag) set_reg8(0, r);
            break;
        }
        case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D: {
            uint16_t imm = fetch16(); int wb_flag;
            uint16_t r = alu16(op >> 3, cpu.ax, imm, &wb_flag);
            if (wb_flag) cpu.ax = r;
            break;
        }

        /* PUSH/POP seg */
        case 0x06: push16(cpu.es); break;
        case 0x07: cpu.es = pop16(); break;
        case 0x0E: push16(cpu.cs); break;
        case 0x16: push16(cpu.ss); break;
        case 0x17: cpu.ss = pop16(); break;
        case 0x1E: push16(cpu.ds); break;
        case 0x1F: cpu.ds = pop16(); break;

        /* DAA/DAS/AAA/AAS */
        case 0x27: { /* DAA */
            uint8_t al = cpu.ax & 0xFF;
            int cf = (cpu.flags & F_CF) != 0;
            if ((al & 0x0F) > 9 || (cpu.flags & F_AF)) {
                al += 6;
                if (al < 6) cf = 1;
                cpu.flags |= F_AF;
            } else cpu.flags &= ~F_AF;
            if ((cpu.ax & 0xFF) > 0x99 || cf) {
                al += 0x60;
                cf = 1;
            }
            set_reg8(0, al);
            set_szp8(al);
            if (cf) cpu.flags |= F_CF; else cpu.flags &= ~F_CF;
            break;
        }
        case 0x2F: { /* DAS */
            uint8_t al = cpu.ax & 0xFF;
            int cf = (cpu.flags & F_CF) != 0;
            if ((al & 0x0F) > 9 || (cpu.flags & F_AF)) {
                if (al < 6) cf = 1;
                al -= 6;
                cpu.flags |= F_AF;
            } else cpu.flags &= ~F_AF;
            if ((cpu.ax & 0xFF) > 0x99 || cf) {
                al -= 0x60;
                cf = 1;
            }
            set_reg8(0, al);
            set_szp8(al);
            if (cf) cpu.flags |= F_CF; else cpu.flags &= ~F_CF;
            break;
        }
        case 0x37: { /* AAA */
            if ((cpu.ax & 0x0F) > 9 || (cpu.flags & F_AF)) {
                cpu.ax += 0x0106;
                cpu.flags |= F_AF | F_CF;
            } else {
                cpu.flags &= ~(F_AF | F_CF);
            }
            cpu.ax &= 0xFF0F;
            break;
        }
        case 0x3F: { /* AAS */
            if ((cpu.ax & 0x0F) > 9 || (cpu.flags & F_AF)) {
                cpu.ax -= 6;
                cpu.ax = (uint16_t)(((cpu.ax >> 8) - 1) << 8) | (cpu.ax & 0xFF);
                cpu.flags |= F_AF | F_CF;
            } else {
                cpu.flags &= ~(F_AF | F_CF);
            }
            cpu.ax &= 0xFF0F;
            break;
        }

        /* INC/DEC r16 */
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: {
            uint16_t *p = reg16_ptr(op & 7);
            uint16_t a = *p;
            uint16_t r = a + 1;
            *p = r;
            set_szp16(r);
            if (a == 0x7FFF) cpu.flags |= F_OF; else cpu.flags &= ~F_OF;
            if ((a & 0x0F) == 0x0F) cpu.flags |= F_AF; else cpu.flags &= ~F_AF;
            break;
        }
        case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
            uint16_t *p = reg16_ptr(op & 7);
            uint16_t a = *p;
            uint16_t r = a - 1;
            *p = r;
            set_szp16(r);
            if (a == 0x8000) cpu.flags |= F_OF; else cpu.flags &= ~F_OF;
            if ((a & 0x0F) == 0) cpu.flags |= F_AF; else cpu.flags &= ~F_AF;
            break;
        }

        /* PUSH/POP r16 */
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x55: case 0x56: case 0x57:
            push16(*reg16_ptr(op & 7));
            break;
        case 0x54: /* PUSH SP (8086/186: pushes new SP on 8086, old SP on 286+; 186 pushes new SP) */
            push16(cpu.sp);
            break;
        case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            *reg16_ptr(op & 7) = pop16();
            break;

        /* 80186 PUSHA / POPA / BOUND / PUSH imm / IMUL r16,rm16,imm */
        case 0x60: {
            uint16_t tmp = cpu.sp;
            push16(cpu.ax); push16(cpu.cx); push16(cpu.dx); push16(cpu.bx);
            push16(tmp); push16(cpu.bp); push16(cpu.si); push16(cpu.di);
            break;
        }
        case 0x61:
            cpu.di = pop16(); cpu.si = pop16(); cpu.bp = pop16(); (void)pop16();
            cpu.bx = pop16(); cpu.dx = pop16(); cpu.cx = pop16(); cpu.ax = pop16();
            break;
        case 0x68:
            push16(fetch16());
            break;
        case 0x69: {
            uint8_t m = fetch8(); uint16_t s, o; int ir;
            int16_t src = (int16_t)read_rm16(m, seg_ovr, &s, &o, &ir);
            int16_t imm = (int16_t)fetch16();
            int32_t prod = (int32_t)src * imm;
            *reg16_ptr(m >> 3) = (uint16_t)prod;
            int ovf = (prod != (int32_t)(int16_t)prod);
            set_cf_of(ovf, ovf);
            break;
        }
        case 0x6A:
            push16((uint16_t)(int16_t)(int8_t)fetch8());
            break;
        case 0x6B: {
            uint8_t m = fetch8(); uint16_t s, o; int ir;
            int16_t src = (int16_t)read_rm16(m, seg_ovr, &s, &o, &ir);
            int16_t imm = (int16_t)(int8_t)fetch8();
            int32_t prod = (int32_t)src * imm;
            *reg16_ptr(m >> 3) = (uint16_t)prod;
            int ovf = (prod != (int32_t)(int16_t)prod);
            set_cf_of(ovf, ovf);
            break;
        }

        /* Jcc */
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
            int8_t disp = (int8_t)fetch8();
            if (cond_true(op & 15)) cpu.ip = (uint16_t)(cpu.ip + disp);
            break;
        }

        /* 80/81/82/83 ALU imm */
        case 0x80: case 0x82: {
            uint8_t m = fetch8(); uint16_t s, o; int ir, wb_flag;
            uint8_t d = read_rm8(m, seg_ovr, &s, &o, &ir);
            uint8_t imm = fetch8();
            uint8_t r = alu8(m >> 3, d, imm, &wb_flag);
            if (wb_flag) write_rm8(ir, s, o, r);
            break;
        }
        case 0x81: {
            uint8_t m = fetch8(); uint16_t s, o; int ir, wb_flag;
            uint16_t d = read_rm16(m, seg_ovr, &s, &o, &ir);
            uint16_t imm = fetch16();
            uint16_t r = alu16(m >> 3, d, imm, &wb_flag);
            if (wb_flag) write_rm16(ir, s, o, r);
            break;
        }
        case 0x83: {
            uint8_t m = fetch8(); uint16_t s, o; int ir, wb_flag;
            uint16_t d = read_rm16(m, seg_ovr, &s, &o, &ir);
            uint16_t imm = (uint16_t)(int16_t)(int8_t)fetch8();
            uint16_t r = alu16(m >> 3, d, imm, &wb_flag);
            if (wb_flag) write_rm16(ir, s, o, r);
            break;
        }

        /* TEST / XCHG */
        case 0x84: {
            uint8_t m = fetch8(); uint16_t s, o; int ir;
            uint8_t d = read_rm8(m, seg_ovr, &s, &o, &ir);
            set_szp8(d & get_reg8(m >> 3));
            set_cf_of(0, 0);
            break;
        }
        case 0x85: {
            uint8_t m = fetch8(); uint16_t s, o; int ir;
            uint16_t d = read_rm16(m, seg_ovr, &s, &o, &ir);
            set_szp16(d & *reg16_ptr(m >> 3));
            set_cf_of(0, 0);
            break;
        }
        case 0x86: {
            uint8_t m = fetch8(); uint16_t s, o; int ir;
            uint8_t d = read_rm8(m, seg_ovr, &s, &o, &ir);
            uint8_t r = get_reg8(m >> 3);
            set_reg8(m >> 3, d);
            write_rm8(ir, s, o, r);
            break;
        }
        case 0x87: {
            uint8_t m = fetch8(); uint16_t s, o; int ir;
            uint16_t d = read_rm16(m, seg_ovr, &s, &o, &ir);
            uint16_t *p = reg16_ptr(m >> 3);
            uint16_t tmp = *p;
            *p = d;
            write_rm16(ir, s, o, tmp);
            break;
        }

        /* MOV */
        case 0x88: {
            uint8_t m = fetch8(); uint16_t s, o; int ir;
            decode_modrm(m, seg_ovr, &s, &o, &ir);
            write_rm8(ir, s, o, get_reg8(m >> 3));
            break;
        }
        case 0x89: {
            uint8_t m = fetch8(); uint16_t s, o; int ir;
            decode_modrm(m, seg_ovr, &s, &o, &ir);
            write_rm16(ir, s, o, *reg16_ptr(m >> 3));
            break;
        }
        case 0x8A: {
            uint8_t m = fetch8(); uint16_t s, o; int ir;
            set_reg8(m >> 3, read_rm8(m, seg_ovr, &s, &o, &ir));
            break;
        }
        case 0x8B: {
            uint8_t m = fetch8(); uint16_t s, o; int ir;
            *reg16_ptr(m >> 3) = read_rm16(m, seg_ovr, &s, &o, &ir);
            break;
        }
        case 0x8C: {
            uint8_t m = fetch8(); uint16_t s, o; int ir;
            decode_modrm(m, seg_ovr, &s, &o, &ir);
            write_rm16(ir, s, o, *seg_ptr(m >> 3));
            break;
        }
        case 0x8D: { /* LEA */
            uint8_t m = fetch8(); uint16_t s, o; int ir;
            decode_modrm(m, seg_ovr, &s, &o, &ir);
            *reg16_ptr(m >> 3) = o;
            break;
        }
        case 0x8E: {
            uint8_t m = fetch8(); uint16_t s, o; int ir;
            *seg_ptr(m >> 3) = read_rm16(m, seg_ovr, &s, &o, &ir);
            break;
        }
        case 0x8F: {
            uint8_t m = fetch8(); uint16_t s, o; int ir;
            decode_modrm(m, seg_ovr, &s, &o, &ir);
            write_rm16(ir, s, o, pop16());
            break;
        }

        /* NOP / XCHG AX, r16 */
        case 0x90: break;
        case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
            uint16_t *p = reg16_ptr(op & 7);
            uint16_t t = cpu.ax; cpu.ax = *p; *p = t;
            break;
        }

        case 0x98: cpu.ax = (uint16_t)(int16_t)(int8_t)(cpu.ax & 0xFF); break; /* CBW */
        case 0x99: cpu.dx = (cpu.ax & 0x8000) ? 0xFFFF : 0x0000; break; /* CWD */
        case 0x9A: { /* CALL FAR ptr16:16 */
            uint16_t off = fetch16();
            uint16_t seg = fetch16();
            if (check_far_traps(seg, off)) break;
            push16(cpu.cs);
            push16(cpu.ip);
            cpu.cs = seg;
            cpu.ip = off;
            break;
        }
        case 0x9B: break; /* WAIT */
        case 0x9C: push16(cpu.flags); break;
        case 0x9D: cpu.flags = (pop16() & 0x0FD5u) | 0x0002u; break;
        case 0x9E: cpu.flags = (cpu.flags & 0xFF00u) | ((cpu.ax >> 8) & 0xD5u) | 0x02u; break;
        case 0x9F: cpu.ax = (cpu.ax & 0x00FFu) | ((cpu.flags & 0xFFu) << 8); break;

        /* MOV AL/AX, moffs / moffs, AL/AX */
        case 0xA0: { uint16_t a = fetch16(); set_reg8(0, rb(phys(eff_seg(seg_ovr, cpu.ds), a))); break; }
        case 0xA1: { uint16_t a = fetch16(); cpu.ax = rw(phys(eff_seg(seg_ovr, cpu.ds), a)); break; }
        case 0xA2: { uint16_t a = fetch16(); wb(phys(eff_seg(seg_ovr, cpu.ds), a), cpu.ax & 0xFF); break; }
        case 0xA3: { uint16_t a = fetch16(); ww(phys(eff_seg(seg_ovr, cpu.ds), a), cpu.ax); break; }

        /* String ops */
        case 0xA4: { /* MOVSB */
            uint16_t s = eff_seg(seg_ovr, cpu.ds);
            int16_t d = (cpu.flags & F_DF) ? -1 : 1;
            uint16_t n = rep ? cpu.cx : 1;
            for (uint16_t i = 0; i < n; i++) {
                wb(phys(cpu.es, cpu.di), rb(phys(s, cpu.si)));
                cpu.si += d; cpu.di += d;
            }
            if (rep) cpu.cx = 0;
            break;
        }
        case 0xA5: { /* MOVSW */
            uint16_t s = eff_seg(seg_ovr, cpu.ds);
            int16_t d = (cpu.flags & F_DF) ? -2 : 2;
            uint16_t n = rep ? cpu.cx : 1;
            for (uint16_t i = 0; i < n; i++) {
                ww(phys(cpu.es, cpu.di), rw(phys(s, cpu.si)));
                cpu.si += d; cpu.di += d;
            }
            if (rep) cpu.cx = 0;
            break;
        }
        case 0xA6: { /* CMPSB */
            uint16_t s = eff_seg(seg_ovr, cpu.ds);
            int16_t d = (cpu.flags & F_DF) ? -1 : 1;
            uint16_t n = rep ? cpu.cx : 1;
            for (uint16_t i = 0; i < n; i++) {
                int dummy;
                uint8_t a = rb(phys(s, cpu.si));
                uint8_t b = rb(phys(cpu.es, cpu.di));
                alu8(7, a, b, &dummy);
                cpu.si += d; cpu.di += d;
                if (rep) {
                    cpu.cx--;
                    if ((rep == 2 && !(cpu.flags & F_ZF)) || (rep == 1 && (cpu.flags & F_ZF))) break;
                }
            }
            break;
        }
        case 0xA7: { /* CMPSW */
            uint16_t s = eff_seg(seg_ovr, cpu.ds);
            int16_t d = (cpu.flags & F_DF) ? -2 : 2;
            uint16_t n = rep ? cpu.cx : 1;
            for (uint16_t i = 0; i < n; i++) {
                int dummy;
                uint16_t a = rw(phys(s, cpu.si));
                uint16_t b = rw(phys(cpu.es, cpu.di));
                alu16(7, a, b, &dummy);
                cpu.si += d; cpu.di += d;
                if (rep) {
                    cpu.cx--;
                    if ((rep == 2 && !(cpu.flags & F_ZF)) || (rep == 1 && (cpu.flags & F_ZF))) break;
                }
            }
            break;
        }
        case 0xA8: { uint8_t imm = fetch8(); set_szp8((cpu.ax & 0xFF) & imm); set_cf_of(0, 0); break; }
        case 0xA9: { uint16_t imm = fetch16(); set_szp16(cpu.ax & imm); set_cf_of(0, 0); break; }
        case 0xAA: { /* STOSB */
            int16_t d = (cpu.flags & F_DF) ? -1 : 1;
            uint16_t n = rep ? cpu.cx : 1;
            uint8_t al = cpu.ax & 0xFF;
            for (uint16_t i = 0; i < n; i++) {
                wb(phys(cpu.es, cpu.di), al);
                cpu.di += d;
            }
            if (rep) cpu.cx = 0;
            break;
        }
        case 0xAB: { /* STOSW */
            int16_t d = (cpu.flags & F_DF) ? -2 : 2;
            uint16_t n = rep ? cpu.cx : 1;
            for (uint16_t i = 0; i < n; i++) {
                ww(phys(cpu.es, cpu.di), cpu.ax);
                cpu.di += d;
            }
            if (rep) cpu.cx = 0;
            break;
        }
        case 0xAC: { /* LODSB */
            uint16_t s = eff_seg(seg_ovr, cpu.ds);
            int16_t d = (cpu.flags & F_DF) ? -1 : 1;
            uint16_t n = rep ? cpu.cx : 1;
            for (uint16_t i = 0; i < n; i++) {
                set_reg8(0, rb(phys(s, cpu.si)));
                cpu.si += d;
            }
            if (rep) cpu.cx = 0;
            break;
        }
        case 0xAD: { /* LODSW */
            uint16_t s = eff_seg(seg_ovr, cpu.ds);
            int16_t d = (cpu.flags & F_DF) ? -2 : 2;
            uint16_t n = rep ? cpu.cx : 1;
            for (uint16_t i = 0; i < n; i++) {
                cpu.ax = rw(phys(s, cpu.si));
                cpu.si += d;
            }
            if (rep) cpu.cx = 0;
            break;
        }
        case 0xAE: { /* SCASB */
            int16_t d = (cpu.flags & F_DF) ? -1 : 1;
            uint16_t n = rep ? cpu.cx : 1;
            uint8_t al = cpu.ax & 0xFF;
            for (uint16_t i = 0; i < n; i++) {
                int dummy;
                alu8(7, al, rb(phys(cpu.es, cpu.di)), &dummy);
                cpu.di += d;
                if (rep) {
                    cpu.cx--;
                    if ((rep == 2 && !(cpu.flags & F_ZF)) || (rep == 1 && (cpu.flags & F_ZF))) break;
                }
            }
            break;
        }
        case 0xAF: { /* SCASW */
            int16_t d = (cpu.flags & F_DF) ? -2 : 2;
            uint16_t n = rep ? cpu.cx : 1;
            for (uint16_t i = 0; i < n; i++) {
                int dummy;
                alu16(7, cpu.ax, rw(phys(cpu.es, cpu.di)), &dummy);
                cpu.di += d;
                if (rep) {
                    cpu.cx--;
                    if ((rep == 2 && !(cpu.flags & F_ZF)) || (rep == 1 && (cpu.flags & F_ZF))) break;
                }
            }
            break;
        }

        /* MOV r8, imm8 / r16, imm16 */
        case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
            set_reg8(op & 7, fetch8());
            break;
        case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            *reg16_ptr(op & 7) = fetch16();
            break;

        /* Shifts & RET */
        case 0xC0: {
            uint8_t m = fetch8(); uint16_t s, o; int ir;
            uint8_t d = read_rm8(m, seg_ovr, &s, &o, &ir);
            uint8_t cnt = fetch8();
            write_rm8(ir, s, o, shift8(m >> 3, d, cnt));
            break;
        }
        case 0xC1: {
            uint8_t m = fetch8(); uint16_t s, o; int ir;
            uint16_t d = read_rm16(m, seg_ovr, &s, &o, &ir);
            uint8_t cnt = fetch8();
            write_rm16(ir, s, o, shift16(m >> 3, d, cnt));
            break;
        }
        case 0xC2: { uint16_t n = fetch16(); cpu.ip = pop16(); cpu.sp += n; break; }
        case 0xC3: cpu.ip = pop16(); break;
        case 0xC4: { /* LES */
            uint8_t m = fetch8(); uint16_t s, o; int ir;
            decode_modrm(m, seg_ovr, &s, &o, &ir);
            *reg16_ptr(m >> 3) = rw(phys(s, o));
            cpu.es = rw(phys(s, o + 2));
            break;
        }
        case 0xC5: { /* LDS */
            uint8_t m = fetch8(); uint16_t s, o; int ir;
            decode_modrm(m, seg_ovr, &s, &o, &ir);
            *reg16_ptr(m >> 3) = rw(phys(s, o));
            cpu.ds = rw(phys(s, o + 2));
            break;
        }
        case 0xC6: {
            uint8_t m = fetch8(); uint16_t s, o; int ir;
            decode_modrm(m, seg_ovr, &s, &o, &ir);
            write_rm8(ir, s, o, fetch8());
            break;
        }
        case 0xC7: {
            uint8_t m = fetch8(); uint16_t s, o; int ir;
            decode_modrm(m, seg_ovr, &s, &o, &ir);
            write_rm16(ir, s, o, fetch16());
            break;
        }
        case 0xC8: { /* ENTER */
            uint16_t sz = fetch16();
            uint8_t lvl = fetch8() & 0x1F;
            push16(cpu.bp);
            uint16_t frame = cpu.sp;
            if (lvl > 0) {
                for (int i = 1; i < lvl; i++) {
                    cpu.bp -= 2;
                    push16(rw(phys(cpu.ss, cpu.bp)));
                }
                push16(frame);
            }
            cpu.bp = frame;
            cpu.sp -= sz;
            break;
        }
        case 0xC9: cpu.sp = cpu.bp; cpu.bp = pop16(); break; /* LEAVE */
        case 0xCA: { uint16_t n = fetch16(); cpu.ip = pop16(); cpu.cs = pop16(); cpu.sp += n; break; }
        case 0xCB: cpu.ip = pop16(); cpu.cs = pop16(); break;
        case 0xCC: handle_int(3); break;
        case 0xCD: handle_int(fetch8()); break;
        case 0xCE: if (cpu.flags & F_OF) handle_int(4); break;
        case 0xCF: cpu.ip = pop16(); cpu.cs = pop16(); cpu.flags = (pop16() & 0x0FD5u) | 0x0002u; break;

        case 0xD0: {
            uint8_t m = fetch8(); uint16_t s, o; int ir;
            uint8_t d = read_rm8(m, seg_ovr, &s, &o, &ir);
            write_rm8(ir, s, o, shift8(m >> 3, d, 1));
            break;
        }
        case 0xD1: {
            uint8_t m = fetch8(); uint16_t s, o; int ir;
            uint16_t d = read_rm16(m, seg_ovr, &s, &o, &ir);
            write_rm16(ir, s, o, shift16(m >> 3, d, 1));
            break;
        }
        case 0xD2: {
            uint8_t m = fetch8(); uint16_t s, o; int ir;
            uint8_t d = read_rm8(m, seg_ovr, &s, &o, &ir);
            write_rm8(ir, s, o, shift8(m >> 3, d, cpu.cx & 0xFF));
            break;
        }
        case 0xD3: {
            uint8_t m = fetch8(); uint16_t s, o; int ir;
            uint16_t d = read_rm16(m, seg_ovr, &s, &o, &ir);
            write_rm16(ir, s, o, shift16(m >> 3, d, cpu.cx & 0xFF));
            break;
        }
        case 0xD4: { /* AAM */
            uint8_t base = fetch8();
            if (base == 0) { cpu.ax = 0; break; }
            uint8_t al = cpu.ax & 0xFF;
            cpu.ax = ((uint16_t)(al / base) << 8) | (al % base);
            set_szp8(cpu.ax & 0xFF);
            break;
        }
        case 0xD5: { /* AAD */
            uint8_t base = fetch8();
            uint8_t al = (uint8_t)(((cpu.ax >> 8) * base + (cpu.ax & 0xFF)) & 0xFF);
            cpu.ax = al;
            set_szp8(al);
            break;
        }
        case 0xD7: { /* XLAT */
            uint16_t s = eff_seg(seg_ovr, cpu.ds);
            set_reg8(0, rb(phys(s, cpu.bx + (cpu.ax & 0xFF))));
            break;
        }
        case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF: {
            /* x87 FPU opcodes: consume modrm if needed (software FPU emu is used if no 8087) */
            uint8_t m = fetch8(); uint16_t s, o; int ir;
            if ((m >> 6) != 3) decode_modrm(m, seg_ovr, &s, &o, &ir);
            break;
        }

        /* LOOP / JCXZ */
        case 0xE0: { int8_t d = (int8_t)fetch8(); cpu.cx--; if (cpu.cx && !(cpu.flags & F_ZF)) cpu.ip += d; break; }
        case 0xE1: { int8_t d = (int8_t)fetch8(); cpu.cx--; if (cpu.cx && (cpu.flags & F_ZF)) cpu.ip += d; break; }
        case 0xE2: { int8_t d = (int8_t)fetch8(); cpu.cx--; if (cpu.cx) cpu.ip += d; break; }
        case 0xE3: { int8_t d = (int8_t)fetch8(); if (!cpu.cx) cpu.ip += d; break; }

        /* IN / OUT */
        case 0xE4: { (void)fetch8(); set_reg8(0, 0x00); break; }
        case 0xE5: { (void)fetch8(); cpu.ax = 0x0000; break; }
        case 0xE6: { (void)fetch8(); break; }
        case 0xE7: { (void)fetch8(); break; }
        case 0xEC: { set_reg8(0, 0x00); break; }
        case 0xED: { cpu.ax = 0x0000; break; }
        case 0xEE: break;
        case 0xEF: break;

        /* CALL / JMP */
        case 0xE8: { int16_t d = (int16_t)fetch16(); push16(cpu.ip); cpu.ip += d; break; }
        case 0xE9: { int16_t d = (int16_t)fetch16(); cpu.ip += d; break; }
        case 0xEA: {
            uint16_t off = fetch16(); uint16_t seg = fetch16();
            cpu.cs = seg; cpu.ip = off;
            break;
        }
        case 0xEB: { int8_t d = (int8_t)fetch8(); cpu.ip += d; break; }

        case 0xF4: cpu.halted = 1; break; /* HLT */
        case 0xF5: cpu.flags ^= F_CF; break; /* CMC */
        case 0xF8: cpu.flags &= ~F_CF; break;
        case 0xF9: cpu.flags |= F_CF; break;
        case 0xFA: cpu.flags &= ~F_IF; break;
        case 0xFB: cpu.flags |= F_IF; break;
        case 0xFC: cpu.flags &= ~F_DF; break;
        case 0xFD: cpu.flags |= F_DF; break;

        /* F6 / F7 */
        case 0xF6: {
            uint8_t m = fetch8(); uint16_t s, o; int ir;
            uint8_t d = read_rm8(m, seg_ovr, &s, &o, &ir);
            switch ((m >> 3) & 7) {
                case 0: case 1: {
                    uint8_t imm = fetch8();
                    set_szp8(d & imm); set_cf_of(0, 0);
                    break;
                }
                case 2: write_rm8(ir, s, o, ~d); break;
                case 3: {
                    int dummy;
                    uint8_t r = alu8(5, 0, d, &dummy);
                    write_rm8(ir, s, o, r);
                    if (d != 0) cpu.flags |= F_CF; else cpu.flags &= ~F_CF;
                    break;
                }
                case 4: {
                    uint16_t prod = (uint16_t)(cpu.ax & 0xFF) * d;
                    cpu.ax = prod;
                    int ovf = (prod >> 8) != 0;
                    set_cf_of(ovf, ovf);
                    break;
                }
                case 5: {
                    int16_t prod = (int16_t)(int8_t)(cpu.ax & 0xFF) * (int8_t)d;
                    cpu.ax = (uint16_t)prod;
                    int ovf = (prod != (int16_t)(int8_t)prod);
                    set_cf_of(ovf, ovf);
                    break;
                }
                case 6: {
                    if (d == 0) { cpu.ax = 0; break; }
                    uint16_t q = cpu.ax / d, r = cpu.ax % d;
                    cpu.ax = (uint16_t)((r << 8) | (q & 0xFF));
                    break;
                }
                case 7: {
                    if (d == 0) { cpu.ax = 0; break; }
                    int16_t num = (int16_t)cpu.ax;
                    int8_t den = (int8_t)d;
                    int16_t q = num / den, r = num % den;
                    cpu.ax = (uint16_t)(((uint8_t)r << 8) | (uint8_t)q);
                    break;
                }
            }
            break;
        }
        case 0xF7: {
            uint8_t m = fetch8(); uint16_t s, o; int ir;
            uint16_t d = read_rm16(m, seg_ovr, &s, &o, &ir);
            switch ((m >> 3) & 7) {
                case 0: case 1: {
                    uint16_t imm = fetch16();
                    set_szp16(d & imm); set_cf_of(0, 0);
                    break;
                }
                case 2: write_rm16(ir, s, o, ~d); break;
                case 3: {
                    int dummy;
                    uint16_t r = alu16(5, 0, d, &dummy);
                    write_rm16(ir, s, o, r);
                    if (d != 0) cpu.flags |= F_CF; else cpu.flags &= ~F_CF;
                    break;
                }
                case 4: {
                    uint32_t prod = (uint32_t)cpu.ax * d;
                    cpu.ax = (uint16_t)(prod & 0xFFFF);
                    cpu.dx = (uint16_t)(prod >> 16);
                    int ovf = cpu.dx != 0;
                    set_cf_of(ovf, ovf);
                    break;
                }
                case 5: {
                    int32_t prod = (int32_t)(int16_t)cpu.ax * (int16_t)d;
                    cpu.ax = (uint16_t)(prod & 0xFFFF);
                    cpu.dx = (uint16_t)((uint32_t)prod >> 16);
                    int ovf = (prod != (int32_t)(int16_t)prod);
                    set_cf_of(ovf, ovf);
                    break;
                }
                case 6: {
                    if (d == 0) { cpu.ax = cpu.dx = 0; break; }
                    uint32_t num = ((uint32_t)cpu.dx << 16) | cpu.ax;
                    cpu.ax = (uint16_t)(num / d);
                    cpu.dx = (uint16_t)(num % d);
                    break;
                }
                case 7: {
                    if (d == 0) { cpu.ax = cpu.dx = 0; break; }
                    int32_t num = (int32_t)(((uint32_t)cpu.dx << 16) | cpu.ax);
                    int16_t den = (int16_t)d;
                    cpu.ax = (uint16_t)(int16_t)(num / den);
                    cpu.dx = (uint16_t)(int16_t)(num % den);
                    break;
                }
            }
            break;
        }

        /* FE / FF */
        case 0xFE: {
            uint8_t m = fetch8(); uint16_t s, o; int ir;
            uint8_t d = read_rm8(m, seg_ovr, &s, &o, &ir);
            if (((m >> 3) & 7) == 0) {
                uint8_t r = d + 1;
                write_rm8(ir, s, o, r);
                set_szp8(r);
                if (d == 0x7F) cpu.flags |= F_OF; else cpu.flags &= ~F_OF;
                if ((d & 0x0F) == 0x0F) cpu.flags |= F_AF; else cpu.flags &= ~F_AF;
            } else {
                uint8_t r = d - 1;
                write_rm8(ir, s, o, r);
                set_szp8(r);
                if (d == 0x80) cpu.flags |= F_OF; else cpu.flags &= ~F_OF;
                if ((d & 0x0F) == 0) cpu.flags |= F_AF; else cpu.flags &= ~F_AF;
            }
            break;
        }
        case 0xFF: {
            uint8_t m = fetch8(); uint16_t s, o; int ir;
            uint16_t d = read_rm16(m, seg_ovr, &s, &o, &ir);
            switch ((m >> 3) & 7) {
                case 0: {
                    uint16_t r = d + 1;
                    write_rm16(ir, s, o, r);
                    set_szp16(r);
                    if (d == 0x7FFF) cpu.flags |= F_OF; else cpu.flags &= ~F_OF;
                    if ((d & 0x0F) == 0x0F) cpu.flags |= F_AF; else cpu.flags &= ~F_AF;
                    break;
                }
                case 1: {
                    uint16_t r = d - 1;
                    write_rm16(ir, s, o, r);
                    set_szp16(r);
                    if (d == 0x8000) cpu.flags |= F_OF; else cpu.flags &= ~F_OF;
                    if ((d & 0x0F) == 0) cpu.flags |= F_AF; else cpu.flags &= ~F_AF;
                    break;
                }
                case 2: push16(cpu.ip); cpu.ip = d; break;
                case 3: {
                    uint16_t t_off = rw(phys(s, o));
                    uint16_t t_seg = rw(phys(s, o + 2));
                    if (check_far_traps(t_seg, t_off)) break;
                    push16(cpu.cs); push16(cpu.ip);
                    cpu.cs = t_seg; cpu.ip = t_off;
                    break;
                }
                case 4: cpu.ip = d; break;
                case 5: {
                    uint16_t t_off = rw(phys(s, o));
                    uint16_t t_seg = rw(phys(s, o + 2));
                    cpu.cs = t_seg; cpu.ip = t_off;
                    break;
                }
                case 6: push16(d); break;
            }
            break;
        }

        default:
            fprintf(stderr, "Unimplemented opcode 0x%02x at %04x:%04x\n", op, cpu.cs, cpu.ip - 1);
            cpu.halted = 1;
            break;
    }
}

static void run_until_trap(uint64_t max_steps) {
    cpu.halted = 0;
    uint16_t prev_cs = cpu.cs, prev_ip = cpu.ip;
    for (uint64_t step = 0; step < max_steps && !cpu.halted; step++) {
        if (cpu.cs == TRAP_SEG) break;
        if (search_active && cpu.cs == LOAD_SEG && cpu.ip == 0x0182 && ds_rw(0xd644) > search_initial_ply) {
            /* Engine finished its move and looped back to top of game loop */
            cpu.idle_hit = 1;
            cpu.halted = 1;
            break;
        }
        if (cpu.cs == LOAD_SEG && (cpu.ip == 0x9226 || cpu.ip == 0x9247 || cpu.ip == 0x925a || cpu.ip == 0x926e)) {
            uint16_t fn_ip = cpu.ip;
            cpu.ip = pop16();
            cpu.cs = pop16();
            check_far_traps(LOAD_SEG, fn_ip);
            continue;
        }
        if (cpu.cs == LOAD_SEG && (cpu.ip == 0x7253 || cpu.ip == 0x72b6)) {
            uint32_t start_p = rw(phys(cpu.ss, cpu.bp - 4)) | ((uint32_t)rw(phys(cpu.ss, cpu.bp - 2)) << 16);
            uint32_t end_p   = rw(phys(cpu.ss, cpu.bp - 8)) | ((uint32_t)rw(phys(cpu.ss, cpu.bp - 6)) << 16);
            for (uint32_t p = start_p; p < end_p; p += 10) {
                if (p < EXT_MEM_SIZE) ext_mem[p] = 0;
            }
            cpu.ip = (cpu.ip == 0x7253) ? 0x727d : 0x738e;
            continue;
        }
        if (cpu.cs < LOAD_SEG) {
            fprintf(stderr, "Jumped below LOAD_SEG! step=%llu from %04x:%04x to %04x:%04x SP=%04x BP=%04x\n",
                    (unsigned long long)step, prev_cs, prev_ip, cpu.cs, cpu.ip, cpu.sp, cpu.bp);
            cpu.halted = 1;
            break;
        }
        prev_cs = cpu.cs; prev_ip = cpu.ip;
        if ((step & 0x7FFu) == 0) {
            update_bda_ticks();
            if (search_active) {
                if (stop_requested || (search_deadline_ms && get_ms() >= search_deadline_ms)) {
                    /* Trigger clean search abort in MChessPro */
                    ds_wb(0x4e11, 1);
                    ds_wb(0xdac0, 1);
                }
            }
        }
        step_cpu();
    }
}

static void call_far(uint16_t seg, uint16_t off, uint64_t max_steps) {
    uint16_t saved_cs = cpu.cs, saved_ip = cpu.ip;
    push16(TRAP_SEG);
    push16(0x0000);
    cpu.cs = seg;
    cpu.ip = off;
    run_until_trap(max_steps);
    cpu.cs = saved_cs;
    cpu.ip = saved_ip;
}

/* =========================================================================
 * Engine Initialization, Snapshot & Position/Search Helpers
 * ========================================================================= */

static uint8_t dgroup_snapshot[65536];
static CPU boot_cpu_snapshot;
static int current_stm = 0; /* 0=White, 1=Black */

static void boot_engine(void) {
    init_parity();
    init_dos_heap();
    memset(mem, 0, MEM_SIZE);
    memset(ext_mem, 0, EXT_MEM_SIZE);
    memset(vfiles, 0, sizeof(vfiles));
    boot_ms = get_ms();

    for (int v = 0; v < 256; v++) {
        uint16_t off = (uint16_t)(v * 4);
        ww((uint32_t)v * 4, off);
        ww((uint32_t)v * 4 + 2, 0xE000u);
        uint32_t stub = phys(0xE000u, off);
        mem[stub] = 0xCD;
        mem[stub + 1] = (uint8_t)v;
        mem[stub + 2] = 0xCF;
    }

    memcpy(&mem[LOAD_SEG << 4], mcpro_module, MCPRO_MODULE_SIZE);
    for (uint32_t i = 0; i < MCPRO_RELOC_COUNT; i++) {
        uint32_t addr = (LOAD_SEG << 4) + mcpro_relocs[i];
        ww(addr, rw(addr) + LOAD_SEG);
    }

    const char *env_str = "COMSPEC=C:\\COMMAND.COM\0";
    uint32_t ep = ENV_SEG << 4;
    memcpy(&mem[ep], env_str, 24);
    ep += 24;
    mem[ep++] = 0;
    ww(ep, 1); ep += 2;
    const char *prog = "C:\\MCPRO\\MCPRO.EXE";
    memcpy(&mem[ep], prog, strlen(prog) + 1);

    uint32_t psp = PSP_SEG << 4;
    mem[psp + 0] = 0xCD; mem[psp + 1] = 0x20;
    ww(psp + 2, 0x9FFF);
    ww(psp + 0x2C, ENV_SEG);
    mem[psp + 0x80] = 3;
    memcpy(&mem[psp + 0x81], " /X\r", 4);

    ww(0x0410, 0x0021);
    ww(0x0413, 640);
    ww(0x044A, 80);
    mem[0x0449] = 3;

    memset(&cpu, 0, sizeof(cpu));
    cpu.ds = PSP_SEG;
    cpu.es = PSP_SEG;
    cpu.cs = LOAD_SEG + MCPRO_INIT_CS;
    cpu.ip = MCPRO_INIT_IP;
    cpu.ss = LOAD_SEG + MCPRO_INIT_SS;
    cpu.sp = MCPRO_INIT_SP;
    cpu.flags = 0x0202;

    cpu.stop_on_idle = 1;
    cpu.idle_hit = 0;
    run_until_trap(50000000ULL);
    cpu.stop_on_idle = 0;

    memcpy(dgroup_snapshot, &mem[DGROUP_SEG << 4], 65536);
    boot_cpu_snapshot = cpu;
}

static void restore_startpos(void) {
    memcpy(&mem[DGROUP_SEG << 4], dgroup_snapshot, 65536);
    cpu = boot_cpu_snapshot;
    current_stm = 0;
    apply_engine_options();
}

static void make_uci_move(const char *uci_str) {
    if (strlen(uci_str) < 4) return;
    int f1 = uci_str[0] - 'a';
    int r1 = uci_str[1] - '1';
    int f2 = uci_str[2] - 'a';
    int r2 = uci_str[3] - '1';
    if (f1 < 0 || f1 > 7 || r1 < 0 || r1 > 7 || f2 < 0 || f2 > 7 || r2 < 0 || r2 > 7) return;

    uint8_t sq1 = (uint8_t)((r1 + 2) * 10 + (f1 + 1));
    uint8_t sq2 = (uint8_t)((r2 + 2) * 10 + (f2 + 1));
    uint8_t pc = ds_rb((uint16_t)(0xd574 + sq1));
    uint8_t flag = 0;
    if (uci_str[4] != '\0') {
        switch (tolower((unsigned char)uci_str[4])) {
            case 'n': flag = 2; break;
            case 'b': flag = 3; break;
            case 'r': flag = 4; break;
            case 'q': flag = 5; break;
            default: break;
        }
    } else if ((pc == 6 || pc == 13) && (f2 - f1 == 2 || f1 - f2 == 2)) {
        flag = 6; /* Castling */
    } else if ((pc == 1 || pc == 8) && f1 != f2 && ds_rb((uint16_t)(0xd574 + sq2)) == 0) {
        flag = 1; /* En passant */
    }
    ds_wb(0xce4c, sq1);
    ds_wb(0xce4d, sq2);
    ds_wb(0xce4e, flag);
    ds_wb(0x4d4c, (uint8_t)current_stm);
    ds_wb(0xd54a, (uint8_t)current_stm);

    /* Call make_move(0xce4c, stm, ply+1, 0xce4f, 0xce50) */
    uint16_t ply = ds_rw(0xd644) + 1;
    push16(0xce4c);
    push16((uint16_t)current_stm);
    push16(ply);
    push16(0xce4f);
    push16(0xce50);
    call_far(LOAD_SEG + 0x0fcb, 0xb312, 1000000ULL);
    current_stm ^= 1;
    ds_wb(0x4d4c, (uint8_t)current_stm);
    ds_wb(0xd54a, (uint8_t)current_stm);
}

static void setup_fen_position(const char *fen) {
    restore_startpos();

    /* Clear 10x12 setup board at DS:0xc632 */
    for (int r = 0; r < 12; r++) {
        for (int f = 0; f < 10; f++) {
            uint8_t v = (r < 2 || r > 9 || f < 1 || f > 8) ? 0x07 : 0x00;
            ds_wb((uint16_t)(0xc632 + r * 10 + f), v);
        }
    }

    const char *p = fen;
    int r = 7, f = 0;
    while (*p && *p != ' ') {
        char c = *p++;
        if (c == '/') {
            r--; f = 0;
        } else if (c >= '1' && c <= '8') {
            f += c - '0';
        } else {
            uint8_t pc = 0;
            switch (c) {
                case 'P': pc = 1; break;
                case 'N': pc = 2; break;
                case 'B': pc = 3; break;
                case 'R': pc = 4; break;
                case 'Q': pc = 5; break;
                case 'K': pc = 6; break;
                case 'p': pc = 8; break;
                case 'n': pc = 9; break;
                case 'b': pc = 10; break;
                case 'r': pc = 11; break;
                case 'q': pc = 12; break;
                case 'k': pc = 13; break;
            }
            if (r >= 0 && r < 8 && f >= 0 && f < 8 && pc != 0) {
                uint8_t sq = (uint8_t)((r + 2) * 10 + (f + 1));
                ds_wb((uint16_t)(0xc632 + sq), pc);
            }
            f++;
        }
    }
    while (*p == ' ') p++;

    current_stm = (*p == 'b') ? 1 : 0;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;

    int wk = 0, wq = 0, bk = 0, bq = 0;
    if (*p == '-') {
        p++;
    } else {
        while (*p && *p != ' ') {
            if (*p == 'K') wk = 1;
            else if (*p == 'Q') wq = 1;
            else if (*p == 'k') bk = 1;
            else if (*p == 'q') bq = 1;
            p++;
        }
    }
    while (*p == ' ') p++;

    uint8_t ep_sq = 0;
    if (*p == '-') {
        p++;
    } else if (p[0] >= 'a' && p[0] <= 'h' && p[1] >= '1' && p[1] <= '8') {
        ep_sq = (uint8_t)(((p[1] - '1') + 2) * 10 + ((p[0] - 'a') + 1));
        p += 2;
    }
    while (*p == ' ') p++;

    int halfmove = 0;
    if (isdigit((unsigned char)*p)) {
        halfmove = atoi(p);
    }

    ds_wb(0x4d4c, (uint8_t)current_stm);
    ds_wb(0xd54a, (uint8_t)current_stm);
    call_far(LOAD_SEG + 0x2f95, 0xa5d0, 5000000ULL); /* setup_accept */

    /* Override castling, ep, halfmove, and recompute hash */
    ds_wb(0xe6a6, wk ? 0 : 0xFF);
    ds_wb(0x3038, wq ? 0 : 0xFF);
    ds_wb(0xe6a7, bk ? 0 : 0xFF);
    ds_wb(0x3039, bq ? 0 : 0xFF);
    ds_wb(0x8fee, (!wk && !wq) ? 1 : 0);
    ds_wb(0xe14e, (!bk && !bq) ? 1 : 0);
    ds_wb((uint16_t)(0xd54c + ds_rw(0xd644)), ep_sq);
    ds_wb(0xdc75, (uint8_t)halfmove);
    call_far(LOAD_SEG + 0x3f47, 0x1a26, 1000000ULL);
    apply_engine_options();
    ds_wb(0x4cc4, 1); /* Disable opening book for custom FENs */
}

/* =========================================================================
 * Non-blocking Stdin Reader Thread (for stop / ponderhit / quit)
 * ========================================================================= */

#define CMD_Q_SIZE 64
static char cmd_queue[CMD_Q_SIZE][4096];
static int cmd_q_head = 0, cmd_q_tail = 0;
static volatile int stdin_eof = 0;

#ifdef _WIN32
static CRITICAL_SECTION q_lock;
static HANDLE q_sem;
static DWORD WINAPI stdin_thread_fn(LPVOID arg) {
    (void)arg;
    char line[4096];
    while (fgets(line, sizeof(line), stdin)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (search_active && (strcmp(line, "stop") == 0 || strcmp(line, "quit") == 0)) {
            stop_requested = 1;
        }
        EnterCriticalSection(&q_lock);
        int next = (cmd_q_tail + 1) % CMD_Q_SIZE;
        if (next != cmd_q_head) {
            strcpy(cmd_queue[cmd_q_tail], line);
            cmd_q_tail = next;
        }
        LeaveCriticalSection(&q_lock);
        ReleaseSemaphore(q_sem, 1, NULL);
        if (strcmp(line, "quit") == 0) break;
    }
    stdin_eof = 1;
    ReleaseSemaphore(q_sem, 1, NULL);
    return 0;
}
#else
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cond = PTHREAD_COND_INITIALIZER;
static void *stdin_thread_fn(void *arg) {
    (void)arg;
    char line[4096];
    while (fgets(line, sizeof(line), stdin)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (search_active && (strcmp(line, "stop") == 0 || strcmp(line, "quit") == 0)) {
            stop_requested = 1;
        }
        pthread_mutex_lock(&q_lock);
        int next = (cmd_q_tail + 1) % CMD_Q_SIZE;
        if (next != cmd_q_head) {
            strcpy(cmd_queue[cmd_q_tail], line);
            cmd_q_tail = next;
        }
        pthread_cond_signal(&q_cond);
        pthread_mutex_unlock(&q_lock);
        if (strcmp(line, "quit") == 0) break;
    }
    pthread_mutex_lock(&q_lock);
    stdin_eof = 1;
    pthread_cond_signal(&q_cond);
    pthread_mutex_unlock(&q_lock);
    return NULL;
}
#endif

static int next_cmd(char *out, int maxlen) {
#ifdef _WIN32
    for (;;) {
        EnterCriticalSection(&q_lock);
        if (cmd_q_head != cmd_q_tail) {
            strncpy(out, cmd_queue[cmd_q_head], maxlen - 1);
            out[maxlen - 1] = '\0';
            cmd_q_head = (cmd_q_head + 1) % CMD_Q_SIZE;
            LeaveCriticalSection(&q_lock);
            return 1;
        }
        if (stdin_eof) { LeaveCriticalSection(&q_lock); return 0; }
        LeaveCriticalSection(&q_lock);
        WaitForSingleObject(q_sem, INFINITE);
    }
#else
    pthread_mutex_lock(&q_lock);
    while (cmd_q_head == cmd_q_tail && !stdin_eof) {
        pthread_cond_wait(&q_cond, &q_lock);
    }
    if (cmd_q_head != cmd_q_tail) {
        strncpy(out, cmd_queue[cmd_q_head], maxlen - 1);
        out[maxlen - 1] = '\0';
        cmd_q_head = (cmd_q_head + 1) % CMD_Q_SIZE;
        pthread_mutex_unlock(&q_lock);
        return 1;
    }
    pthread_mutex_unlock(&q_lock);
    return 0;
#endif
}

/* =========================================================================
 * UCI Command Handlers
 * ========================================================================= */

static void cmd_setoption(const char *line) {
    const char *np = strstr(line, "name ");
    if (!np) return;
    np += 5;
    while (*np == ' ') np++;
    const char *vp = strstr(np, " value ");
    char name[128] = {0};
    char val[128] = {0};
    if (vp) {
        size_t nlen = (size_t)(vp - np);
        if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
        memcpy(name, np, nlen);
        while (nlen > 0 && name[nlen - 1] == ' ') name[--nlen] = '\0';
        vp += 7;
        while (*vp == ' ') vp++;
        strncpy(val, vp, sizeof(val) - 1);
    } else {
        strncpy(name, np, sizeof(name) - 1);
    }

    if (ci_eq(name, "Hash")) {
        int mb = atoi(val);
        if (mb < 1) mb = 1;
        if (mb > 10) mb = 10;
        opt_hash_mb = mb;
        uint32_t units_2kb = (uint32_t)mb * 512u;
        ds_ww(0xe806, (uint16_t)(units_2kb & 0xFFFF));
        ds_ww(0xe808, (uint16_t)(units_2kb >> 16));
        call_far(LOAD_SEG, 0x63e6, 500000ULL);
    } else if (ci_eq(name, "OwnBook")) {
        opt_own_book = ci_eq(val, "true");
    } else if (ci_eq(name, "Ponder")) {
        opt_ponder = ci_eq(val, "true");
    } else if (ci_eq(name, "PlayingStyle")) {
        if (ci_eq(val, "Cautious")) opt_playing_style = 0;
        else if (ci_eq(val, "Aggressive")) opt_playing_style = 2;
        else opt_playing_style = 1;
    } else if (ci_eq(name, "RandomPlay")) {
        if (ci_eq(val, "SlightlyRandom")) opt_random_play = 1;
        else if (ci_eq(val, "Randomized")) opt_random_play = 3;
        else opt_random_play = 0;
    } else if (ci_eq(name, "EasierPlay")) {
        opt_easier_play = ci_eq(val, "true");
    } else if (ci_eq(name, "PassiveMoves")) {
        opt_passive_moves = ci_eq(val, "true");
    } else if (ci_eq(name, "PresetLevel")) {
        int lvl = atoi(val);
        if (lvl < 0) lvl = 0;
        if (lvl > 9) lvl = 9;
        opt_preset_level = lvl;
    }
    apply_engine_options();
}

static void cmd_position(const char *line) {
    const char *p = line + 8;
    while (*p == ' ') p++;
    if (strncmp(p, "startpos", 8) == 0) {
        restore_startpos();
        p += 8;
    } else if (strncmp(p, "fen ", 4) == 0) {
        p += 4;
        while (*p == ' ') p++;
        const char *mp = strstr(p, " moves ");
        char fen[256] = {0};
        if (mp) {
            size_t len = (size_t)(mp - p);
            if (len >= sizeof(fen)) len = sizeof(fen) - 1;
            memcpy(fen, p, len);
            p = mp;
        } else {
            strncpy(fen, p, sizeof(fen) - 1);
            p += strlen(p);
        }
        setup_fen_position(fen);
    }

    const char *mp = strstr(p, "moves");
    if (mp) {
        mp += 5;
        while (*mp) {
            while (*mp == ' ') mp++;
            if (!*mp) break;
            char mv[16] = {0};
            int i = 0;
            while (*mp && *mp != ' ' && i < 15) mv[i++] = *mp++;
            make_uci_move(mv);
        }
    }
}

static void cmd_go(const char *line) {
    int depth = 0;
    int movetime = 0;
    int wtime = 0, btime = 0, winc = 0, binc = 0, movestogo = 0;
    int infinite = 0;

    const char *p = line + 2;
    char tok[64];
    while (sscanf(p, "%63s", tok) == 1) {
        p = strstr(p, tok) + strlen(tok);
        if (strcmp(tok, "infinite") == 0 || strcmp(tok, "ponder") == 0) {
            infinite = 1;
        } else if (strcmp(tok, "depth") == 0 && sscanf(p, "%d", &depth) == 1) {
            p = strchr(p, ' ') ? p : p;
        } else if (strcmp(tok, "movetime") == 0 && sscanf(p, "%d", &movetime) == 1) {
        } else if (strcmp(tok, "wtime") == 0 && sscanf(p, "%d", &wtime) == 1) {
        } else if (strcmp(tok, "btime") == 0 && sscanf(p, "%d", &btime) == 1) {
        } else if (strcmp(tok, "winc") == 0 && sscanf(p, "%d", &winc) == 1) {
        } else if (strcmp(tok, "binc") == 0 && sscanf(p, "%d", &binc) == 1) {
        } else if (strcmp(tok, "movestogo") == 0 && sscanf(p, "%d", &movestogo) == 1) {
        }
    }

    apply_engine_options();
    /* Human side is opposite of current_stm so engine computes for current_stm */
    ds_wb(0x4d4c, (uint8_t)(1 - current_stm));
    ds_wb(0x4e11, 0);
    ds_wb(0xdac0, 0);

    stop_requested = 0;
    search_start_ms = get_ms();
    search_deadline_ms = 0;

    if (depth > 0) {
        ds_wb(0xd8f6, 3); /* Fixed depth */
        ds_ww(0x1952, (uint16_t)depth);
    } else if (infinite) {
        ds_wb(0xd8f6, 6); /* Infinite */
    } else if (movetime > 0) {
        ds_wb(0xd8f6, 6); /* Controlled by exact ms deadline */
        search_deadline_ms = search_start_ms + (uint64_t)movetime;
    } else if (wtime > 0 || btime > 0) {
        int my_time = (current_stm == 0) ? wtime : btime;
        int my_inc = (current_stm == 0) ? winc : binc;
        int mtg = (movestogo > 0) ? movestogo : 30;
        int budget = my_time / mtg + (my_inc * 3) / 4;
        if (budget > my_time / 2) budget = my_time / 2;
        if (budget < 50) budget = 50;
        ds_wb(0xd8f6, 6);
        search_deadline_ms = search_start_ms + (uint64_t)budget;
    } else {
        ds_wb(0xd8f6, 0); /* Preset level (opt_preset_level) */
    }

    pv_hook_enabled = 1;
    search_initial_ply = ds_rw(0xd644);
    search_active = 1;
    cpu.stop_on_idle = 1;
    cpu.idle_hit = 0;
    run_until_trap(5000000000ULL);
    cpu.stop_on_idle = 0;
    search_active = 0;
    pv_hook_enabled = 0;

    uint16_t mptr = ds_rw(0x4fbe);
    uint8_t best[3] = { 0, 0, 0 };
    if (mptr != 0) {
        best[0] = (uint8_t)(ds_rb(mptr) & 0x7F);
        best[1] = (uint8_t)(ds_rb((uint16_t)(mptr + 1)) & 0x7F);
        best[2] = ds_rb((uint16_t)(mptr + 2));
    }
    if (best[0] == 0 || best[1] == 0) {
        best[0] = (uint8_t)(ds_rb(0xd6b6) & 0x7F);
        best[1] = (uint8_t)(ds_rb(0xd6b7) & 0x7F);
        best[2] = 0;
    }
    char best_uci[8] = "0000";
    if (best[0] != 0 && best[1] != 0) {
        move3_to_uci(best, best_uci);
    }
    printf("bestmove %s\n", best_uci);
    fflush(stdout);

    /* Update current_stm since MChessPro played the move on its internal board */
    current_stm ^= 1;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);
    boot_engine();
    apply_engine_options();

#ifdef _WIN32
    InitializeCriticalSection(&q_lock);
    q_sem = CreateSemaphore(NULL, 0, CMD_Q_SIZE, NULL);
    CreateThread(NULL, 0, stdin_thread_fn, NULL, 0, NULL);
#else
    pthread_t th;
    pthread_create(&th, NULL, stdin_thread_fn, NULL);
#endif

    char line[4096];
    while (next_cmd(line, sizeof(line))) {
        if (line[0] == '\0') continue;
        if (strcmp(line, "uci") == 0) {
            printf("id name MChessProUCI 3.5\n");
            printf("id author Marty Hirsch (UCI wrapper)\n");
            printf("option name Hash type spin default 10 min 1 max 10\n");
            printf("option name OwnBook type check default true\n");
            printf("option name Ponder type check default false\n");
            printf("option name PlayingStyle type combo default Normal var Cautious var Normal var Aggressive\n");
            printf("option name RandomPlay type combo default Optimal var Optimal var SlightlyRandom var Randomized\n");
            printf("option name EasierPlay type check default false\n");
            printf("option name PassiveMoves type check default false\n");
            printf("option name PresetLevel type spin default 2 min 0 max 9\n");
            printf("uciok\n");
            fflush(stdout);
        } else if (strcmp(line, "isready") == 0) {
            printf("readyok\n");
            fflush(stdout);
        } else if (strcmp(line, "ucinewgame") == 0) {
            restore_startpos();
            call_far(LOAD_SEG, 0x71ba, 1000000ULL);
        } else if (strncmp(line, "setoption ", 10) == 0) {
            cmd_setoption(line);
        } else if (strncmp(line, "position ", 9) == 0) {
            cmd_position(line);
        } else if (strncmp(line, "go", 2) == 0 && (line[2] == '\0' || line[2] == ' ')) {
            cmd_go(line);
        } else if (strcmp(line, "quit") == 0) {
            break;
        }
    }
    return 0;
}

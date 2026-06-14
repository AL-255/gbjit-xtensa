#include "cpu_state.h"
#include "memory.h"
#include "sm83_interp.h"
#include "dispatcher.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

/* Cap the address space the emulator can claim. Bugs in JIT codegen
 * (runaway loops, leaks, fault-recovery code paths) have crashed the
 * host before; this turns "consumed all the RAM and OOM-killed
 * something important" into a clean ENOMEM/abort. 500 MB is plenty
 * for any sane workload — the JIT arena is ~64 KB, the cart ROM up to
 * 256 KB, and unit-test cycle budgets max out at ~2 GB GB-cycles
 * which produce no asymptotic memory growth. Override with
 * GBJIT_RLIMIT_MB=0 (disable) or any positive value (cap in MB). */
static void install_memory_guard(void) {
    long mb = 500;
    const char *env = getenv("GBJIT_RLIMIT_MB");
    if (env) mb = strtol(env, NULL, 10);
    if (mb <= 0) return;
    struct rlimit rl;
    rl.rlim_cur = (rlim_t)mb * 1024u * 1024u;
    rl.rlim_max = rl.rlim_cur;
    if (setrlimit(RLIMIT_AS, &rl) != 0) {
        fprintf(stderr, "warning: setrlimit(RLIMIT_AS, %ld MB) failed (running unbounded)\n", mb);
    }
}

static void serial_cb(void *ctx, u8 b) {
    (void)ctx;
    fputc(b, stdout);
    fflush(stdout);
}

static int read_file(const char *path, u8 **out, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (long)ROM_SIZE_MAX) { fclose(f); return -2; }
    u8 *buf = malloc((size_t)sz);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return -3; }
    fclose(f);
    *out = buf;
    *len = (size_t)sz;
    return 0;
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s [--interp|--jit] [--max-cycles N] <rom.gb>\n"
        "  --interp        Run with reference interpreter (default).\n"
        "  --jit           Run with host JIT (when M3 is done).\n"
        "  --max-cycles N  Stop after N T-cycles (default: 200M).\n",
        argv0);
}

/* Write the 160x144 framebuffer as a 4-shade PGM. The PPU stores raw
 * shade values 0..3; we map 0→255 (lightest) 3→0 (darkest) to match
 * conventional DMG screenshots. Caller is responsible for the file's
 * existence and writability. */
static int dump_framebuffer_pgm(const mmu *m, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P5\n160 144\n255\n");
    for (int i = 0; i < 160 * 144; i++) {
        u8 shade = m->framebuffer[i] & 3;
        u8 px = (u8)(255 - (shade * 85));   /* 0→255, 1→170, 2→85, 3→0 */
        fputc(px, f);
    }
    fclose(f);
    return 0;
}

/* Dump WRAM ($C000..$DFFF) as 8192 raw bytes for host-vs-device byte diffing. */
static int dump_wram_bin(const mmu *m, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    for (int a = 0xC000; a <= 0xDFFF; a++) fputc(mmu_read8((mmu *)m, (u16)a), f);
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    install_memory_guard();
    bool use_jit = false;
    bool no_cache = false;
    bool no_prefetch = false;
    u64 max_cycles = 200000000ull;
    const char *rom_path = NULL;
    const char *dump_path = NULL;
    u64 dump_at_cycles = 0;
    const char *wram_path = NULL;
    u64 wram_at_cycles = 0;
    bool wram_dumped = false;
    /* Scripted joypad presses: set m.buttons=mask once cpu.cycles reaches cyc.
       Bit layout matches mmu_read8's JOYP: 0=A 1=B 2=Select 3=Start
       4=Right 5=Left 6=Up 7=Down. Repeatable: `--press <cyc> <hexmask>`. */
    struct { u64 cyc; u8 mask; } presses[64];
    int n_press = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--interp") == 0) use_jit = false;
        else if (strcmp(argv[i], "--jit") == 0) use_jit = true;
        else if (strcmp(argv[i], "--no-cache") == 0) { use_jit = true; no_cache = true; }
        else if (strcmp(argv[i], "--no-prefetch") == 0) { use_jit = true; no_prefetch = true; }
        else if (strcmp(argv[i], "--press") == 0 && i + 2 < argc && n_press < 64) {
            presses[n_press].cyc  = strtoull(argv[++i], NULL, 0);
            presses[n_press].mask = (u8)strtoul(argv[++i], NULL, 0);
            n_press++;
        } else if (strcmp(argv[i], "--max-cycles") == 0 && i + 1 < argc) {
            max_cycles = strtoull(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--dump-fb") == 0 && i + 2 < argc) {
            /* `--dump-fb <cycles> <file.pgm>`: run until cycles, then
             * write the 160x144 framebuffer as a P5 PGM. Used by the
             * a/b comparison pipeline against a golden emulator. */
            dump_at_cycles = strtoull(argv[++i], NULL, 0);
            dump_path = argv[++i];
            if (max_cycles < dump_at_cycles) max_cycles = dump_at_cycles;
        } else if (strcmp(argv[i], "--dump-wram") == 0 && i + 2 < argc) {
            wram_at_cycles = strtoull(argv[++i], NULL, 0);
            wram_path = argv[++i];
            if (max_cycles < wram_at_cycles) max_cycles = wram_at_cycles;
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
            return 1;
        } else {
            rom_path = argv[i];
        }
    }
    if (!rom_path) { usage(argv[0]); return 1; }

    u8 *rom = NULL;
    size_t rom_len = 0;
    if (read_file(rom_path, &rom, &rom_len) != 0) {
        fprintf(stderr, "failed to read %s\n", rom_path);
        return 2;
    }

    static mmu m;
    gb_mmu_init(&m);
    if (!mmu_load_rom(&m, rom, rom_len)) {
        fprintf(stderr, "rom too large or invalid\n");
        return 3;
    }
    m.serial_sink = serial_cb;
    free(rom);

    cpu_state cpu;
    cpu_reset(&cpu, &m);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    /* Sort presses ascending and make sure max_cycles covers every event. */
    for (int a = 1; a < n_press; a++) {
        u64 c = presses[a].cyc; u8 mk = presses[a].mask; int b = a;
        while (b > 0 && presses[b-1].cyc > c) { presses[b]=presses[b-1]; b--; }
        presses[b].cyc = c; presses[b].mask = mk;
    }
    for (int a = 0; a < n_press; a++) if (presses[a].cyc > max_cycles) max_cycles = presses[a].cyc;

    gbjit_dispatcher disp;
    if (use_jit) {
        if (!gbjit_dispatcher_init(&disp, &cpu) ) {
            fprintf(stderr, "JIT init failed\n");
            return 4;
        }
        disp.no_cache = no_cache;
        if (no_prefetch) disp.prefetch_enabled = false;
    }

    /* Unified segmented run: advance to each event cycle (a scripted press or
     * the framebuffer dump), apply it, and continue — same path for interp
     * and jit so an injected Start exercises identical code. */
    int pi = 0; bool dumped = false;
    while (cpu.cycles < max_cycles) {
        u64 next = max_cycles;
        if (pi < n_press && presses[pi].cyc < next) next = presses[pi].cyc;
        if (dump_path && !dumped && dump_at_cycles < next) next = dump_at_cycles;
        if (wram_path && !wram_dumped && wram_at_cycles < next) next = wram_at_cycles;
        if (next <= cpu.cycles) next = cpu.cycles + 1;
        if (use_jit) gbjit_dispatcher_run_until(&disp, next);
        else         sm83_run_until(&cpu, next);

        while (pi < n_press && cpu.cycles >= presses[pi].cyc) {
            m.buttons = presses[pi].mask;
            fprintf(stderr, "[press] buttons=%02X at cycles=%llu\n",
                    presses[pi].mask, (unsigned long long)cpu.cycles);
            pi++;
        }
        if (wram_path && !wram_dumped && cpu.cycles >= wram_at_cycles) {
            dump_wram_bin(&m, wram_path);
            fprintf(stderr, "[dump-wram] %s at cycles=%llu pc=%04X\n",
                    wram_path, (unsigned long long)cpu.cycles, cpu.pc);
            wram_dumped = true;
        }
        if (dump_path && !dumped && cpu.cycles >= dump_at_cycles) {
            if (dump_framebuffer_pgm(&m, dump_path) == 0)
                fprintf(stderr, "[dump-fb] %s at cycles=%llu pc=%04X\n",
                        dump_path, (unsigned long long)cpu.cycles, cpu.pc);
            else
                fprintf(stderr, "[dump-fb] failed to write %s\n", dump_path);
            dumped = true;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed_us = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;
    double mhz = (double)cpu.cycles / elapsed_us;
    double dmg_ratio = mhz / 4.194304;

    const char *mode_label = !use_jit ? "interp"
                            : no_cache ? "JIT(no_cache)"
                            : no_prefetch ? "JIT(no_prefetch)"
                            : "JIT";
    fprintf(stderr, "\n--- %s halted at PC=%04X cycles=%llu elapsed=%.0f us throughput=%.2f MHz (%.2fx DMG)",
            mode_label,
            cpu.pc, (unsigned long long)cpu.cycles, elapsed_us, mhz, dmg_ratio);
    if (use_jit) {
        fprintf(stderr,
                " blocks=%llu/%llu chain=%llu/%llu prefetch=%llu(+%llu_cached)",
                (unsigned long long)disp.blocks_compiled,
                (unsigned long long)disp.blocks_executed,
                (unsigned long long)disp.chain_hits,
                (unsigned long long)disp.chain_misses,
                (unsigned long long)disp.prefetched_blocks,
                (unsigned long long)disp.prefetch_already_cached);
        gbjit_dispatcher_shutdown(&disp);
    }
    fprintf(stderr, " ---\n");
    return 0;
}

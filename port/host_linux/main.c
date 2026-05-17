#include "cpu_state.h"
#include "memory.h"
#include "sm83_interp.h"
#include "dispatcher.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    if (sz <= 0 || sz > (long)ROM_SIZE) { fclose(f); return -2; }
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

int main(int argc, char **argv) {
    bool use_jit = false;
    bool no_cache = false;
    u64 max_cycles = 200000000ull;
    const char *rom_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--interp") == 0) use_jit = false;
        else if (strcmp(argv[i], "--jit") == 0) use_jit = true;
        else if (strcmp(argv[i], "--no-cache") == 0) { use_jit = true; no_cache = true; }
        else if (strcmp(argv[i], "--max-cycles") == 0 && i + 1 < argc) {
            max_cycles = strtoull(argv[++i], NULL, 0);
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
    mmu_init(&m);
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

    gbjit_dispatcher disp;
    if (use_jit) {
        if (!gbjit_dispatcher_init(&disp, &cpu) ) {
            fprintf(stderr, "JIT init failed\n");
            return 4;
        }
        disp.no_cache = no_cache;
        gbjit_dispatcher_run_until(&disp, max_cycles);
    } else {
        sm83_run_until(&cpu, max_cycles);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed_us = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;
    double mhz = (double)cpu.cycles / elapsed_us;
    double dmg_ratio = mhz / 4.194304;

    const char *mode_label = !use_jit ? "interp"
                            : no_cache ? "JIT(no_cache)"
                            : "JIT";
    fprintf(stderr, "\n--- %s halted at PC=%04X cycles=%llu elapsed=%.0f us throughput=%.2f MHz (%.2fx DMG)",
            mode_label,
            cpu.pc, (unsigned long long)cpu.cycles, elapsed_us, mhz, dmg_ratio);
    if (use_jit) {
        fprintf(stderr, " blocks=%llu/%llu chain=%llu/%llu",
                (unsigned long long)disp.blocks_compiled,
                (unsigned long long)disp.blocks_executed,
                (unsigned long long)disp.chain_hits,
                (unsigned long long)disp.chain_misses);
        gbjit_dispatcher_shutdown(&disp);
    }
    fprintf(stderr, " ---\n");
    return 0;
}

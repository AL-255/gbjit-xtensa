/* peanut_gb-based golden-reference framebuffer dump.
 *
 * Build against deltabeard/Peanut-GB (vendored single-header expected
 * at tools/third_party/peanut_gb.h or fetched at build time). Runs a
 * ROM for N frames and writes the final scanline buffer as a P5 PGM
 * for byte-level comparison against gbjit_host --dump-fb.
 *
 * Usage:
 *   peanut_dump <rom> <frames_to_run> <out.pgm>
 *
 * peanut_gb counts frames, not GB cycles, so the caller picks N to
 * line up with the gbjit_host cycle-mark they want to compare. SML at
 * ~70 224 cycles/frame means 1 M cycles ≈ 14 frames, 50 M ≈ 712,
 * 100 M ≈ 1424, 200 M ≈ 2849. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* peanut_gb expects ENABLE_LCD=1 + ENABLE_SOUND=0 for headless render. */
#define ENABLE_LCD   1
#define ENABLE_SOUND 0
#define PEANUT_GB_HIGH_LCD_ACCURACY 1
#include "peanut_gb.h"

struct priv {
    uint8_t *rom;
    size_t   rom_size;
    /* peanut_gb only emits the LCD frame via per-line callbacks. Stash
     * the latest 160-pixel line into here so the final frame is
     * captured by the time gb_run_frame returns. */
    uint8_t  fb[160 * 144];
};

static uint8_t rom_read(struct gb_s *gb, const uint_fast32_t addr) {
    struct priv *p = (struct priv *)gb->direct.priv;
    if (addr < p->rom_size) return p->rom[addr];
    return 0xFF;
}
static uint8_t cart_ram_read(struct gb_s *gb, const uint_fast32_t addr) {
    (void)gb; (void)addr; return 0xFF;
}
static void cart_ram_write(struct gb_s *gb, const uint_fast32_t addr,
                            const uint8_t v) {
    (void)gb; (void)addr; (void)v;
}
static void gb_error(struct gb_s *gb, const enum gb_error_e err,
                      const uint16_t addr) {
    (void)gb;
    fprintf(stderr, "peanut_gb: error %d at %04x\n", err, addr);
}
static void lcd_draw_line(struct gb_s *gb, const uint8_t *pixels,
                           const uint_fast8_t line) {
    struct priv *p = (struct priv *)gb->direct.priv;
    if (line < 144) {
        memcpy(&p->fb[line * 160], pixels, 160);
    }
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <rom> <frames> <out.pgm>\n", argv[0]);
        return 1;
    }
    const char *rom_path = argv[1];
    unsigned long frames = strtoul(argv[2], NULL, 0);
    const char *out_path = argv[3];

    FILE *f = fopen(rom_path, "rb");
    if (!f) { perror("rom"); return 2; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *rom = (uint8_t *)malloc((size_t)sz);
    if (fread(rom, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "short read of %s\n", rom_path); return 3;
    }
    fclose(f);

    struct priv priv;
    priv.rom = rom;
    priv.rom_size = (size_t)sz;
    memset(priv.fb, 0, sizeof(priv.fb));

    struct gb_s gb;
    enum gb_init_error_e err = gb_init(
        &gb, rom_read, cart_ram_read, cart_ram_write, gb_error, &priv);
    if (err != GB_INIT_NO_ERROR) {
        fprintf(stderr, "gb_init: %d\n", err); return 4;
    }
    gb_init_lcd(&gb, lcd_draw_line);

    for (unsigned long i = 0; i < frames; i++) {
        gb_run_frame(&gb);
    }

    FILE *o = fopen(out_path, "wb");
    if (!o) { perror("out"); return 5; }
    fprintf(o, "P5\n160 144\n255\n");
    /* peanut_gb pixel value: low 2 bits = palette index (0..3), and the
     * upper bits encode source layer. Our PGM expects 8-bit grayscale
     * with 0 (lightest) = 255 (raw value 0) and 3 (darkest) = 0. */
    for (int i = 0; i < 160 * 144; i++) {
        uint8_t shade = priv.fb[i] & 3;
        uint8_t px = (uint8_t)(255 - shade * 85);
        fputc(px, o);
    }
    fclose(o);
    fprintf(stderr, "wrote %s after %lu frames\n", out_path, frames);
    free(rom);
    return 0;
}

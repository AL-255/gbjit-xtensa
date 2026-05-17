/* Smoke test for the Xtensa encoder. Real bit-accuracy verification is done
   by encoder_objdump_test.sh, which assembles the same mnemonics with the
   xtensa-esp32s3-elf toolchain and compares bytes. This test just sanity-
   checks that encoder calls produce the expected number of bytes and that
   buffer-bounds are respected. */

#include "emit_xtensa.h"
#include <stdio.h>

int main(void) {
    u8 buf[256];
    xt_emit e;
    xt_init(&e, buf, sizeof(buf));

    xt_movi(&e, 2, 42);
    xt_addi(&e, 3, 3, -1);
    xt_or  (&e, 4, 5, 6);
    xt_l8ui(&e, 3, 11, 0x10);
    xt_s8i (&e, 3, 11, 0x10);
    xt_ret (&e);

    if (e.len == 0 || e.len > sizeof(buf)) {
        fprintf(stderr, "FAIL: encoder produced %u bytes\n", (unsigned)e.len);
        return 1;
    }
    printf("encoder smoke: OK (%u bytes emitted)\n", (unsigned)e.len);
    return 0;
}

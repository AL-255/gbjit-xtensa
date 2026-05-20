/* QEMU virtual-LCD streamer.
 *
 * The Espressif QEMU esp32s3 machine has no SSD1306 / I2C-OLED device
 * model, so the physical OLED path can't be exercised under QEMU. This
 * task instead streams the full 160x144 Game Boy framebuffer out of a
 * secondary UART (UART1). A host-side viewer (tools/qemu_lcd.py)
 * connects to that UART's chardev and renders the GB screen in a
 * window — a "virtual LCD" at the original DMG resolution.
 *
 * Built only when GBJIT_QEMU_LCD is defined (see main/CMakeLists.txt);
 * otherwise qemu_lcd_start() is an empty stub. */
#ifndef QEMU_LCD_H
#define QEMU_LCD_H

struct cpu_state;

/* Start the framebuffer streamer task. Pinned to Core 1, polls
 * mmu->frame_seq, and emits one framed packet per completed frame. */
void qemu_lcd_start(struct cpu_state *cpu);

#endif

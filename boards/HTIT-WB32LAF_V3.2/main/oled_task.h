#ifndef OLED_TASK_H
#define OLED_TASK_H

struct cpu_state;

/* Start the OLED display task. Pinned to Core 0 (Core 1 is busy with
 * ppu_thread). Polls the PPU's frame_seq counter, crops a 128x64 window
 * out of the centre of the 160x144 framebuffer, thresholds to 1bpp and
 * pushes via I2C. */
void oled_task_start(struct cpu_state *cpu);

#endif

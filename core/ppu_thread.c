/* See ppu_thread.h. */

#include "ppu_thread.h"

#ifndef GBJIT_PPU_ASYNC

void ppu_thread_start(struct cpu_state *cpu) { (void)cpu; }
void ppu_thread_stop(void) {}

#else

#include "ppu.h"
#include "cpu_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static TaskHandle_t s_ppu_task = NULL;
static volatile bool s_ppu_run = false;

/* Core-1 task body. Spins on ppu_tick. Most calls early-return inside
 * ppu_tick (cycles haven't crossed ppu_next_event_cycles and LCDC hasn't
 * changed), so the cost per spin is a few loads + a compare.
 *
 * Pacing: we taskYIELD() each iteration. With this task pinned to Core 1
 * and no other Core-1 work in the firmware, the FreeRTOS idle task is
 * the only competitor and it'll get scheduled long enough to feed the
 * watchdog. If a future workload needs Core 1 too, swap the yield for a
 * notification-driven wake (Core 0 notifies on block-exit cycle sync). */
static void ppu_task(void *arg) {
    struct cpu_state *cpu = (struct cpu_state *)arg;
    while (s_ppu_run) {
        ppu_tick(cpu);
        taskYIELD();
    }
    vTaskDelete(NULL);
}

void ppu_thread_start(struct cpu_state *cpu) {
    if (s_ppu_task) return;
    s_ppu_run = true;
    /* Stack: ppu_tick is shallow (no recursion, leaf-ish), 4 KB is
     * comfortable. Priority: above idle so it actually runs; below the
     * default app_main priority so a runaway PPU loop can't lock out
     * the CPU side (which is on the other core anyway, but kept low
     * defensively). */
    BaseType_t ok = xTaskCreatePinnedToCore(
        ppu_task, "gbjit_ppu", 4096, cpu,
        tskIDLE_PRIORITY + 1, &s_ppu_task,
        1 /* Core 1 */);
    if (ok != pdPASS) {
        s_ppu_task = NULL;
        ESP_LOGE("gbjit_ppu", "xTaskCreatePinnedToCore failed (ok=%d)", (int)ok);
    }
}

void ppu_thread_stop(void) {
    if (!s_ppu_task) return;
    s_ppu_run = false;
    s_ppu_task = NULL;
}

#endif /* GBJIT_PPU_ASYNC */

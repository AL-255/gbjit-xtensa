#include "codecache.h"
#include <string.h>

void codecache_init(codecache *cc, u8 *base, u32 cap) {
    cc->base = base;
    cc->used = 0;
    cc->cap = cap;
}

u8 *codecache_alloc(codecache *cc, u32 size) {
    /* 4-byte align allocations. */
    cc->used = (cc->used + 3u) & ~3u;
    if (cc->used + size > cc->cap) return NULL;
    u8 *p = cc->base + cc->used;
    cc->used += size;
    return p;
}

#if defined(ESP_PLATFORM)
/* ESP32-S3 IRAM is internal SRAM that is NOT routed through the L1 cache
 * (cache covers flash/PSRAM only). Writes via the data path are visible to
 * the instruction fetch path immediately. We still emit a memory barrier
 * so the compiler can't reorder later instruction fetches above the code-
 * writing stores. */
void codecache_finalize(codecache *cc, u8 *block, u32 size) {
    (void)cc; (void)block; (void)size;
    __sync_synchronize();
}
#else
void codecache_finalize(codecache *cc, u8 *block, u32 size) {
    (void)cc; (void)block; (void)size;
    /* Host x86_64: writes are coherent with icache. */
    __builtin___clear_cache((char *)block, (char *)(block + size));
}
#endif

void codecache_reset(codecache *cc) {
    cc->used = 0;
}

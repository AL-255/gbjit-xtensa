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
#include "esp_cache.h"
#include "esp_heap_caps.h"
void codecache_finalize(codecache *cc, u8 *block, u32 size) {
    (void)cc;
    /* Ensure stores are visible and the icache for [block,block+size) is
       invalidated so the CPU fetches the freshly-written instructions. */
    esp_cache_msync(block, size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
}
#else
void codecache_finalize(codecache *cc, u8 *block, u32 size) {
    (void)cc; (void)block; (void)size;
    /* Host x86_64: writes are coherent with icache. Just sync via the
       builtin barrier for defence-in-depth. */
    __builtin___clear_cache((char *)block, (char *)(block + size));
}
#endif

void codecache_reset(codecache *cc) {
    cc->used = 0;
}

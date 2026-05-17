#ifndef CODECACHE_H
#define CODECACHE_H

#include "gb_types.h"

/* Single-arena code cache. The host port allocates the backing memory with
   PROT_READ|PROT_WRITE|PROT_EXEC; the ESP32-S3 port uses heap_caps_malloc
   with MALLOC_CAP_EXEC|MALLOC_CAP_INTERNAL. */

typedef struct {
    u8  *base;
    u32  used;
    u32  cap;
} codecache;

void codecache_init(codecache *cc, u8 *base, u32 cap);

/* Reserve `size` bytes; returns pointer or NULL if full. The returned region
   is uninitialised; caller writes machine code then calls codecache_finalize
   to invalidate icache for that range. */
u8 *codecache_alloc(codecache *cc, u32 size);

/* Make written code visible to the icache. On hosts where the host CPU
   coherency suffices this is a no-op; on the S3 we call
   esp_cache_writeback / esp_cache_invalidate_icache. */
void codecache_finalize(codecache *cc, u8 *block, u32 size);

void codecache_reset(codecache *cc);

#endif

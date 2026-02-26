/*
 * spiram_alloc.h — redirect libtsm heap calls to PSRAM.
 *
 * Force-included (gcc -include) into every libtsm translation unit for the
 * IDF target so that libtsm's ~150 KB of internal screen/VTE allocations
 * land in PSRAM rather than scarce internal DRAM.
 *
 * Falls back to internal DRAM if PSRAM is unavailable or full, so the build
 * is safe even on boards without SPIRAM.
 *
 * NOT included for the simulator build (plain host libc malloc is fine there).
 */
#pragma once

#include <stddef.h>
#include "esp_heap_caps.h"

#undef malloc
#undef calloc
#undef realloc
#undef free

static inline void *_tsm_malloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p;
}

static inline void *_tsm_calloc(size_t n, size_t sz)
{
    void *p = heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_calloc(n, sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p;
}

/*
 * heap_caps_realloc preserves the original pointer on failure (C-standard
 * behaviour), so the two-step fallback is safe.
 */
static inline void *_tsm_realloc(void *ptr, size_t n)
{
    void *p = heap_caps_realloc(ptr, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_realloc(ptr, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p;
}

#define malloc(n)      _tsm_malloc(n)
#define calloc(n, sz)  _tsm_calloc((n), (sz))
#define realloc(p, n)  _tsm_realloc((p), (n))
#define free(p)        heap_caps_free(p)

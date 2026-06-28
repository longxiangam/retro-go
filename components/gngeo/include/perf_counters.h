#ifndef PERF_COUNTERS_H
#define PERF_COUNTERS_H

#include <stdint.h>

#ifdef ESP32_PLATFORM

/* ── Global performance counters, reset every reporting interval ── */
typedef struct {
    /* Sprite cache (ctile) */
    uint32_t spr_hits;          /* tiles found in cache */
    uint32_t spr_misses;        /* banks loaded from SD */
    int64_t  spr_sd_us;         /* total SD read time for sprite cache (us) */
    int64_t  spr_prefetch_us;   /* total time in prefetch_sprite_banks() (us) */

    /* ADPCM cache */
    uint32_t adpcm_hits;
    uint32_t adpcm_misses;

    /* IPC pool */
    uint32_t ipc_lookups;       /* ipclist[] hash lookups */
    uint32_t ipc_misses;        /* ipclist[] compiles (new IPC blocks) */
    uint32_t ipc_flushes;       /* pool full resets */

    /* Frame timing */
    int64_t  frame_max_us;      /* worst-case frame time */
    int64_t  frame_min_us;      /* best-case frame time */
} perf_counters_t;

extern perf_counters_t g_perf;

static inline void perf_reset(void) {
    g_perf.spr_hits = 0;
    g_perf.spr_misses = 0;
    g_perf.spr_sd_us = 0;
    g_perf.spr_prefetch_us = 0;
    g_perf.adpcm_hits = 0;
    g_perf.adpcm_misses = 0;
    g_perf.ipc_lookups = 0;
    g_perf.ipc_misses = 0;
    g_perf.ipc_flushes = 0;
    g_perf.frame_max_us = 0;
    g_perf.frame_min_us = 999999;
}

#endif /* ESP32_PLATFORM */
#endif /* PERF_COUNTERS_H */

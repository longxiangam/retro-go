#include "adpcm_cache.h"
#include <stdlib.h>
#include <string.h>

int adpcm_cache_init(adpcm_cache_t *c, FILE *file, uint32_t file_size, uint32_t cache_bytes) {
    memset(c, 0, sizeof(*c));

    c->num_slots = cache_bytes / ADPCM_PAGE_SIZE;
    if (c->num_slots < 16) return -1;

    c->data = malloc(c->num_slots * ADPCM_PAGE_SIZE);
    if (!c->data) return -1;

    c->slot_tag = malloc(c->num_slots * sizeof(int));
    if (!c->slot_tag) {
        free(c->data);
        c->data = NULL;
        return -1;
    }

    for (uint32_t i = 0; i < c->num_slots; i++)
        c->slot_tag[i] = -1;
    c->last_page = -1;

    c->file = file;
    c->file_size = file_size;
    c->active = 1;

    printf("ADPCM cache: %u KB, %u pages (ROM: %u KB)\n",
           cache_bytes / 1024, c->num_slots, file_size / 1024);
    return 0;
}

void adpcm_cache_free(adpcm_cache_t *c) {
    if (c->data) { free(c->data); c->data = NULL; }
    if (c->slot_tag) { free(c->slot_tag); c->slot_tag = NULL; }
    if (c->file) { fclose(c->file); c->file = NULL; }
    c->active = 0;
}

uint8_t adpcm_cache_miss(adpcm_cache_t *c, uint32_t addr, uint32_t page_num, uint32_t slot) {
    uint32_t page_off = addr & ADPCM_PAGE_MASK;

    /* Sequential-stream read-ahead: sample playback walks pages in order,
     * and each SD command's latency (~2.5 ms) dominates over its data size.
     * Batching 4 pages into one command cuts burst-stall cost ~4x. */
    int ahead = (c->last_page >= 0 && page_num == (uint32_t)(c->last_page + 1)) ? 4 : 1;
    c->last_page = (int)page_num;

    for (int k = 0; k < ahead; k++) {
        uint32_t pg = page_num + (uint32_t)k;
        uint32_t off = pg << ADPCM_PAGE_SHIFT;
        if (off >= c->file_size)
            break;

        uint32_t read_size = ADPCM_PAGE_SIZE;
        if (off + read_size > c->file_size)
            read_size = c->file_size - off;

        uint32_t sl = pg % c->num_slots;
        uint8_t *dst = c->data + sl * ADPCM_PAGE_SIZE;

        /* Clear the slot first (handles partial reads at end of file) */
        if (read_size < ADPCM_PAGE_SIZE)
            memset(dst, 0, ADPCM_PAGE_SIZE);

        fseek(c->file, off, SEEK_SET);
        fread(dst, 1, read_size, c->file);
        c->slot_tag[sl] = (int)pg;
    }

    return c->data[slot * ADPCM_PAGE_SIZE + page_off];
}

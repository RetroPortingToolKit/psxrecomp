/* card_read_summary.c — non-evicting first-32 completed card reads. */

#include "card_read_summary.h"
#include "psx_cycles.h"
#include <string.h>

extern uint32_t g_debug_current_func_addr;
extern uint32_t g_debug_last_store_pc;

static CardReadSummary s_ring[CARD_READ_SUMMARY_CAP];
static uint32_t        s_count = 0;
static uint64_t        s_seq   = 0;

void card_read_summary_record(uint8_t slot, uint8_t cmd, uint16_t sector,
                              uint8_t checksum, uint8_t data_idx,
                              const uint8_t *data128) {
    if (s_count >= CARD_READ_SUMMARY_CAP) return;
    CardReadSummary *e = &s_ring[s_count];
    memset(e, 0, sizeof(*e));
    e->seq             = s_seq++;
    e->psx_cycle_count = psx_get_cycle_count();
    e->current_func    = g_debug_current_func_addr;
    e->last_store_pc   = g_debug_last_store_pc;
    e->slot            = slot;
    e->cmd             = cmd;
    e->sector          = sector;
    e->checksum_card   = checksum;
    e->data_idx_at_end = data_idx;
    e->dest_ram_addr   = 0;  /* filled by chain trace */
    if (data128) {
        memcpy(e->data_peek, data128, CARD_READ_SUMMARY_PEEK);
    }
    s_count++;
}

uint32_t card_read_summary_get(const CardReadSummary **buf_out) {
    if (buf_out) *buf_out = s_ring;
    return s_count;
}

void card_read_summary_reset(void) {
    memset(s_ring, 0, sizeof(s_ring));
    s_count = 0;
    s_seq   = 0;
}

void card_read_summary_set_dest(uint32_t dest_addr) {
    if (s_count == 0) return;
    /* Set on the most recent record (the one that just completed). */
    s_ring[s_count - 1].dest_ram_addr = dest_addr;
}

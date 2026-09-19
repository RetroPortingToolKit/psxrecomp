/* card_data_writes.c — capture BIOS RAM destination of card data bytes. */

#include "card_data_writes.h"
#include "psx_cycles.h"
#include <string.h>

extern uint32_t g_debug_current_func_addr;
extern uint32_t g_debug_last_store_pc;

/* "Armed" byte from the most recent SIO_RX_DATA read inside a card
 * data phase. The next matching RAM write captures it. */
static volatile int      s_armed = 0;
static uint8_t           s_armed_value = 0;
static uint16_t          s_armed_state = 0;
static uint8_t           s_armed_idx   = 0;
static uint8_t           s_armed_slot  = 0;

/* Ring (rolling: keeps last CAP entries; total_seq lets clients see
 * how many were dropped). */
static CardDataWriteEntry s_ring[CARD_DATA_WRITES_CAP];
static uint64_t           s_seq  = 0;
static uint32_t           s_head = 0;

void card_data_writes_arm(uint8_t value, uint16_t mc_state,
                          uint8_t mc_data_idx, uint8_t slot) {
    s_armed_value = value;
    s_armed_state = mc_state;
    s_armed_idx   = mc_data_idx;
    s_armed_slot  = slot;
    s_armed = 1;
}

int card_data_writes_check(uint32_t phys, uint32_t value, uint8_t width) {
    if (!s_armed) return 0;
    /* Match on the byte value zero-extended. For width==1, compare bottom
     * byte. For wider, compare bottom byte of stored value. */
    uint8_t low = (uint8_t)(value & 0xFF);
    if (low != s_armed_value) return 0;

    CardDataWriteEntry *e = &s_ring[s_head];
    e->seq                  = s_seq++;
    e->psx_cycle_count      = psx_get_cycle_count();
    e->addr                 = phys;
    e->store_pc             = g_debug_last_store_pc;
    e->func_addr            = g_debug_current_func_addr;
    e->value                = value;
    e->mc_state_at_read     = s_armed_state;
    e->mc_data_idx_at_read  = s_armed_idx;
    e->width                = width;
    e->slot                 = s_armed_slot;
    s_head = (s_head + 1) % CARD_DATA_WRITES_CAP;

    s_armed = 0;
    return 1;
}

uint32_t card_data_writes_get(const CardDataWriteEntry **buf_out,
                              uint64_t *total_seq_out,
                              uint32_t *head_out) {
    if (buf_out)       *buf_out       = s_ring;
    if (total_seq_out) *total_seq_out = s_seq;
    if (head_out)      *head_out      = s_head;
    uint32_t avail = (s_seq < CARD_DATA_WRITES_CAP)
                     ? (uint32_t)s_seq : CARD_DATA_WRITES_CAP;
    return avail;
}

void card_data_writes_reset(void) {
    memset(s_ring, 0, sizeof(s_ring));
    s_seq = 0;
    s_head = 0;
    s_armed = 0;
}

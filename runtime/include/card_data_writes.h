/*
 * card_data_writes.h — always-on capture of where the BIOS stores
 * incoming SIO data bytes during memory-card READ_DATA.
 *
 * The chain handler reads each byte from 0x1F801040 (SIO_RX_DATA) and
 * stores it into a destination buffer in RAM. We capture (cycle, addr,
 * value, mc_state, mc_data_idx, store_pc, func_addr) for every byte
 * that lands in RAM during a card data phase.
 *
 * Mechanism:
 *  - sio_read(0x1F801040) sets a "pending" flag when mc_state is in
 *    READ_DATA..READ_END and active_device==DEV_MEMCARD.
 *  - The next byte/halfword/word RAM write whose new_val matches the
 *    armed RX value is captured into the ring; the flag is cleared.
 *  - Non-matching writes between read and store don't disarm us.
 *
 * Ring is non-evicting up to CARD_DATA_WRITES_CAP, then keeps a
 * rolling tail (so we never lose the LATEST writes). For first-32-
 * read audit, this gives ~32*128=4096 byte writes — enough to see
 * the full destination buffer for several consecutive reads.
 */
#ifndef PSXRECOMP_CARD_DATA_WRITES_H
#define PSXRECOMP_CARD_DATA_WRITES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CARD_DATA_WRITES_CAP 4096

typedef struct {
    uint64_t seq;                 /* monotonic across all entries */
    uint64_t psx_cycle_count;
    uint32_t addr;                /* dest physical RAM address */
    uint32_t store_pc;            /* g_debug_last_store_pc when stored */
    uint32_t func_addr;           /* g_debug_current_func_addr */
    uint32_t value;               /* value written (zero-extended) */
    uint16_t mc_state_at_read;    /* mc_state at the SIO read */
    uint8_t  mc_data_idx_at_read; /* mc_data_idx at the SIO read (0..127) */
    uint8_t  width;               /* 1, 2, or 4 */
    uint8_t  slot;                /* mc_slot at read */
    uint8_t  pad0;
    uint8_t  pad1;
    uint8_t  pad2;
} CardDataWriteEntry;

/* Called from sio_read(0x1F801040) inside the MC_READ_DATA..READ_END
 * window. Records the byte we just delivered to BIOS so the next
 * matching RAM write can be tagged. */
void card_data_writes_arm(uint8_t value, uint16_t mc_state,
                          uint8_t mc_data_idx, uint8_t slot);

/* Called from psx_write_byte/half/word. If armed and value matches
 * the armed RX byte, record + disarm. Returns 1 if captured, 0
 * otherwise. */
int  card_data_writes_check(uint32_t phys, uint32_t value, uint8_t width);

uint32_t card_data_writes_get(const CardDataWriteEntry **buf_out,
                              uint64_t *total_seq_out,
                              uint32_t *head_out);

void card_data_writes_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_CARD_DATA_WRITES_H */

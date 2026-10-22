/*
 * card_read_summary.h — non-evicting evidence ring for the first 32
 * completed memory-card reads.
 *
 * The card-txn ring (256 entries, evicting) covers recent activity
 * but the long history is gone by the time we audit. This ring is
 * fixed-size and STOPS recording after 32 entries, so the FIRST 32
 * successful reads post-CROSS are preserved indefinitely.
 *
 * Each entry captures the SIO-layer evidence at the MC_READ_END
 * transition: slot/cmd/sector/checksum/first 16 bytes/cycle counts/
 * caller function. The destination RAM pointer is filled in later
 * by the chain-handler trace (Phase 3).
 */
#ifndef PSXRECOMP_CARD_READ_SUMMARY_H
#define PSXRECOMP_CARD_READ_SUMMARY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CARD_READ_SUMMARY_CAP   32
#define CARD_READ_SUMMARY_PEEK  16   /* bytes captured from start of buffer */

typedef struct {
    uint64_t seq;                 /* monotonic; first record = 0 */
    uint64_t psx_cycle_count;     /* guest cycle clock at completion */
    uint32_t current_func;        /* g_debug_current_func_addr at MC_READ_END */
    uint32_t last_store_pc;       /* g_debug_last_store_pc at MC_READ_END */
    uint16_t sector;
    uint8_t  slot;
    uint8_t  cmd;                 /* mc_cmd at completion (e.g. 0x52) */
    uint8_t  checksum_card;       /* simulated card's computed checksum */
    uint8_t  data_idx_at_end;     /* should be 128 for clean READ_END */
    uint8_t  pad0;
    uint8_t  pad1;
    uint32_t dest_ram_addr;       /* filled by chain-handler trace; 0 if unknown */
    uint8_t  data_peek[CARD_READ_SUMMARY_PEEK];
} CardReadSummary;

void card_read_summary_record(uint8_t slot, uint8_t cmd, uint16_t sector,
                              uint8_t checksum, uint8_t data_idx,
                              const uint8_t *data128);

/* Read access for debug_server. Returns count of recorded entries. */
uint32_t card_read_summary_get(const CardReadSummary **buf_out);

/* Reset the ring (for clean audit re-runs). */
void card_read_summary_reset(void);

/* Late-fill the destination RAM pointer for the most recent record.
 * Called by chain-handler trace once the BIOS commits the byte to RAM. */
void card_read_summary_set_dest(uint32_t dest_addr);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_CARD_READ_SUMMARY_H */

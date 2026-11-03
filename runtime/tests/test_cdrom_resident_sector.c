/* Actual controller integration: original FIFO/IRQ/DMA/snapshots, no guest
 * callback or controller-state stubs. The backing disc alone is synthetic. */
#define main fifo_baseline_main
#include "test_cdrom_sector_fifo.c"
#undef main

static uint8_t resident_data[SECTOR_SIZE];

static void setup_resident(uint8_t mode) {
    setup();
    mode_reg = mode;
    reading = 0;
    exec_command(0x06);
    CHECK(reading && irq_flag == CDIRQ_ACK);
    CHECK(cdrom_read(0x1F801801) == stat_reg);
    ack();
    memset(resident_data, 100, sizeof resident_data);
}

static void consume_resident(unsigned lba, uint8_t mode) {
    int m, s, f;
    CHECK(irq_flag == CDIRQ_DATA_READY);
    CHECK(cdrom_read(0x1F801801) == stat_reg);
    ack(); request();
    if (mode == 0xA0) {
        lba_to_msf((int)lba, 150, &m, &s, &f);
        CHECK(cdrom_dma_read() == (uint32_t)(bin_to_bcd(m) |
            bin_to_bcd(s) << 8 | bin_to_bcd(f) << 16 | 2u << 24));
        CHECK(cdrom_dma_read() == 0x00080709u);
        CHECK(cdrom_dma_read() == 0x00080709u);
    }
    drain(lba, 512);
}

static void original_continuation_and_pause(void) {
    for (unsigned k = 0; k < 2; ++k) {
        uint8_t mode = k ? 0xA0 : 0x80;
        setup_resident(mode);
        const uint64_t fires = s_dataready_fires;
        CHECK(cdrom_resident_feed_sector(100, resident_data));
        CHECK(reading && read_cmd == 0x06 && last_sector_lba == 100);
        CHECK(msf_to_lba(read_min, read_sec, read_sect) == 101);
        CHECK(s_dataready_fires == fires + 1);
        CHECK(read_delay == sector_delay_cycles());
        CHECK(!cdrom_resident_feed_sector(101, resident_data));
        consume_resident(100, mode);
        /* No host transfer state is required to continue through real timing. */
        sector();
        consume_resident(101, mode);
        CHECK(msf_to_lba(read_min, read_sec, read_sect) == 102);
        exec_command(0x09);
        CHECK(!reading && irq_flag == CDIRQ_ACK && pending.pending);
        (void)cdrom_read(0x1F801801); ack();
        psx_cycle_count += 10000000;
        process_pending(0);
        CHECK(irq_flag == CDIRQ_COMPLETE && !pending.pending);
    }
}

static void repeated_one_sector_and_snapshot(void) {
    for (unsigned k = 0; k < 2; ++k) {
        uint8_t mode = k ? 0xA0 : 0x80;
        setup_resident(mode);
        for (unsigned lba = 100; lba < 112; ++lba) {
            memset(resident_data, lba, sizeof resident_data);
            CHECK(cdrom_resident_feed_sector(lba, resident_data));
            const uint32_t size = cdrom_snapshot_bytes();
            uint8_t* wire = malloc(size); CHECK(wire);
            cdrom_snapshot_write(wire);
            consume_resident(lba, mode);
            CHECK(cdrom_snapshot_read(wire, size));
            consume_resident(lba, mode);
            free(wire);
            CHECK(reading && msf_to_lba(read_min, read_sec, read_sect) == (int)lba + 1);
        }
    }
}

static void decline_without_mutation(void) {
    for (unsigned n = 0; n < 33; ++n) {
        setup_resident(0xA0);
        uint32_t lba = 100;
        const uint8_t* data = resident_data;
        switch (n) {
        case 0: data = NULL; break;
        case 1: lba = 99; break;
        case 2: reading = 0; break;
        case 3: read_cmd = 0x1B; break;
        case 4: mode_reg = 0xE0; break;
        case 5: xa_stream_active = 1; break;
        case 6: cdda_playing = 1; break;
        case 7: setloc_pending = 1; break;
        case 8: irq_flag = CDIRQ_ACK; break;
        case 9: pending.pending = 1; break;
        case 10: queued_cmd.pending = 1; break;
        case 11: pending_dataready = 1; break;
        case 12: pending_present_due = psx_cycle_count + 10; break;
        case 13: param_count = 1; break;
        case 14: response_read = 0; response_count = 1; break;
        case 15: test_dma_active = 1; break;
        case 16: RB_.size = SECTOR_SIZE; RB_.pos = 4; break;
        case 17: test_raw_fail = 1; break;
        case 18: resident_data[42] ^= 1; break;
        case 19: test_raw_bad_header = 1; break;
        case 20: test_raw_bad_copy = 1; break;
        case 21: test_raw_mode = 1; break;
        case 22: test_submode = 0x0A; break; /* video */
        case 23: test_submode = 0x0C; break; /* audio */
        case 24: test_submode = 0x28; break; /* Form2 */
        case 25: test_submode = 0x48; break; /* realtime */
        case 26: test_submode = 0; break;
        case 27: lba = 1000; read_sec = 15; read_sect = 25; break;
        case 28: mode_reg = 0; break;
        case 29:
        case 30:
            RB_.size = WHOLE_SECTOR_SIZE; RB_.pos = 12 + SECTOR_SIZE;
            last_sector_mode = mode_reg; last_sector_have_raw = 1;
            last_sector_lba = n == 29 ? 98 : 99;
            if (n == 30) --RB_.pos;
            break;
        case 31: xa_data_end_pending = 1; break;
        case 32: cdda_data_end_pending = 1; break;
        }
        const uint32_t size = cdrom_snapshot_bytes();
        uint8_t *before = malloc(size), *after = malloc(size);
        CHECK(before && after);
        cdrom_snapshot_write(before);
        CHECK(!cdrom_resident_feed_sector(lba, data));
        cdrom_snapshot_write(after);
        CHECK(!memcmp(before, after, size));
        free(before); free(after);
    }
}

int main(void) {
    CHECK(fifo_baseline_main() == 0);
    original_continuation_and_pause();
    repeated_one_sector_and_snapshot();
    decline_without_mutation();
    puts("PASS: resident single-sector FIFO/IRQ, 24 snapshot restores, original continuation/Pause, 33 atomic declines");
    return 0;
}

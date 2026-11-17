/* Native PS1 Mouse wire regression. Poll ID 0x12, active-low buttons,
 * signed movement, carry, ACK edges and multitap padding. Unsupported
 * commands remain high impedance; motion saturation is a host policy. */
#include "sio.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
uint32_t sio_snapshot_bytes(void);
void sio_snapshot_write(uint8_t *p);
int sio_snapshot_read(const uint8_t *p, uint32_t len);

uint32_t i_stat = 0;
int g_psx_render_pass_active;
int memcard_last_flush_result(int card) { (void)card; return 0; }
uint32_t i_mask = 0;
uint32_t g_debug_current_func_addr = 0;
uint32_t g_debug_last_store_pc = 0;
int psx_get_in_exception(void) { return 0; }
uint8_t psx_read_byte(uint32_t addr) { (void)addr; return 0; }
uint32_t psx_read_word(uint32_t addr) { (void)addr; return 0; }

/* sio.c gates card-transfer deferral on `psx_get_cycle_count() < deadline`, so
 * a constant clock would defer forever. Advance monotonically. */
static uint64_t clock_now;
uint64_t psx_get_cycle_count(void) { return clock_now; }
uint32_t memory_get_sr(void) { return 0; }
void debug_server_poll(void) {}
void debug_server_log_sio_write(uint32_t a, uint32_t v, uint8_t w) {
    (void)a; (void)v; (void)w;
}
void starvation_ring_record(uint8_t k, uint8_t tx, uint8_t rx, uint16_t c,
                            uint16_t st, uint8_t d, uint8_t ss, uint16_t ms,
                            uint8_t mc, uint16_t sec, uint8_t di, uint32_t f) {
    (void)k; (void)tx; (void)rx; (void)c; (void)st; (void)d; (void)ss;
    (void)ms; (void)mc; (void)sec; (void)di; (void)f;
}
void card_read_summary_record(uint8_t s, uint8_t c, uint16_t sec, uint8_t e,
                              uint8_t chk, uint8_t d0, uint8_t d1, uint32_t f) {
    (void)s; (void)c; (void)sec; (void)e; (void)chk; (void)d0; (void)d1; (void)f;
}
void card_data_writes_arm(uint8_t v, uint16_t s, uint8_t i, uint8_t sl) {
    (void)v; (void)s; (void)i; (void)sl;
}
void event_ring_record_aux(uint16_t k, uint8_t d, uint32_t a) {
    (void)k; (void)d; (void)a;
}
void psx_irq_raise(uint32_t bit, uint32_t detail) {
    (void)detail; i_stat |= 1u << bit;
}
int memcard_is_present(int card) { (void)card; return 0; }
int memcard_read_sector(int card, int sector, uint8_t *buf) {
    (void)card; (void)sector; memset(buf, 0, 128); return -1;
}
int memcard_write_sector(int card, int sector, const uint8_t *buf) {
    (void)card; (void)sector; (void)buf; return -1;
}
void memcard_flush(int card) { (void)card; }

static unsigned checks;
static void check(int okay,const char *message) {
    checks++;
    if(!okay){fprintf(stderr,"FAIL: %s\n",message);exit(1);}
}
static void advance(unsigned n) { while(n--) { clock_now++;sio_advance(1); } }

/* One transaction: rx[i] is the reply to tx[i]; ack[i] says IRQ7 followed. */
static void txn(int port,const uint8_t *tx,unsigned count,uint8_t *rx,uint8_t *ack) {
    const uint32_t slot = port ? 0x2000u : 0u;
    sio_write(0x1F80104A,0);
    sio_write(0x1F80104A,0x1003u|slot);
    for (unsigned i=0;i<count;++i) {
        sio_write(0x1F801040,tx[i]); advance(1088);
        rx[i] = (uint8_t)sio_read(0x1F801040);
        advance(256);
        ack[i] = (uint8_t)((i_stat >> 7) & 1u);
        sio_write(0x1F80104A,0x1013u|slot); i_stat &= ~0x80u;
    }
    sio_write(0x1F80104A,0);
    advance(2000);
}

/* A 42h read of a 7-byte device; compares all 7 reply bytes and the /ACK
 * pattern (after bytes 1..6, not after byte 7). */
static void expect_read(int port,const uint8_t want[7],const char *what) {
    static const uint8_t tx[7] = { 0x01,0x42,0x00,0x00,0x00,0x00,0x00 };
    uint8_t rx[7], ack[7];
    txn(port,tx,7,rx,ack);
    int ok = 1;
    for (unsigned i=0;i<7;++i) {
        if (rx[i] != want[i]) ok = 0;
        if (ack[i] != (i < 6 ? 1 : 0)) ok = 0;
    }
    if (!ok) {
        fprintf(stderr,"FAIL: %s\n  got   ",what);
        for (unsigned i=0;i<7;++i) fprintf(stderr,"%02X%s ",rx[i],ack[i]?"+":"");
        fprintf(stderr,"\n  want  ");
        for (unsigned i=0;i<7;++i) fprintf(stderr,"%02X%s ",want[i],i<6?"+":"");
        fprintf(stderr,"\n  (+ = /ACK followed)\n");
        exit(1);
    }
    checks++;
}
#define EXPECT(port, what, ...) do { static const uint8_t w_[7] = { __VA_ARGS__ }; expect_read(port,w_,what); } while (0)

static void setup_bus(void) {
    sio_init();
    sio_write(0x1F801048,0xD); sio_write(0x1F80104E,0x88);
}

#ifndef SIO_DEVICE_MOUSE
/* Control trees without the native device must fail at the actual wire ID. */
int main(void) {
    setup_bus(); sio_set_pad_connected(0,1); sio_set_pad_config_capable(0,0);
    EXPECT(0,"native mouse wire ID",0xff,0x12,0x5a,0xff,0xfc,0,0);
    return 0;
}
#else
int main(void) {
    setup_bus();
    sio_set_port_device(0,SIO_DEVICE_MOUSE);
    sio_set_pad_connected(0,1);
    check(sio_get_port_device(0) == SIO_DEVICE_MOUSE,"slot 0 reports the mouse kind");
    check(sio_get_port_device(1) == SIO_DEVICE_PAD,"slot 1 stays a pad");

    /* Spec test vectors. */
    EXPECT(0,"idle mouse",                     0xFF,0x12,0x5A,0xFF,0xFC,0x00,0x00);
    sio_set_mouse_buttons(0,1,0); sio_mouse_add_motion(0,5,-3);
    EXPECT(0,"left, dX +5, dY -3",             0xFF,0x12,0x5A,0xFF,0xF4,0x05,0xFD);
    sio_set_mouse_buttons(0,0,1); sio_mouse_add_motion(0,-1,1);
    EXPECT(0,"right, dX -1, dY +1",            0xFF,0x12,0x5A,0xFF,0xF8,0xFF,0x01);
    sio_set_mouse_buttons(0,1,1); sio_mouse_add_motion(0,-128,127);
    EXPECT(0,"both, dX -128, dY +127",         0xFF,0x12,0x5A,0xFF,0xF0,0x80,0x7F);
    sio_set_mouse_buttons(0,0,0);
    EXPECT(0,"motion is consumed by the read", 0xFF,0x12,0x5A,0xFF,0xFC,0x00,0x00);

    /* Carry past one byte (host policy): +300 reads 7F 7F 2E 00. */
    sio_mouse_add_motion(0,300,-300);
    EXPECT(0,"carry read 1", 0xFF,0x12,0x5A,0xFF,0xFC,0x7F,0x80);
    EXPECT(0,"carry read 2", 0xFF,0x12,0x5A,0xFF,0xFC,0x7F,0x80);
    EXPECT(0,"carry read 3", 0xFF,0x12,0x5A,0xFF,0xFC,0x2E,0xD4);
    EXPECT(0,"carry read 4", 0xFF,0x12,0x5A,0xFF,0xFC,0x00,0x00);

    /* The pending motion is clamped to SIO_MOUSE_ACCUM_MAX. */
    sio_mouse_add_motion(0,5000,0);
    sio_mouse_add_motion(0,2147483647,0);
    {
        int total = 0;
        for (int r=0;r<16;++r) {
            static const uint8_t tx[7] = { 0x01,0x42,0x00,0x00,0x00,0x00,0x00 };
            uint8_t rx[7], ack[7];
            txn(0,tx,7,rx,ack);
            total += (int8_t)rx[5];
        }
        check(total == SIO_MOUSE_ACCUM_MAX,"pending motion is clamped to SIO_MOUSE_ACCUM_MAX");
    }
    sio_mouse_add_motion(0,40,40);
    sio_mouse_clear_motion(0);
    EXPECT(0,"clear drops pending motion", 0xFF,0x12,0x5A,0xFF,0xFC,0x00,0x00);

    /* Any other command: hi-z and no /ACK, the transfer ends. */
    {
        static const uint8_t probe[3] = { 0x01,0x43,0x00 };
        uint8_t rx[3], ack[3];
        txn(0,probe,3,rx,ack);
        check(rx[0] == 0xFF && ack[0] == 1,"address byte is acknowledged");
        check(rx[1] == 0xFF && ack[1] == 0,"43h gets hi-z and no /ACK");
        check(rx[2] == 0xFF,"nothing answers after the transfer ended");
        static const uint8_t probe45[3] = { 0x01,0x45,0x00 };
        txn(0,probe45,3,rx,ack);
        check(rx[1] == 0xFF && ack[1] == 0,"45h gets hi-z and no /ACK");
    }
    sio_mouse_add_motion(0,9,0);
    EXPECT(0,"a read after a probe still works", 0xFF,0x12,0x5A,0xFF,0xFC,0x09,0x00);

    /* Two-port use: digital pad on port 1, mouse on port 2 (Quake II style). */
    setup_bus();
    check(sio_get_port_device(0) == SIO_DEVICE_MOUSE,"the kind survives sio_init");
    sio_set_port_device(0,SIO_DEVICE_PAD);
    sio_set_port_device(1,SIO_DEVICE_MOUSE);
    sio_set_pad_connected(0,1); sio_set_pad_connected(1,1);
    sio_set_pad_config_capable(0,0);
    sio_set_pad_state_slot(0,0xFFEF);
    sio_mouse_add_motion(1,-2,4); sio_set_mouse_buttons(1,1,0);
    sio_mouse_add_motion(0,50,50);   /* ignored: slot 0 is a pad */
    sio_set_mouse_buttons(0,1,1);    /* ignored */
    {
        static const uint8_t tx[5] = { 0x01,0x42,0x00,0x00,0x00 };
        uint8_t rx[5], ack[5];
        txn(0,tx,5,rx,ack);
        check(rx[1] == 0x41 && rx[2] == 0x5A && rx[3] == 0xEF && rx[4] == 0xFF,
              "port 1 pad answers 41 5A EF FF");
        check(ack[3] == 1 && ack[4] == 0,"pad /ACK pattern is unchanged");
    }
    EXPECT(1,"port 2 mouse", 0xFF,0x12,0x5A,0xFF,0xF4,0xFE,0x04);

    /* The kind is not in the snapshot wire. */
    {
        const uint32_t with_mouse = sio_snapshot_bytes();
        sio_set_port_device(1,SIO_DEVICE_PAD);
        check(sio_snapshot_bytes() == with_mouse,"snapshot size does not depend on the kind");
    }

    /* Back to a pad: the port answers as a pad again, with no mouse residue. */
    sio_set_port_device(1,SIO_DEVICE_MOUSE);
    sio_mouse_add_motion(1,3,3);
    sio_set_port_device(1,SIO_DEVICE_PAD);
    sio_set_port_device(1,SIO_DEVICE_MOUSE);
    EXPECT(1,"switching kinds clears motion", 0xFF,0x12,0x5A,0xFF,0xFC,0x00,0x00);

    /* Disconnected mouse: nothing answers. */
    sio_set_pad_connected(1,0);
    sio_mouse_add_motion(1,7,7);
    {
        static const uint8_t tx[3] = { 0x01,0x42,0x00 };
        uint8_t rx[3], ack[3];
        txn(1,tx,3,rx,ack);
        check(rx[1] == 0xFF && ack[0] == 0,"a disconnected mouse port is silent");
    }
    sio_set_pad_connected(1,1);
    EXPECT(1,"motion waits for the next read", 0xFF,0x12,0x5A,0xFF,0xFC,0x07,0x07);

    /* Netplay canonicalization turns every seat back into a pad. */
    sio_netplay_canonicalize_session_pads(2);
    check(sio_get_port_device(1) == SIO_DEVICE_PAD,"netplay seats are pads");

#if PSX_MAX_PLAYERS >= 5
    /* Multitap on port 1, mouse on seat B: its 8-byte block in a bulk read is
     * 12 5A FF <buttons> dX dY FF FF (PSX-SPX multitap method 1). */
    setup_bus();
    sio_set_multitap_port(0); sio_set_multitap(1);
    for (int s=0;s<4;++s) {
        sio_set_port_device(s,s == 1 ? SIO_DEVICE_MOUSE : SIO_DEVICE_PAD);
        sio_set_pad_connected(s,1);
        sio_set_pad_config_capable(s,0);
    }
    sio_mouse_add_motion(1,-6,2); sio_set_mouse_buttons(1,0,1);
    {
        static const uint8_t req[5] = { 0x01,0x42,0x01,0x00,0x00 };
        uint8_t tx[35] = { 0x01,0x42,0x00 };
        uint8_t rx[35], ack[35];
        txn(0,req,5,rx,ack);
        txn(0,tx,35,rx,ack);
        check(rx[1] == 0x80 && rx[2] == 0x5A,"bulk read has the multitap ID");
        static const uint8_t seat_a[8] = { 0x41,0x5A,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
        static const uint8_t seat_b[8] = { 0x12,0x5A,0xFF,0xF8,0xFA,0x02,0xFF,0xFF };
        check(!memcmp(&rx[3],seat_a,8),"seat A is a digital pad");
        check(!memcmp(&rx[11],seat_b,8),"seat B is the mouse, padded with FF FF");
    }
    sio_set_multitap(0);
#endif

    printf("sio_mouse: %u checks passed (players=%d)\n",checks,PSX_MAX_PLAYERS);
    return 0;
}

#endif

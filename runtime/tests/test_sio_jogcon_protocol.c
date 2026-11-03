/* JogCon wire protocol regression: ID, buttons, signed steering, direction,
 * command response, and rollback state. */

#include "sio.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t sio_snapshot_bytes(void);
void sio_snapshot_write(uint8_t *p);
int sio_snapshot_read(const uint8_t *p, uint32_t len);

uint32_t i_stat = 0;
uint32_t i_mask = 0;
uint32_t g_debug_current_func_addr = 0;
uint32_t g_debug_last_store_pc = 0;
int psx_get_in_exception(void) { return 0; }
uint8_t psx_read_byte(uint32_t addr) { (void)addr; return 0; }
uint32_t psx_read_word(uint32_t addr) { (void)addr; return 0; }

/* sio.c gates card-transfer deferral on `psx_get_cycle_count() < deadline`, so
 * a constant clock would defer forever. Advance monotonically. */
uint64_t psx_get_cycle_count(void) {
    static uint64_t t;
    return t += 64;
}
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

#define SIO_TX_DATA 0x1F801040u
#define SIO_RX_DATA 0x1F801040u
#define SIO_CTRL    0x1F80104Au
#define CTRL_TX_EN      (1u << 0)
#define CTRL_SELECT     (1u << 1)
#define CTRL_ACK        (1u << 4)
#define CTRL_ACK_IRQ_EN (1u << 12)
#define CTRL_SLOT       (1u << 13)

static int failures;

#define EXPECT(label, expected, actual) do {                                  \
    unsigned e_ = (unsigned)(expected), a_ = (unsigned)(actual);              \
    if (e_ != a_) {                                                           \
        fprintf(stderr, "FAIL %s: expected=0x%X actual=0x%X\n",             \
                label, e_, a_);                                               \
        failures++;                                                           \
    }                                                                         \
} while (0)

static uint8_t xchg(int slot, uint8_t tx) {
    const uint16_t ctrl = (uint16_t)(CTRL_TX_EN | CTRL_SELECT |
        CTRL_ACK_IRQ_EN | (slot ? CTRL_SLOT : 0));
    sio_write(SIO_CTRL, ctrl);
    sio_write(SIO_TX_DATA, tx);
    sio_tick(2000);
    const uint8_t rx = (uint8_t)sio_read(SIO_RX_DATA);
    sio_write(SIO_CTRL, ctrl | CTRL_ACK);
    return rx;
}

static void finish_six_data_bytes(int slot, unsigned already_sent) {
    while (already_sent++ < 6) (void)xchg(slot, 0x00);
}

static void enter_config(int slot) {
    EXPECT("enter.prefix", 0xFF, xchg(slot, 0x01));
    EXPECT("enter.id", 0x73, xchg(slot, 0x43));
    EXPECT("enter.ack", 0x5A, xchg(slot, 0x00));
    (void)xchg(slot, 0x01);
    finish_six_data_bytes(slot, 1);
}

static void enter_jogcon_config(int slot) {
    EXPECT("jogcon.enter.prefix", 0xFF, xchg(slot, 0x01));
    EXPECT("jogcon.enter.id", 0xE3, xchg(slot, 0x43));
    EXPECT("jogcon.enter.ack", 0x5A, xchg(slot, 0x00));
    (void)xchg(slot, 0x01);
    finish_six_data_bytes(slot, 1);
}

static void set_rumble_map(int slot, const uint8_t map[6],
                           const uint8_t expected_old[6]) {
    EXPECT("map.prefix", 0xFF, xchg(slot, 0x01));
    EXPECT("map.id", 0xF3, xchg(slot, 0x4D));
    EXPECT("map.ack", 0x5A, xchg(slot, 0x00));
    for (int i = 0; i < 6; i++)
        EXPECT("map.echo", expected_old[i], xchg(slot, map[i]));
}

static void exit_config(int slot) {
    EXPECT("exit.prefix", 0xFF, xchg(slot, 0x01));
    EXPECT("exit.id", 0xF3, xchg(slot, 0x43));
    EXPECT("exit.ack", 0x5A, xchg(slot, 0x00));
    (void)xchg(slot, 0x00);
    finish_six_data_bytes(slot, 1);
}

static void jogcon_config_command(int slot, uint8_t command, uint8_t selector,
                                  const uint8_t expected_tail[4]) {
    EXPECT("jogcon.config.prefix", 0xFF, xchg(slot, 0x01));
    EXPECT("jogcon.config.id", 0xF3, xchg(slot, command));
    EXPECT("jogcon.config.status", 0x5A, xchg(slot, 0x00));
    EXPECT("jogcon.config.selector", 0x00, xchg(slot, selector));
    EXPECT("jogcon.config.data1", 0x00, xchg(slot, 0x00));
    for (int i = 0; i < 4; i++) {
        char label[48];
        snprintf(label, sizeof(label), "jogcon.config.tail%d", i);
        EXPECT(label, expected_tail[i], xchg(slot, 0x00));
    }
}

static void poll_with_motors(int slot, uint8_t small, uint8_t large) {
    EXPECT("poll.prefix", 0xFF, xchg(slot, 0x01));
    EXPECT("poll.id", 0x73, xchg(slot, 0x42));
    EXPECT("poll.ack", 0x5A, xchg(slot, 0x00));
    (void)xchg(slot, small);
    (void)xchg(slot, large);
    finish_six_data_bytes(slot, 2);
}

int main(void) {
    sio_init();
    sio_connect_pad(0);

    /* A hot-selected JogCon must be able to complete the setup command that
     * games such as R4 issue through 0x43. It begins from an explicitly
     * digital, config-inert slot to exercise the deferred host type change. */
    sio_set_pad_type(0, SIO_PAD_DIGITAL, 0x80, 0x80, 0x80, 0x80);
    sio_set_pad_config_capable(0, 0);
    sio_request_pad_type(0, SIO_PAD_JOGCON);
    EXPECT("hotplug.prefix", 0xFF, xchg(0, 0x01));
    EXPECT("hotplug.enter.id", 0xE3, xchg(0, 0x43));
    EXPECT("hotplug.enter.ack", 0x5A, xchg(0, 0x00));
    (void)xchg(0, 0x01);
    finish_six_data_bytes(0, 1);
    EXPECT("hotplug.config.prefix", 0xFF, xchg(0, 0x01));
    EXPECT("hotplug.config.poll", 0xF3, xchg(0, 0x42));
    for (int i = 0; i < 7; i++) (void)xchg(0, 0x00);
    exit_config(0);

    sio_set_pad_type(0, SIO_PAD_JOGCON, 0x80, 0x80, 0x80, 0x80);
    sio_set_pad_config_capable(0, 1);
    sio_set_pad_state_slot(0, 0xFFEFu);

    /* R4 probes JogCon configuration with data-selected 0x46/0x47/0x4C
     * replies. Both selector branches must match the device reference. */
    enter_jogcon_config(0);
    {
        static const uint8_t c46_0[4] = { 0x01, 0x02, 0x00, 0x0A };
        static const uint8_t c46_1[4] = { 0x01, 0x01, 0x01, 0x14 };
        static const uint8_t c47_0[4] = { 0x02, 0x00, 0x01, 0x00 };
        static const uint8_t c47_1[4] = { 0x00, 0x00, 0x00, 0x00 };
        static const uint8_t c4c_0[4] = { 0x00, 0x04, 0x00, 0x00 };
        static const uint8_t c4c_1[4] = { 0x03, 0x00, 0x00, 0x00 };
        jogcon_config_command(0, 0x46, 0x00, c46_0);
        jogcon_config_command(0, 0x46, 0x01, c46_1);
        jogcon_config_command(0, 0x47, 0x00, c47_0);
        jogcon_config_command(0, 0x47, 0x01, c47_1);
        jogcon_config_command(0, 0x4C, 0x00, c4c_0);
        jogcon_config_command(0, 0x4C, 0x01, c4c_1);
    }
    exit_config(0);

    EXPECT("poll.prefix", 0xFF, xchg(0, 0x01));
    EXPECT("poll.id", 0xE3, xchg(0, 0x42));
    EXPECT("poll.status", 0x5A, xchg(0, 0x00));
    EXPECT("poll.buttons.low", 0xEF, xchg(0, 0x00));
    EXPECT("poll.buttons.high", 0xFF, xchg(0, 0x00));
    EXPECT("poll.center", 0x00, xchg(0, 0x00));
    EXPECT("poll.center.sign", 0x00, xchg(0, 0x00));
    EXPECT("poll.center.direction", 0x00, xchg(0, 0x00));
    EXPECT("poll.reserved", 0x00, xchg(0, 0x00));

    sio_set_pad_type(0, SIO_PAD_DUALSHOCK, 0x80, 0x80, 0x80, 0x80);
    EXPECT("switch.dualshock.prefix", 0xFF, xchg(0, 0x01));
    EXPECT("switch.dualshock.id", 0x73, xchg(0, 0x42));
    for (int i = 0; i < 7; i++) (void)xchg(0, 0x00);
    sio_set_pad_type(0, SIO_PAD_JOGCON, 0x80, 0x80, 0x80, 0x80);
    EXPECT("switch.jogcon.prefix", 0xFF, xchg(0, 0x01));
    EXPECT("switch.jogcon.id", 0xE3, xchg(0, 0x42));
    for (int i = 0; i < 7; i++) (void)xchg(0, 0x00);

    sio_set_pad_sticks(0, 0x00, 0x80, 0x80, 0x80);
    EXPECT("left.prefix", 0xFF, xchg(0, 0x01));
    EXPECT("left.id", 0xE3, xchg(0, 0x42));
    (void)xchg(0, 0x00); (void)xchg(0, 0x00); (void)xchg(0, 0x00);
    EXPECT("left.position", 0x80, xchg(0, 0x00));
    EXPECT("left.sign", 0xFF, xchg(0, 0x00));
    EXPECT("left.direction", 0x02, xchg(0, 0x00));
    (void)xchg(0, 0x00);

    const uint32_t snapshot_len = sio_snapshot_bytes();
    /* JogCon steering[0] sits before steering[1..], the motor bytes and the
     * trailing pad-mode section (lock + device per pad). */
    const uint32_t steering_at = snapshot_len - 2u * PSX_MAX_PLAYERS -
                                 2u * PSX_MAX_PLAYERS;
    uint8_t *snapshot = (uint8_t *)malloc(snapshot_len);
    if (!snapshot) return 1;
    sio_snapshot_write(snapshot);
    EXPECT("snapshot.steering.field", 0x80, snapshot[steering_at]);

    sio_set_pad_sticks(0, 0xFF, 0x80, 0x80, 0x80);
    EXPECT("right.prefix", 0xFF, xchg(0, 0x01));
    EXPECT("right.id", 0xE3, xchg(0, 0x42));
    (void)xchg(0, 0x00); (void)xchg(0, 0x00); (void)xchg(0, 0x00);
    EXPECT("right.position", 0x7F, xchg(0, 0x00));
    EXPECT("right.sign", 0x00, xchg(0, 0x00));
    EXPECT("right.direction", 0x01, xchg(0, 0x00));
    (void)xchg(0, 0x00);

    /* Partial position stays proportional and reports movement toward center. */
    sio_set_pad_sticks(0, 0xC0, 0x80, 0x80, 0x80);
    EXPECT("partial.prefix", 0xFF, xchg(0, 0x01));
    EXPECT("partial.id", 0xE3, xchg(0, 0x42));
    (void)xchg(0, 0x00); (void)xchg(0, 0x00); (void)xchg(0, 0x00);
    EXPECT("partial.position", 0x40, xchg(0, 0x00));
    EXPECT("partial.sign", 0x00, xchg(0, 0x00));
    EXPECT("partial.direction", 0x02, xchg(0, 0x00));
    (void)xchg(0, 0x00);

    EXPECT("rollback.restore", 1, sio_snapshot_read(snapshot, snapshot_len));
    EXPECT("rollback.type", SIO_PAD_JOGCON, sio_get_pad_analog(0));
    {
        uint8_t *roundtrip = (uint8_t *)malloc(snapshot_len);
        if (roundtrip) {
            sio_snapshot_write(roundtrip);
            EXPECT("rollback.steering.field", snapshot[steering_at],
                   roundtrip[steering_at]);
            free(roundtrip);
        }
    }
    sio_set_pad_sticks(0, 0xFF, 0x80, 0x80, 0x80);
    EXPECT("rollback.prefix", 0xFF, xchg(0, 0x01));
    EXPECT("rollback.id", 0xE3, xchg(0, 0x42));
    (void)xchg(0, 0x00); (void)xchg(0, 0x00); (void)xchg(0, 0x00);
    EXPECT("rollback.position", 0x7F, xchg(0, 0x00));
    (void)xchg(0, 0x00);
    EXPECT("rollback.direction", 0x01, xchg(0, 0x00));
    (void)xchg(0, 0x00); (void)xchg(0, 0x00);
    free(snapshot);

    /* Set config mode, bind the game command into the first input byte, and
     * check that the following response reports command 2 in the upper nibble. */
    EXPECT("config.prefix", 0xFF, xchg(0, 0x01));
    EXPECT("config.enter.id", 0xE3, xchg(0, 0x43));
    EXPECT("config.enter.status", 0x5A, xchg(0, 0x00));
    (void)xchg(0, 0x01);
    finish_six_data_bytes(0, 1);
    EXPECT("map.prefix", 0xFF, xchg(0, 0x01));
    EXPECT("map.id", 0xF3, xchg(0, 0x4D));
    (void)xchg(0, 0x00);
    for (int i = 0; i < 6; i++) (void)xchg(0, i == 0 ? 0x00 : 0xFF);
    EXPECT("motor.poll.prefix", 0xFF, xchg(0, 0x01));
    EXPECT("motor.poll.id", 0xF3, xchg(0, 0x42));
    (void)xchg(0, 0x00);
    (void)xchg(0, 0x21);
    for (int i = 0; i < 5; i++) (void)xchg(0, 0x00);
    {   /* host force feedback reads command and strength */
        uint8_t cmd = 0xEE, strength = 0xEE;
        sio_get_pad_jogcon_motor(0, &cmd, &strength);
        EXPECT("motor.host.command", 0x02, cmd);
        EXPECT("motor.host.strength", 0x01, strength);
        sio_get_pad_jogcon_motor(1, &cmd, &strength);
        EXPECT("motor.host.other_slot", 0x00, cmd | strength);
    }
    EXPECT("motor.status.prefix", 0xFF, xchg(0, 0x01));
    EXPECT("motor.status.id", 0xF3, xchg(0, 0x42));
    for (int i = 0; i < 5; i++) (void)xchg(0, 0x00);
    EXPECT("motor.command", 0x20, xchg(0, 0x00));
    (void)xchg(0, 0x00);

    sio_set_pad_connected(0, 0);
    if (failures) {
        fprintf(stderr, "JogCon SIO protocol: %d failure(s)\n", failures);
        return 1;
    }
    fprintf(stderr, "JogCon SIO protocol: passed\n");
    return 0;
}

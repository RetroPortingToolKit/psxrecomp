/* Real registry/framing, with only the committed-package lookup doubled. */
#include "mod_snapshot.h"
#include "pst_wire.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int enable_a, enable_b, ready_a = 1, writer_fails, unstable_size;
static uint32_t size_calls, a_value = 42, b_value = 71, b_size = 4;
static int a_reads, b_reads, a_validations, b_validations, observed;
static const uint8_t *observed_data;
static uint32_t observed_len;

int mod_runtime_snapshot_package_active(const char *id) {
    if (!strcmp(id, "test.a")) return enable_a;
    if (!strcmp(id, "test.b")) return enable_b;
    return 0;
}
static uint32_t a_bytes(void) { return unstable_size ? ++size_calls : 4; }
static uint32_t b_bytes(void) { return b_size; }
static int write_value(uint8_t *out, uint32_t len, uint32_t value) {
    PstW w;
    if (writer_fails || len < 4) return 0;
    memset(out, 0, len);
    pst_w_init(&w, out, len);
    return pst_w_u32(&w, value);
}
static int a_write(uint8_t *out, uint32_t len) { return write_value(out, len, a_value); }
static int b_write(uint8_t *out, uint32_t len) { return write_value(out, len, b_value); }
static int a_validate(const uint8_t *p, uint32_t len) {
    ++a_validations; return p && len == 4 && p[0] != 0xff;
}
static int b_validate(const uint8_t *p, uint32_t len) {
    ++b_validations; return p && (len == 4 || len == 8) && p[0] != 0xff;
}
static void a_read(const uint8_t *p, uint32_t len) {
    PstR r; pst_r_init(&r, p, len); assert(pst_r_u32(&r, &a_value)); ++a_reads;
}
static void b_read(const uint8_t *p, uint32_t len) {
    PstR r; pst_r_init(&r, p, len); assert(pst_r_u32(&r, &b_value)); ++b_reads;
}
static int a_ready(void) { return ready_a; }
static void observe(const uint8_t *p, uint32_t len) {
    ++observed; observed_data = p; observed_len = len;
}
static void put_u32(uint8_t *p, uint32_t value) {
    PstW w; pst_w_init(&w, p, 4); assert(pst_w_u32(&w, value));
}
static uint8_t *capture(uint32_t *len) {
    uint8_t *data = NULL;
    assert(psx_mod_snapshot_capture(&data, len));
    return data;
}
static void expect_capture_failure(void) {
    uint8_t *data = (uint8_t *)(uintptr_t)1;
    uint32_t len = 99;
    assert(!psx_mod_snapshot_capture(&data, &len));
    assert(!data && !len);
}

int main(void) {
    PSXModSnapshotProvider a = {1, a_bytes, a_write, a_validate, a_read, a_ready};
    const PSXModSnapshotProvider b = {7, b_bytes, b_write, b_validate, b_read, NULL};
    uint32_t len, cookie;
    uint8_t *saved, *broken;
    assert(!psx_mod_register_snapshot_provider(NULL, &a));
    assert(!psx_mod_register_snapshot_provider("Test.a", &a));
    assert(!psx_mod_register_snapshot_provider(".test", &a));
    assert(!psx_mod_register_snapshot_provider("test.", &a));
    assert(!psx_mod_register_snapshot_provider("test.a", NULL));
    a.version = 0;
    assert(!psx_mod_register_snapshot_provider("test.a", &a));
    a.version = 1;
    a.read = NULL;
    assert(!psx_mod_register_snapshot_provider("test.a", &a));
    a.read = a_read;
    /* Reverse registration order must still yield canonical package order. */
    assert(psx_mod_register_snapshot_provider("test.b", &b));
    assert(psx_mod_register_snapshot_provider("test.a", &a));
    assert(!psx_mod_register_snapshot_provider("test.a", &a));
    memset(&a, 0, sizeof a); /* Registration copied the callbacks. */
    saved = capture(&len);
    assert(!saved && !len && !psx_mod_snapshot_layout_cookie());
    ready_a = 0;
    assert(psx_mod_snapshot_ready()); /* Disabled callback cannot block. */
    enable_a = 1;
    assert(!psx_mod_snapshot_ready());
    expect_capture_failure();
    ready_a = 1;
    enable_b = 1;
    cookie = psx_mod_snapshot_layout_cookie();
    assert(cookie);
    saved = capture(&len);
    assert(len == 60 && !memcmp(saved + 28, "test.a", 6) &&
           !memcmp(saved + 50, "test.b", 6));
    assert(a_validations == 1 && b_validations == 1);
    a_value = 0; b_value = 0;
    /* Live variable-length state can differ from the saved length. */
    b_size = 8;
    assert(psx_mod_snapshot_layout_cookie() == cookie);
    assert(psx_mod_snapshot_validate(saved, len));
    psx_mod_snapshot_read(saved, len);
    assert(a_value == 42 && b_value == 71 && a_reads == 1 && b_reads == 1);
    broken = (uint8_t *)malloc(len + 1); assert(broken);
    memcpy(broken, saved, len); broken[len] = 0;
    assert(!psx_mod_snapshot_validate(saved, len - 1));
    assert(!psx_mod_snapshot_validate(broken, len + 1));
    /* The later provider's invalid payload never causes an earlier read. */
    broken[len - 4] = 0xff;
    assert(!psx_mod_snapshot_validate(broken, len));
    assert(a_reads == 1 && b_reads == 1);
    memcpy(broken, saved, len); put_u32(broken + 38, 8); /* schema B */
    assert(!psx_mod_snapshot_validate(broken, len));
    memcpy(broken, saved, len); memcpy(broken + 50, "test.a", 6); /* duplicate */
    assert(!psx_mod_snapshot_validate(broken, len));
    memcpy(broken, saved, len); put_u32(broken + 24, 0xffffffffu); /* payload A */
    assert(!psx_mod_snapshot_validate(broken, len));
    enable_b = 0;
    assert(psx_mod_snapshot_layout_cookie() != cookie);
    assert(!psx_mod_snapshot_validate(saved, len));
    enable_b = 1;
    writer_fails = 1; expect_capture_failure(); writer_fails = 0;
    unstable_size = 1; size_calls = 0; expect_capture_failure(); unstable_size = 0;
    b_size = 0xffffffffu; expect_capture_failure(); b_size = 4;
    b_value = 0xff; expect_capture_failure(); b_value = 71;
    assert(psx_mod_register_snapshot_loaded_observer("test.a", observe));
    assert(psx_mod_register_snapshot_loaded_observer("test.b", observe));
    assert(!psx_mod_register_snapshot_loaded_observer("test.a", observe));
    psx_mod_snapshot_notify_loaded(saved, len);
    assert(observed == 1 && observed_data == saved && observed_len == len);
    enable_a = enable_b = 0;
    psx_mod_snapshot_notify_loaded(saved, len);
    assert(observed == 1);
    free(broken); free(saved);
    puts("mod snapshot registry, framing, quiescence and observer checks passed");
    return 0;
}

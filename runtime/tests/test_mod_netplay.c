#include "mod_netplay.h"
#include "netplay_state_digest.h"
#include "crc32.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Deterministic device providers: exercise the actual core CRC fold, including
 * the trusted plugin's host-state partition, without creating a game/window. */
static uint8_t ram[2048], scratchpad[1024], spu_ram[16];
static uint16_t vram[1024 * 512];
uint32_t i_stat, i_mask, g_psx_icache_tv[1024];
uint64_t psx_cycle_count;
uint8_t* memory_get_ram_ptr(void) { return ram; }
uint32_t memory_get_ram_bytes(void) { return sizeof(ram); }
uint8_t* memory_get_scratchpad_ptr(void) { return scratchpad; }
uint8_t* spu_get_ram_ptr(void) { return spu_ram; }
uint32_t spu_get_ram_bytes(void) { return sizeof(spu_ram); }
const uint16_t* gpu_get_vram(void) { return vram; }
uint32_t interrupts_get_cycles_since_vblank(void) { return 0; }
uint32_t dirty_ram_get_bitmap_word_count(void) { return 1; }
uint32_t dirty_ram_get_bitmap_word(uint32_t index) { (void)index; return 0; }
void timers_get_snapshot(uint16_t counter[3], uint32_t mode[3],
                         uint16_t target[3], int32_t irq_line[3], uint32_t frac[3]) {
    memset(counter, 0, 3 * sizeof(*counter));
    memset(mode, 0, 3 * sizeof(*mode));
    memset(target, 0, 3 * sizeof(*target));
    memset(irq_line, 0, 3 * sizeof(*irq_line));
    memset(frac, 0, 3 * sizeof(*frac));
}
#define EMPTY_SNAPSHOT(module) \
    uint32_t module##_snapshot_bytes(void) { return 0; } \
    void module##_snapshot_write(uint8_t* out) { (void)out; }
EMPTY_SNAPSHOT(cdrom)
EMPTY_SNAPSHOT(gpu)
EMPTY_SNAPSHOT(spu)
EMPTY_SNAPSHOT(mdec)
EMPTY_SNAPSHOT(dma)
EMPTY_SNAPSHOT(sio)
void sio_snapshot_section_ends(uint32_t out[5]) { memset(out, 0, 5 * sizeof(*out)); }

static uint32_t gameplay_state;
static unsigned digest_calls;
static uint32_t gameplay_digest(void) { ++digest_calls; return gameplay_state; }

int main(int argc, char** argv) {
    CPUState cpu;
    NetplayCoreParts baseline, live, changed;
    PSXModNetplayProfile invalid, duplicate;
    static PSXModNetplayProfile profile = {
        "test.simulation", "test-coop-delay-v1", 0, 0, 1, 1u, NULL, gameplay_digest
    };
    const int wide = argc > 1 && strcmp(argv[1], "--wide") == 0;
    uint32_t original_session, profile_session;
    char original_identity[65], current_identity[65];
    const char* fingerprint =
        "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
    memset(&cpu, 0, sizeof(cpu));
    cpu.pc = 0x80010000;
    netplay_core_digest_parts(&cpu, &baseline);
    assert(baseline.mod == 0);
    assert(psx_mod_netplay_rollback_supported());
    assert(psx_mod_netplay_savestates_supported());
    assert(strcmp(psx_mod_netplay_version("dev"), "dev") == 0);
    assert(psx_mod_netplay_session_id(123) == 123);
    assert(psx_mod_netplay_content_identity("", current_identity) && !current_identity[0]);
    assert(psx_mod_netplay_content_identity(fingerprint, current_identity) &&
           strcmp(fingerprint, current_identity) == 0);
    assert(!psx_mod_netplay_content_identity("invalid", current_identity));
    psx_mod_netplay_set_active(1);
    assert(!psx_mod_netplay_is_active());
    assert(!psx_mod_register_netplay_profile(NULL));
    invalid = profile;
    invalid.plugin_id = "";
    assert(!psx_mod_register_netplay_profile(&invalid));
    invalid = profile;
    invalid.compatibility_id =
        "0123456789012345678901234567890123456789012345678901234567890123";
    assert(!psx_mod_register_netplay_profile(&invalid));
    invalid = profile;
    invalid.fixed_aspect_mask = 8u;
    assert(!psx_mod_register_netplay_profile(&invalid));
    if (wide) profile.fixed_aspect_mask = 6u;
    assert(psx_mod_register_netplay_profile(&profile));
    assert(psx_mod_register_netplay_profile(&profile));
    duplicate = profile;
    assert(!psx_mod_register_netplay_profile(&duplicate));
    assert(!psx_mod_netplay_rollback_supported());
    assert(psx_mod_netplay_savestates_supported());
    assert(strcmp(psx_mod_netplay_version("override"), "test-coop-delay-v1") == 0);
    assert(psx_mod_netplay_aspect() == (wide ? 1 : 0));
    original_session = psx_mod_netplay_session_id(123);
    assert(psx_mod_netplay_content_identity("", original_identity) && strlen(original_identity) == 64);
    assert(psx_mod_netplay_content_identity("", current_identity) &&
           strcmp(original_identity, current_identity) == 0);
    assert(psx_mod_netplay_content_identity(fingerprint, current_identity) &&
           strcmp(original_identity, current_identity) != 0 && strcmp(fingerprint, current_identity) != 0);
    assert(original_session != 123);
    assert(original_session == psx_mod_netplay_session_id(123));
    assert(!psx_mod_netplay_set_aspect(-1));
    assert(!psx_mod_netplay_set_aspect(3));
    assert(!psx_mod_netplay_set_aspect(wide ? 0 : 1));
    assert(psx_mod_netplay_aspect() == (wide ? 1 : 0));
    if (wide) {
        assert(psx_mod_netplay_set_aspect(2));
        assert(original_session != psx_mod_netplay_session_id(123));
        assert(psx_mod_netplay_content_identity("", current_identity) &&
               strcmp(original_identity, current_identity) != 0);
    }
    netplay_core_digest_parts(&cpu, &live);
    assert(live.core == baseline.core && digest_calls == 0);
    gameplay_state = 0x12345678;
    psx_mod_netplay_set_active(1);
    assert(psx_mod_netplay_is_active());
    assert(!psx_mod_netplay_savestates_supported());
    assert(psx_mod_netplay_has_state_digest());
    profile_session = psx_mod_netplay_session_id(123);
    netplay_core_digest_parts(&cpu, &live);
    assert(live.mod == gameplay_state && digest_calls == 1);
    assert(live.core != baseline.core && live.ram == baseline.ram && live.cpu == baseline.cpu);
    ++gameplay_state;
    netplay_core_digest_parts(&cpu, &changed);
    assert(changed.core != live.core && changed.mod == gameplay_state);
    assert(changed.ram == live.ram && changed.cpu == live.cpu);
    gameplay_state = 0;
    netplay_core_digest_parts(&cpu, &changed);
    assert(changed.mod == 0 && changed.core != baseline.core); /* zero is present */
    psx_mod_netplay_set_active(0);
    assert(psx_mod_netplay_savestates_supported());
    assert(!psx_mod_netplay_has_state_digest());
    netplay_core_digest_parts(&cpu, &changed);
    assert(changed.core == baseline.core && digest_calls == 3);
    assert(psx_mod_netplay_session_id(123) == profile_session);
    puts("trusted netplay policy and host-state digest tests passed");
    return 0;
}

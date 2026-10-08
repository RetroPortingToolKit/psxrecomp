/* White-box policy coverage against the real user/protocol request handlers.
 * Device/serializer stubs must never be reached; no slot files are written. */
#include <assert.h>
#include "../src/savestate.c"

static int refusals;
static char refusal_reason[128];
void psx_frontend_on_savestate_refused(int load, int slot, const char* reason) {
    (void)load; (void)slot;
    ++refusals;
    snprintf(refusal_reason, sizeof(refusal_reason), "%s", reason);
}
int psx_netplay_active(void) { return 0; }
int psx_netplay_is_host(void) { return 1; }
int psx_hle_scheduler_enabled(void) { return 1; }
int psx_mod_function_entry_active(void) { return 0; }
int psx_is_dispatchable(uint32_t pc) { return pc != 0; }
uint32_t psx_last_irq_check_pc(void) { return 0; }
uint32_t psx_compiled_irq_resume_pc(void) { return 0; }
uint32_t psx_irq_resume_context_snapshot_pc(void) { return 0; }
uint32_t psx_netplay_rb_sticky_bb_pc(void) { return 0; }
int psx_irq_resume_context_snapshot_safe_at(uint32_t pc) { (void)pc; return 1; }
int psx_irq_resume_context_snapshot_site(void) { return 0; }
void interrupts_resync_after_restore(void) { assert(0); }
void psx_cycles_resync_after_restore(struct CPUState* cpu) { (void)cpu; assert(0); }
void cdrom_accelerate_after_savestate(void) { assert(0); }
void psx_scheduler_resume_at(uint32_t pc) { (void)pc; assert(0); }
void psx_frontend_on_savestate_loaded(void) { assert(0); }
void psx_frontend_on_savestate_notify(int load, int slot, int ok) {
    (void)load; (void)slot; (void)ok; assert(0);
}
void gpu_get_display_info(GpuDisplayInfo* out) { (void)out; assert(0); }
uint32_t gpu_display_pixel_argb(const GpuDisplayInfo* out, uint32_t x, uint32_t y) {
    (void)out; (void)x; (void)y; assert(0); return 0;
}
int boot_state_save(const CPUState* cpu, uint32_t bios, uint32_t entry, const char* path) {
    (void)cpu; (void)bios; (void)entry; (void)path; assert(0); return 0;
}
int boot_state_load(const char* path, uint32_t bios, uint32_t entry, CPUState* cpu) {
    (void)path; (void)bios; (void)entry; (void)cpu; assert(0); return 0;
}
int boot_state_load_buffer(const uint8_t* data, size_t size, uint32_t bios,
                            uint32_t entry, CPUState* cpu) {
    (void)data; (void)size; (void)bios; (void)entry; (void)cpu; assert(0); return 0;
}
int boot_state_save_buffer_raw(const CPUState* cpu, uint32_t bios, uint32_t entry,
                                uint8_t** data, size_t* size) {
    (void)cpu; (void)bios; (void)entry; (void)data; (void)size; assert(0); return 0;
}
int boot_state_check_buffer(const uint8_t* data, size_t size, uint32_t bios,
                             uint32_t entry, char* reason, size_t cap) {
    (void)data; (void)size; (void)bios; (void)entry; (void)reason; (void)cap;
    assert(0); return 0;
}

int main(void) {
    CPUState cpu;
    const uint8_t blob = 1;
    static const PSXModNetplayProfile profile = {
        "test.simulation", "test-no-snapshots-v1", 0, 0, 0, 1u, NULL, NULL
    };
    memset(&cpu, 0, sizeof(cpu));
    s_configured = 1; /* No configure/filesystem needed to test request staging. */
    assert(!savestate_title_blocked());
    assert(savestate_request_save(2) && savestate_pending());
    savestate_set_title_blocked(1);
    assert(savestate_title_blocked() && !savestate_pending());
    assert(savestate_take_save_failed());
    assert(!savestate_request_save(2));
    assert(!savestate_request_load(2));
    assert(!savestate_request_save_protocol(2));
    assert(!savestate_request_load_protocol(2));
    assert(!savestate_request_load_blob_protocol(&blob, 1));
    assert(refusals == 5 && strstr(refusal_reason, "game mode"));
    savestate_poll(&cpu, 0x80010000); /* No serializer or device call. */
    savestate_set_title_blocked(0);
    assert(savestate_request_load_blob_protocol(&blob, 1));
    assert(s_load_blob && s_load_blob_len == 1 && savestate_pending());
    savestate_set_title_blocked(1);
    assert(!s_load_blob && s_load_blob_len == 0 && !savestate_pending());
    assert(savestate_take_load_failed());
    savestate_set_title_blocked(0); /* The next session resets before activation. */
    assert(savestate_request_save_protocol(3));
    assert(savestate_request_load_protocol(3));
    savestate_set_title_blocked(1); /* Clear both staged operations. */
    assert(!savestate_pending());
    savestate_set_title_blocked(0);
    assert(psx_mod_register_netplay_profile(&profile));
    assert(savestate_request_save_protocol(1)); /* Registered, inactive: stock. */
    savestate_set_title_blocked(1);
    savestate_set_title_blocked(0);
    psx_mod_netplay_set_active(1);
    assert(!savestate_request_save_protocol(1));
    assert(!savestate_request_load_protocol(1));
    assert(!savestate_request_load_blob_protocol(&blob, 1));
    assert(strstr(refusal_reason, "netplay mode"));
    psx_mod_netplay_set_active(0);
    assert(savestate_request_save_protocol(1));
    savestate_set_title_blocked(1);
    assert(!savestate_pending());
    puts("savestate title policy tests passed");
    return 0;
}

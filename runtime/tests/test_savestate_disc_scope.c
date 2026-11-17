/* Save states of a game on several discs.
 *
 * A save state restores the drive's registers and buffers, not the image in
 * the drive, and its file name carries the disc it was taken on. This test
 * runs the real savestate.c against a stub serializer and a stub drive:
 *
 *   - the sequence a player runs on a multi-disc game: launch on disc 1,
 *     change to disc 2, save, relaunch on disc 1. The slot shows the disc 2
 *     state, and loading it mounts disc 2 first;
 *   - the mounted disc's own state wins over another disc's;
 *   - the load is refused, and disc 1 stays mounted, when disc 2 cannot be
 *     mounted, when the state is refused after the mount, or when its header
 *     is not this build's;
 *   - netplay, and a recorded input replay, see only the mounted disc.
 *
 *   savestate_disc_scope_test DIR   (DIR must exist; the test works in a
 *                                    subfolder and removes its own files)
 */
#include "../src/savestate.c"

#include <sys/types.h>
#if defined(_WIN32)
#include <sys/utime.h>
#define set_file_time _utime
typedef struct _utimbuf file_times;
#else
#include <utime.h>
#define set_file_time utime
typedef struct utimbuf file_times;
#endif

static int s_fails;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); s_fails++; } } while (0)

#define BIOS_SUM  0x1234ABCDu
#define ENTRY_PC  0x80010008u
#define RESUME_PC 0x80012340u
#define T_OLD 1700000000L
#define T_MID 1701000000L
#define T_NEW 1702000000L

/* ---- stub serializer ------------------------------------------------------ */

static char s_loaded_path[700];
static int  s_load_result = 1;
static int  s_header_ok = 1;
static int  s_rollbacks;

int boot_state_save(const CPUState* cpu, uint32_t bios, uint32_t entry, const char* path) {
    (void)cpu; (void)bios; (void)entry; (void)path; return 0;
}
int boot_state_load_checked(const char* path, uint32_t bios, uint32_t entry,
                            CPUState* cpu, BootStateResumeAccept accept) {
    (void)bios; (void)entry; (void)accept;
    snprintf(s_loaded_path, sizeof(s_loaded_path), "%s", path);
    if (s_load_result) cpu->pc = RESUME_PC;
    return s_load_result;
}
int boot_state_load_buffer_checked(const uint8_t* data, size_t size, uint32_t bios,
                                   uint32_t entry, CPUState* cpu, BootStateResumeAccept accept) {
    (void)data; (void)size; (void)bios; (void)entry; (void)cpu; (void)accept; return 0;
}
int boot_state_load_buffer_quiet(const uint8_t* data, size_t size, uint32_t bios,
                                 uint32_t entry, CPUState* cpu) {
    (void)data; (void)size; (void)bios; (void)entry; (void)cpu;
    s_rollbacks++;
    return 1;
}
int boot_state_save_buffer_raw(const CPUState* cpu, uint32_t bios, uint32_t entry,
                               uint8_t** data, size_t* size) {
    (void)cpu; (void)bios; (void)entry;
    *data = (uint8_t*)malloc(1);
    *size = 1;
    return *data != NULL;
}
int boot_state_check_buffer(const uint8_t* data, size_t size, uint32_t bios,
                            uint32_t entry, char* reason, size_t cap) {
    (void)data; (void)size; (void)bios; (void)entry;
    if (reason && cap) reason[0] = '\0';
    return s_header_ok;
}

/* ---- stub drive: the mounted disc and the restore mount ------------------- */

static int s_mounted = 1;
static int s_mount_open, s_mount_disc, s_mount_fail;
static int s_begin_calls, s_end_calls;

int cdrom_disc_restore_begin(int disc) {
    s_begin_calls++;
    if (s_mount_fail || s_mount_open) return 0;
    s_mount_open = 1;
    s_mount_disc = disc;
    return 1;
}
void cdrom_disc_restore_end(int keep) {
    s_end_calls++;
    if (!s_mount_open) return;
    s_mount_open = 0;
    if (keep) s_mounted = s_mount_disc;
}
void cdrom_accelerate_after_savestate(void) {}

/* ---- stub machine and frontend ------------------------------------------- */

static int s_netplay;
static int s_rewind_discards, s_menu_notes, s_loaded_notes;
static int s_notify_failed;
static char s_refusal[200];

int psx_mod_snapshot_ready(void) { return 1; }
int psx_netplay_active(void) { return s_netplay; }
int psx_netplay_is_host(void) { return 1; }
int psx_hle_scheduler_enabled(void) { return 1; }
int g_psx_guest_time_frozen;
int psx_mod_function_entry_active(void) { return 0; }
int func_override_active(void) { return 0; }
unsigned psx_snapshot_host_call_depth(void) { return 0; }
int psx_is_dispatchable(uint32_t pc) { return pc != 0; }
uint32_t psx_last_irq_check_pc(void) { return 0; }
uint32_t psx_compiled_irq_resume_pc(void) { return 0; }
uint32_t psx_irq_resume_context_snapshot_pc(void) { return 0; }
uint32_t psx_netplay_rb_sticky_bb_pc(void) { return 0; }
int psx_irq_resume_context_snapshot_safe_at(uint32_t pc) { (void)pc; return 1; }
int psx_irq_resume_context_snapshot_site(void) { return 0; }
void interrupts_resync_after_restore(void) {}
void psx_cycles_resync_after_restore(struct CPUState* cpu) { (void)cpu; }
/* The real one unwinds to the scheduler; here the poll simply returns. */
void psx_scheduler_resume_at(uint32_t pc) { (void)pc; }
uint32_t psx_rewind_discard_history(void) { s_rewind_discards++; return 0; }
void psx_savestate_menu_note_slots_changed(void) { s_menu_notes++; }
void psx_frontend_on_savestate_loaded(void) { s_loaded_notes++; }
void psx_frontend_on_savestate_notify(int load, int slot, int ok) {
    (void)slot;
    if (load && !ok) s_notify_failed++;
}
void psx_frontend_on_savestate_refused(int load, int slot, const char* reason) {
    (void)load; (void)slot;
    snprintf(s_refusal, sizeof(s_refusal), "%s", reason ? reason : "");
}
void gpu_get_display_info(GpuDisplayInfo* out) { memset(out, 0, sizeof(*out)); out->disabled = 1; }
uint32_t gpu_display_pixel_argb(const GpuDisplayInfo* di, uint32_t x, uint32_t y) {
    (void)di; (void)x; (void)y; return 0;
}
int gpu_hd_textures_residency_save(uint8_t** data, size_t* size) { (void)data; (void)size; return 0; }
int gpu_hd_textures_residency_load(const uint8_t* data, size_t size) { (void)data; (void)size; return 0; }

/* ---- helpers --------------------------------------------------------------- */

static char s_work[600];

static void state_name(int disc, int slot, const char* ext, char* out, size_t cap) {
    char token[16] = "";
    if (disc >= 1) snprintf(token, sizeof(token), "_disc%d", disc);
    snprintf(out, cap, "%s/state_%08X%s_slot%02d.%s", s_work, (unsigned)ENTRY_PC,
             token, slot, ext);
}

static void plant(int disc, int slot, long when) {
    char path[700];
    FILE* f;
    file_times t;
    state_name(disc, slot, "pst", path, sizeof(path));
    f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
    fputs("stub state", f);
    fclose(f);
    t.actime = t.modtime = (time_t)when;
    set_file_time(path, &t);
}

static void plant_thumb(int disc, int slot, uint32_t pixel) {
    char path[700];
    SavestateThumbHeader hdr;
    static uint32_t pixels[SAVESTATE_THUMB_W * SAVESTATE_THUMB_H];
    FILE* f;
    size_t i;
    state_name(disc, slot, "thumb", path, sizeof(path));
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, "PSTH", 4);
    hdr.w = SAVESTATE_THUMB_W;
    hdr.h = SAVESTATE_THUMB_H;
    for (i = 0; i < sizeof(pixels) / sizeof(pixels[0]); i++) pixels[i] = pixel;
    f = fopen(path, "wb");
    if (!f) exit(2);
    fwrite(&hdr, sizeof(hdr), 1, f);
    fwrite(pixels, sizeof(pixels), 1, f);
    fclose(f);
}

static void unplant(int disc, int slot) {
    char path[700];
    state_name(disc, slot, "pst", path, sizeof(path));
    remove(path);
    state_name(disc, slot, "thumb", path, sizeof(path));
    remove(path);
}

/* Launch on `disc` of a set of `count` discs, as main() and the drive do. */
static void launch(int disc, int count) {
    int discs[SAVESTATE_MAX_DISCS];
    int i;
    for (i = 0; i < count; i++) discs[i] = i + 1;
    savestate_set_disc_scope(count > 1 ? disc : 0);
    savestate_set_disc_roster(discs, count);
    savestate_set_cross_disc(1);
    s_mounted = disc;
    s_mount_open = 0;
    s_mount_fail = 0;
    s_netplay = 0;
    s_load_result = 1;
    s_header_ok = 1;
    s_begin_calls = s_end_calls = 0;
    s_rewind_discards = s_menu_notes = s_loaded_notes = 0;
    s_notify_failed = s_rollbacks = 0;
    s_refusal[0] = '\0';
    s_loaded_path[0] = '\0';
}

static int load(CPUState* cpu, int slot) {
    cpu->pc = 0x80010000u;
    if (!savestate_request_load(slot)) return 0;
    savestate_poll(cpu, 0x80010000u);
    return cpu->pc == RESUME_PC;
}

/* ---- tests ------------------------------------------------------------------ */

static void disc_change_then_relaunch(CPUState* cpu) {
    char path[700];
    int64_t when = 0;
    static uint32_t thumb[SAVESTATE_THUMB_W * SAVESTATE_THUMB_H];
    /* Launch on disc 1, change to disc 2 and save to slot 3 (index 2): the
     * drive re-scoped the names, so the file is disc 2's. */
    plant(2, 2, T_NEW);
    plant_thumb(2, 2, 0xFF00FF00u);
    /* Relaunch on disc 1. */
    launch(1, 2);
    CHECK(savestate_slot_disc(2) == 2);
    CHECK(savestate_slot_exists(2));
    CHECK(savestate_slot_mtime(2, &when) && when == T_NEW);
    CHECK(savestate_read_thumb(2, thumb, SAVESTATE_THUMB_W, SAVESTATE_THUMB_H) &&
          thumb[0] == 0xFF00FF00u);
    CHECK(savestate_slot_disc(5) == 0 && !savestate_slot_exists(5));

    CHECK(load(cpu, 2));
    CHECK(s_begin_calls == 1 && s_end_calls == 1 && s_mounted == 2);
    CHECK(strstr(s_loaded_path, "_disc2_slot02.pst") != NULL);
    CHECK(savestate_disc_scope() == 2);
    CHECK(s_rewind_discards == 1 && s_menu_notes == 1 && s_loaded_notes == 1);
    CHECK(s_rollbacks == 0 && !s_notify_failed);
    /* What is saved next is named for the disc now in the drive. */
    CHECK(savestate_slot_path(2, path, sizeof(path)) && strstr(path, "_disc2_slot02.pst"));
    unplant(2, 2);
}

static void own_disc_wins(CPUState* cpu) {
    plant(1, 4, T_OLD);
    plant(2, 4, T_NEW);
    launch(1, 2);
    CHECK(savestate_slot_disc(4) == 1);
    CHECK(load(cpu, 4));
    CHECK(s_begin_calls == 0 && s_mounted == 1 && savestate_disc_scope() == 1);
    CHECK(strstr(s_loaded_path, "_disc1_slot04.pst") != NULL);
    CHECK(s_rewind_discards == 0);
    unplant(1, 4);
    unplant(2, 4);
}

static void newest_other_disc(void) {
    plant(2, 5, T_OLD);
    plant(3, 5, T_MID);
    launch(1, 3);
    CHECK(savestate_slot_disc(5) == 3);
    launch(3, 3);
    CHECK(savestate_slot_disc(5) == 3);      /* disc 3's own state */
    unplant(2, 5);
    unplant(3, 5);
}

static void refusals_keep_the_disc(CPUState* cpu) {
    plant(2, 6, T_NEW);

    /* Disc 2 cannot be mounted. */
    launch(1, 2);
    s_mount_fail = 1;
    CHECK(!load(cpu, 6));
    CHECK(s_mounted == 1 && savestate_disc_scope() == 1 && s_end_calls == 0);
    CHECK(strstr(s_refusal, "needs disc 2") != NULL);
    CHECK(s_rollbacks == 1 && s_rewind_discards == 0);

    /* Disc 2 mounts, then the state is refused: disc 1 goes back. */
    launch(1, 2);
    s_load_result = 0;
    CHECK(!load(cpu, 6));
    CHECK(s_begin_calls == 1 && s_end_calls == 1 && s_mounted == 1);
    CHECK(savestate_disc_scope() == 1 && s_rewind_discards == 0);
    CHECK(s_notify_failed == 1 && s_rollbacks == 1);

    /* Not a state of this build: nothing is mounted. */
    launch(1, 2);
    s_header_ok = 0;
    CHECK(!load(cpu, 6));
    CHECK(s_begin_calls == 0 && s_mounted == 1 && s_notify_failed == 1);
    unplant(2, 6);
}

static void mounted_disc_only(void) {
    plant(2, 7, T_NEW);
    launch(1, 2);
    s_netplay = 1;
    CHECK(savestate_slot_disc(7) == 0 && !savestate_slot_exists(7));
    s_netplay = 0;
    savestate_set_cross_disc(0);               /* a recorded replay plays */
    CHECK(savestate_slot_disc(7) == 0 && !savestate_slot_exists(7));
    savestate_set_cross_disc(1);
    CHECK(savestate_slot_disc(7) == 2);
    unplant(2, 7);
}

static void single_disc_unchanged(void) {
    char path[700];
    plant(0, 8, T_NEW);
    launch(1, 1);
    CHECK(savestate_slot_disc(8) == 0);
    CHECK(savestate_slot_exists(8));
    CHECK(savestate_slot_path(8, path, sizeof(path)) &&
          strstr(path, "/state_80010008_slot08.pst"));
    unplant(0, 8);
}

int main(int argc, char** argv) {
    CPUState cpu;
    if (argc < 2) {
        fprintf(stderr, "usage: savestate_disc_scope_test DIR\n");
        return 2;
    }
    snprintf(s_work, sizeof(s_work), "%s/savestate_disc_scope", argv[1]);
    ensure_dir(s_work);
    memset(&cpu, 0, sizeof(cpu));
    /* The real configure; an empty BIOS token keeps the folder as given. */
    savestate_configure(s_work, BIOS_SUM, ENTRY_PC, NULL, 0);
    CHECK(savestate_set_content_scope(NULL));

    disc_change_then_relaunch(&cpu);
    own_disc_wins(&cpu);
    newest_other_disc();
    refusals_keep_the_disc(&cpu);
    mounted_disc_only();
    single_disc_unchanged();

    if (s_fails) {
        fprintf(stderr, "savestate_disc_scope_test: %d check(s) failed\n", s_fails);
        return 1;
    }
    puts("savestate_disc_scope_test: cross-disc slot lookup, mount, refusals and scope passed");
    return 0;
}

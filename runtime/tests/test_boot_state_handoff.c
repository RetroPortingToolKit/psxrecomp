/* Exercise the real snapshot writer/loader and handoff latch with hardware
 * doubles. In particular, restore must never run the destructive boot hook. */
#include "boot_state.c"
#include "fntrace.c"
#include <assert.h>

static uint8_t ram[2*1024*1024], spad[1024], spuram[512*1024];
static uint16_t vram[1024*512];
static int boot_clears, cd_handoffs, device_restores;
#ifndef PSX_TEST_SNAPSHOT_PACKAGE_LOOKUP
int mod_runtime_snapshot_package_active(const char *id) { (void)id; return 0; }
#endif
uint32_t i_stat, i_mask, g_psx_icache_tv[1024];
uint64_t psx_cycle_count;
uint32_t g_psx_gcs_frac;
uint32_t psx_guest_cycle_scale_config(void) { return 1; }
void psx_guest_cycle_scale_snapshot(uint32_t out[3]) { memset(out,0,12); }
void psx_guest_cycle_scale_restore(const uint32_t in[3]) { (void)in; }
uint8_t* memory_get_ram_ptr(void) { return ram; }
uint32_t memory_get_ram_bytes(void) { return sizeof ram; }
uint8_t* memory_get_scratchpad_ptr(void) { return spad; }
int psx_ram_8mb_active(void) { return 0; }
void memory_clear_low_boot_scratch(void) { ++boot_clears; memset(ram,0,sizeof ram); }
void dirty_ram_clear_image_baseline(void) { ++boot_clears; }
void cdrom_notify_game_started(void) { ++cd_handoffs; }
uint32_t dirty_ram_get_bitmap_word_count(void) { return 16; }
uint32_t dirty_ram_get_bitmap_word(uint32_t i) { (void)i; return 0; }
void dirty_ram_set_bitmap_words(const uint32_t* p,uint32_t n) { (void)p; (void)n; }
void overlay_watch_invalidate_after_ram_restore(void) {}
void psx_kernel_bless_note_range(uint32_t p,uint32_t n) { (void)p; (void)n; }
void gte_canonicalize_cpu_state(CPUState* cpu) { cpu->gpr[0]=0; }
uint32_t interrupts_get_cycles_since_vblank(void) { return 42; }
void interrupts_set_cycles_since_vblank(uint32_t n) { (void)n; ++device_restores; }
void psx_devices_service_to_now(void) {}
void spu_catch_up(void) {}
void timers_get_snapshot(uint16_t c[3],uint32_t m[3],uint16_t t[3],int32_t irq[3],uint32_t f[3]) {
    memset(c,0,6); memset(m,0,12); memset(t,0,6); memset(irq,0,12); memset(f,0,12);
}
void timers_set_snapshot(const uint16_t c[3],const uint32_t m[3],const uint16_t t[3],const int32_t irq[3],const uint32_t f[3]) {
    (void)c;(void)m;(void)t;(void)irq;(void)f; ++device_restores;
}
#define MODULE(name) uint32_t name##_snapshot_bytes(void){return 4;} \
    void name##_snapshot_write(uint8_t* p){memset(p,0,4);} \
    int name##_snapshot_read(const uint8_t* p,uint32_t n){(void)p;++device_restores;return n==4;}
MODULE(gpu) MODULE(spu) MODULE(cdrom) MODULE(dma) MODULE(sio) MODULE(mdec) MODULE(spu_cd)
int spu_snapshot_len_ok(uint32_t n){return n==4;}
int sio_snapshot_validate(const uint8_t* p,uint32_t n){(void)p;return n==4;}
int mdec_snapshot_validate(const uint8_t* p,uint32_t n){(void)p;return n==4;}
uint8_t* spu_get_ram_ptr(void){return spuram;}
uint32_t spu_get_ram_bytes(void){return sizeof spuram;}
uint32_t psx_mod_memory_snapshot_bytes(void){return 0;}
uint32_t psx_mod_memory_layout_cookie(void){return 0;}
void psx_mod_memory_snapshot_write(uint8_t* p){(void)p;}
int psx_mod_memory_snapshot_read(const uint8_t* p,uint32_t n){(void)p;return n==0;}
int psx_mod_memory_snapshot_validate(const uint8_t* p,uint32_t n){(void)p;return n==0;}
int gpu_vram_dirty_tracking(void){return 0;}
uint32_t gpu_vram_dirty_row_count(void){return 0;}
const uint64_t* gpu_vram_dirty_mask(void){static const uint64_t mask[8]={0};return mask;}
void gpu_vram_dirty_clear(void){}
void gr_vram_transfer_in(int x,int y,int w,int h,const uint16_t* p){(void)x;(void)y;memcpy(vram,p,(size_t)w*h*2);++device_restores;}
void gr_vram_transfer_out(int x,int y,int w,int h,uint16_t* p){(void)x;(void)y;memcpy(p,vram,(size_t)w*h*2);}

static void u32(uint8_t* p,uint32_t n){PstW w;pst_w_init(&w,p,4);assert(pst_w_u32(&w,n));}
static uint32_t test_u32(const uint8_t *p) {
    PstR r; uint32_t value; pst_r_init(&r,p,4); assert(pst_r_u32(&r,&value)); return value;
}
static uint8_t *test_find_section(uint8_t *image, size_t len, uint32_t wanted,
                                  size_t *offset, size_t *framed_bytes) {
    size_t at = BOOT_STATE_HEADER_WIRE_BYTES;
    const uint32_t count = test_u32(image + 28);
    for (uint32_t i = 0; i < count; ++i) {
        PstR r; uint32_t tag, flags; uint64_t bytes;
        assert(at <= len && len - at >= 16);
        pst_r_init(&r,image+at,16);
        assert(pst_r_u32(&r,&tag) && pst_r_u32(&r,&flags) && pst_r_u64(&r,&bytes));
        assert(bytes <= len - at - 16);
        if (tag == wanted) {
            if (offset) *offset = at;
            if (framed_bytes) *framed_bytes = 16 + (size_t)bytes;
            return image + at + 16;
        }
        at += 16 + (size_t)bytes;
    }
    return NULL;
}
int main(void) {
    CPUState cpu={0};
    fntrace_set_game_range(0x801b0000,0);
    for(int compressed=0;compressed<2;++compressed) for(int started=0;started<2;++started) {
        uint8_t* saved=NULL; size_t len=0;
        cpu.pc=0x80050044; cpu.gpr[4]=0x12345678; ram[0x100]=0xab;
        fntrace_restore_game_started(started);
        assert(compressed ? boot_state_save_buffer(&cpu,123,0x801b0000,&saved,&len)
                          : boot_state_save_buffer_raw(&cpu,123,0x801b0000,&saved,&len));
        size_t phase_offset, phase_bytes;
        uint8_t *phase = test_find_section(saved,len,BS_SEC_GAME_START,&phase_offset,&phase_bytes);
        const uint32_t count = test_u32(saved+28);
        assert(saved[4]==9 && phase && phase[0]==started);
        fntrace_restore_game_started(!started); ram[0x100]=0; cpu.pc=0;
        assert(boot_state_load_buffer(saved,len,123,0x801b0000,&cpu));
        assert(fntrace_is_game_started()==started && ram[0x100]==0xab && cpu.pc==0x80050044);
        assert(!boot_clears && !cd_handoffs);
        /* Malformed, missing and duplicate phase sections must reject before
         * changing CPU, RAM or the previously active handoff latch. */
        cpu.pc=0xdeadbeef; ram[0x100]=0x55; fntrace_restore_game_started(!started);
        /* A different build-selected implementation must reject atomically,
         * including when its only difference is an HLE execution boundary. */
        PstR key_reader; uint32_t implementation_key;
        pst_r_init(&key_reader, saved + 24, 4);
        assert(pst_r_u32(&key_reader, &implementation_key));
        assert(implementation_key == BOOT_STATE_IMPLEMENTATION_KEY);
        u32(saved + 24, implementation_key ^ 0x54474801u);
        assert(!boot_state_load_buffer(saved,len,123,0x801b0000,&cpu));
        assert(cpu.pc==0xdeadbeef && ram[0x100]==0x55 && fntrace_is_game_started()==!started);
        u32(saved + 24, implementation_key);
        /* A foreign execution identity must fail before any state is applied. */
        u32(saved+32,boot_state_layout_cookie() ^ 0x12345678u);
        assert(!boot_state_load_buffer(saved,len,123,0x801b0000,&cpu));
        assert(cpu.pc==0xdeadbeef && ram[0x100]==0x55 && fntrace_is_game_started()==!started);
        u32(saved+32,boot_state_layout_cookie());
        phase[0]=2;
        assert(!boot_state_load_buffer(saved,len,123,0x801b0000,&cpu));
        phase[0]=started;
        uint8_t *missing=malloc(len); assert(missing);
        memcpy(missing,saved,phase_offset);
        memcpy(missing+phase_offset,saved+phase_offset+phase_bytes,len-phase_offset-phase_bytes);
        u32(missing+28,count-1); /* omit the required phase, wherever located */
        assert(!boot_state_load_buffer(missing,len-phase_bytes,123,0x801b0000,&cpu));
        uint8_t* duplicate=malloc(len+phase_bytes); assert(duplicate);
        memcpy(duplicate,saved,len); memcpy(duplicate+len,saved+phase_offset,phase_bytes);
        u32(duplicate+28,count+1);
        assert(!boot_state_load_buffer(duplicate,len+phase_bytes,123,0x801b0000,&cpu));
        assert(cpu.pc==0xdeadbeef && ram[0x100]==0x55 && fntrace_is_game_started()==!started);
        free(missing); free(duplicate); free(saved);
    }
    puts("snapshot handoff: cold/warm, BIOS/game, compressed/raw and atomic rejection passed");
    return 0;
}

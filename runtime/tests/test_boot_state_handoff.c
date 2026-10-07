/* Exercise the real snapshot writer/loader and handoff latch with hardware
 * doubles. In particular, restore must never run the destructive boot hook. */
#include "boot_state.c"
#include "fntrace.c"
#include <assert.h>

static uint8_t ram[2*1024*1024], spad[1024], spuram[512*1024];
static uint16_t vram[1024*512];
static int boot_clears, cd_handoffs;
uint32_t i_stat, i_mask, g_psx_icache_tv[1024];
uint64_t psx_cycle_count;
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
void interrupts_set_cycles_since_vblank(uint32_t n) { (void)n; }
void timers_get_snapshot(uint16_t c[3],uint32_t m[3],uint16_t t[3],int32_t irq[3],uint32_t f[3]) {
    memset(c,0,6); memset(m,0,12); memset(t,0,6); memset(irq,0,12); memset(f,0,12);
}
void timers_set_snapshot(const uint16_t c[3],const uint32_t m[3],const uint16_t t[3],const int32_t irq[3],const uint32_t f[3]) {
    (void)c;(void)m;(void)t;(void)irq;(void)f;
}
#define MODULE(name) uint32_t name##_snapshot_bytes(void){return 4;} \
    void name##_snapshot_write(uint8_t* p){memset(p,0,4);} \
    int name##_snapshot_read(const uint8_t* p,uint32_t n){(void)p;return n==4;}
MODULE(gpu) MODULE(spu) MODULE(cdrom) MODULE(dma) MODULE(sio) MODULE(mdec)
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
void gr_vram_transfer_in(int x,int y,int w,int h,const uint16_t* p){(void)x;(void)y;memcpy(vram,p,(size_t)w*h*2);}
void gr_vram_transfer_out(int x,int y,int w,int h,uint16_t* p){(void)x;(void)y;memcpy(p,vram,(size_t)w*h*2);}

static void u32(uint8_t* p,uint32_t n){PstW w;pst_w_init(&w,p,4);assert(pst_w_u32(&w,n));}
int main(void) {
    CPUState cpu={0};
    fntrace_set_game_range(0x801b0000,0);
    for(int compressed=0;compressed<2;++compressed) for(int started=0;started<2;++started) {
        uint8_t* saved=NULL; size_t len=0;
        cpu.pc=0x80050044; cpu.gpr[4]=0x12345678; ram[0x100]=0xab;
        fntrace_restore_game_started(started);
        assert(compressed ? boot_state_save_buffer(&cpu,123,0x801b0000,&saved,&len)
                          : boot_state_save_buffer_raw(&cpu,123,0x801b0000,&saved,&len));
        assert(saved[4]==9 && saved[len-4]==started); /* handoff is last */
        fntrace_restore_game_started(!started); ram[0x100]=0; cpu.pc=0;
        assert(boot_state_load_buffer(saved,len,123,0x801b0000,&cpu));
        assert(fntrace_is_game_started()==started && ram[0x100]==0xab && cpu.pc==0x80050044);
        assert(!boot_clears && !cd_handoffs);
        /* Malformed, missing and duplicate phase sections must reject before
         * changing CPU, RAM or the previously active handoff latch. */
        cpu.pc=0xdeadbeef; ram[0x100]=0x55; fntrace_restore_game_started(!started);
        /* A foreign execution identity must fail before any state is applied. */
        u32(saved+32,boot_state_layout_cookie() ^ 0x12345678u);
        assert(!boot_state_load_buffer(saved,len,123,0x801b0000,&cpu));
        assert(cpu.pc==0xdeadbeef && ram[0x100]==0x55 && fntrace_is_game_started()==!started);
        u32(saved+32,boot_state_layout_cookie());
        saved[len-4]=2;
        assert(!boot_state_load_buffer(saved,len,123,0x801b0000,&cpu));
        saved[len-4]=started;
        u32(saved+28,16); /* omit the last, required phase section */
        assert(!boot_state_load_buffer(saved,len-20,123,0x801b0000,&cpu));
        u32(saved+28,18);
        uint8_t* duplicate=malloc(len+20); assert(duplicate);
        memcpy(duplicate,saved,len); memcpy(duplicate+len,saved+len-20,20);
        assert(!boot_state_load_buffer(duplicate,len+20,123,0x801b0000,&cpu));
        assert(cpu.pc==0xdeadbeef && ram[0x100]==0x55 && fntrace_is_game_started()==!started);
        free(duplicate); free(saved);
    }
    puts("snapshot handoff: cold/warm, BIOS/game, compressed/raw and atomic rejection passed");
    return 0;
}

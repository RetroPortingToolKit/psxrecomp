#include "wait_contract.h"
/* Reuse the existing device stubs and detector wiring, not a second cycle model. */
#define main shared_detector_fixture_main
#include "test_idle_skip_store_counter.c"
#undef main
#include <stdlib.h>
#include "psx_icache.h"
#include "psx_native_idle_wait.h"

uint32_t g_debug_last_store_pc;
volatile uint32_t g_psx_last_fn_entry;
int g_psx_precise_slice=0, g_psx_load_delay=1;
volatile int g_ds_recording=0;
static uint32_t backing[0x200000/4];
uint8_t *g_psx_ram=(uint8_t*)backing;
uint32_t g_psx_ram_mask=0x1FFFFFu;
int psx_load_delay_enabled(void) { return 1; }
int psx_slice_block_impl(CPUState *c,uint32_t p,uint32_t n,int e) {
    (void)c;(void)p;(void)n;(void)e;abort();
}
uint32_t psx_cyc_load_word_slow(CPUState *c,uint32_t a,uint32_t r,uint32_t m) {
    (void)c;(void)a;(void)r;(void)m;abort();
}

#define ram ((uint32_t*)(g_psx_ram+0x1FFE00u))
#define counter (*(uint32_t*)(g_psx_ram+(WAIT_COUNTER & 0x1FFFFFu)))
static uint32_t callbacks[3], callback_n;
static uint32_t deliveries;
static uint32_t rd(uint32_t addr) {
    if (addr==WAIT_COUNTER) return counter;
    if (addr>=0x801FFE00u && addr<0x80200000u) return ram[(addr-0x801FFE00u)/4];
    fprintf(stderr,"unexpected read %08X\n",addr); abort();
}
static void wr(uint32_t addr, uint32_t value) {
    if (!(addr>=0x801FFE00u && addr<0x80200000u)) abort();
    ram[(addr-0x801FFE00u)/4]=value;
    guest_store(addr,value);
}
static void host_wr(uint32_t addr,uint32_t value) {
    if (!(addr>=0x801FFE00u && addr<0x80200000u)) abort();
    ram[(addr-0x801FFE00u)/4]=value;
}
void psx_check_interrupts_at(CPUState *cpu,uint32_t pc) {
    psx_cyc_batch_flush();
    psx_idle_note_check(cpu,pc);
    if (!deliveries && psx_get_cycle_count()>=s_event_at) { counter++; deliveries++; }
}
#include "wait.inc"

typedef struct Result {
    uint32_t regs[32], stack[128], calls[3], call_n, pc, vblanks;
    uint32_t hi,lo,scale_fraction;
    uint8_t absorb[33],which,ld,ld_absorb,fudge;
    uint64_t cycles, skips;
} Result;
static Result actual_run(int ext,uint32_t initial,uint32_t target,uint32_t budget,
                         uint32_t event_cycle,uint32_t scale,int batch) {
    CPUState cpu; memset(&cpu,0,sizeof cpu);
    for (int i=1;i<32;i++) cpu.gpr[i]=0x13570000u+(uint32_t)i;
    cpu.gpr[4]=target; cpu.gpr[5]=budget;
    cpu.gpr[29]=0x801FFF00u;cpu.gpr[31]=0x800FFFFCu;
    cpu.ld_which_t=0x20;cpu.read_fudge=0x20;
    cpu.read_word=rd;cpu.write_word=wr;
    memset(ram,0xA5,128*4);memset(callbacks,0,sizeof callbacks);
    counter=initial;callback_n=0;deliveries=0;
    psx_cycles_reset_for_boot();psx_cycles_resync_after_restore(&cpu);psx_icache_reset();
    psx_guest_cycle_scale_set(scale);
    g_psx_gcs_batch=batch;
    g_psx_gcs_frac=0;
    g_idle_skip_enabled=ext;g_idle_skip_ext=ext;
    g_idle_skip_count=0;g_guest_store_count=0;g_mmio_access_count=0;
    const uint32_t sites[]={WAIT_SITE_0,WAIT_SITE_1};
    psx_idle_skip_set_store_counter_sites(sites,2);
    psx_idle_skip_set_host_writer(host_wr);
    psx_idle_skip_set_store_tracker(&g_psx_track_last_store,&g_guest_last_store_addr,&g_guest_last_store_val);
    psx_idle_skip_set_audited_only(ext);
    s_event_at=event_cycle;
    for (int dispatch=0;dispatch<16;dispatch++) {
        if (!cpu.pc || (cpu.pc>=WAIT_ENTRY && cpu.pc<WAIT_END)) WAIT_FUNCTION(&cpu);
        else if (cpu.pc==WAIT_CALLBACK_0 || cpu.pc==WAIT_CALLBACK_1 || cpu.pc==WAIT_CALLBACK_2) {
            if (callback_n>=3) abort();callbacks[callback_n++]=cpu.pc;
            cpu.pc=cpu.gpr[31];
        } else break;
    }
    if (ext && g_psx_track_last_store) {
        fprintf(stderr,"store tracking leaked outside audited wait\n");abort();
    }
    Result r;memset(&r,0,sizeof r);memcpy(r.regs,cpu.gpr,sizeof r.regs);memcpy(r.stack,ram,sizeof r.stack);
    memcpy(r.calls,callbacks,sizeof callbacks);r.call_n=callback_n;r.pc=cpu.pc;
    r.hi=cpu.hi;r.lo=cpu.lo;r.scale_fraction=g_psx_gcs_frac;
    memcpy(r.absorb,cpu.read_absorb,sizeof r.absorb);r.which=cpu.read_absorb_which;
    r.ld=cpu.ld_which_t;r.ld_absorb=cpu.ld_absorb;r.fudge=cpu.read_fudge;
    r.vblanks=deliveries;r.cycles=psx_get_cycle_count();r.skips=g_idle_skip_count;return r;
}
int main(void) {
    unsigned cases=0, accelerated=0;
    for (int batch=0;batch<=1;batch++) for (uint32_t scale=1;scale<=2;scale++) {
        const uint32_t events[]={1,127,4096,200003,400007};
        for(unsigned j=0;j<sizeof events/sizeof *events;j++) {
            Result a=actual_run(0,10,11,1,events[j],scale,batch);
            Result b=actual_run(1,10,11,1,events[j],scale,batch);
            uint64_t skips=b.skips;a.skips=b.skips=0;
            if (memcmp(&a,&b,sizeof a)) {
                fprintf(stderr,"wait mismatch event=%u scale=%u cycles=%llu/%llu skips=%llu\n",
                    events[j],scale,(unsigned long long)a.cycles,(unsigned long long)b.cycles,(unsigned long long)skips);
                return 1;
            }
            cases++;if(skips)accelerated++;
        }
        const uint32_t args[][4]={{12,11,1,4096},{10,11,0,200003},
            {10,11,1,4000000},{0xFFFFFFFEu,0xFFFFFFFFu,1,200003},
            {0x80000000u,0,1,200003},{10,11,0xFFFFu,200003}};
        for(unsigned j=0;j<sizeof args/sizeof *args;j++) {
            Result a=actual_run(0,args[j][0],args[j][1],args[j][2],args[j][3],scale,batch);
            Result b=actual_run(1,args[j][0],args[j][1],args[j][2],args[j][3],scale,batch);
            a.skips=b.skips=0;
            if(memcmp(&a,&b,sizeof a)) {fprintf(stderr,"edge mismatch case=%u scale=%u\n",j,scale);return 1;}
            cases++;
        }
    }
    if(!accelerated) return 1;
    printf("PASS: %u actual-wait caller comparisons; %u accelerated; exact RAM/GPR/pipeline/continuation/callback/event/cycle outputs; scaled-charge batching on/off\n",cases,accelerated);
    return 0;
}

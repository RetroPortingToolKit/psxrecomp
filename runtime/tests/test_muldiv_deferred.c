/* Production mult/div deadline regressions. No BIOS, disc or copied guest code.
 * Companion of test_gte_deferred.c: the MULT/DIV completion deadline and the
 * MFLO/MFHI stall must see CPU work still pending in the batch or a local
 * accumulator, exactly as the GTE helpers do. */
#include "psx_cyc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int g_ls_replay_active = 0;
int g_ls_mode = 0;
int g_precise_mode = 0;
int g_psx_call_bail = 0;
int g_boot_state_cpu_timing_restored = 0; /* no restored boot state in this fixture */
int psx_mod_host_output_scope(void) { return 0; } /* no host-output mod callback */
uint32_t i_mask = 0;
uint64_t g_guest_store_count = 0;
uint64_t g_mmio_access_count = 0;

void sio_advance(uint32_t cycles) { (void)cycles; }
void cdrom_advance(uint32_t cycles) { (void)cycles; }
void dma_advance(uint32_t cycles) { (void)cycles; }
void timers_advance(uint32_t cycles) { (void)cycles; }
void interrupts_advance_cycles(uint32_t cycles) { (void)cycles; }
void interrupts_service_scheduled_events(void) {}

uint32_t interrupts_cycles_to_vblank(void) { return UINT32_MAX; }
uint32_t timers_cycles_to_irq(uint32_t mask) { (void)mask; return UINT32_MAX; }
uint32_t cdrom_cycles_to_irq(uint32_t mask) { (void)mask; return UINT32_MAX; }
uint32_t dma_cycles_to_internal_event(void) { return UINT32_MAX; }
void source_gpu_runtime_advance(void) {}
uint32_t source_gpu_runtime_cycles_to_event(void) { return UINT32_MAX; }
uint32_t dma_cycles_to_deliverable_irq(uint32_t mask) {
    (void)mask;
    return UINT32_MAX;
}
uint32_t sio_cycles_to_irq(uint32_t mask) { (void)mask; return UINT32_MAX; }
uint32_t psx_spu_sample_event_cycles_to_next(void) { return UINT32_MAX; }
void psx_spu_sample_event_service(void) {}
int psx_get_in_exception(void) { return 0; }

void starvation_watchdog_check(void) {}
void starvation_ring_pc_sample(void) {}

int  psx_netplay_active(void) { return 0; }
int  psx_selfcheck_enabled(void) { return 0; }
void dirty_ram_ld_delay_discard(void) {}
void dirty_ram_irq_ambient_resync_after_restore(void) {}


static uint32_t local;
static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);return 1;}}while(0)
/* Published clock 1000, nothing pending, a generated block's deadline probe
 * deferred (bb_defer) so charges accumulate; `mode` 0 pends CPU work in the
 * batch, 1 in a local accumulator, 2 in both. */
static void reset(unsigned mode) {
 psx_cycles_reset_for_boot();
 psx_advance_cycles(1000);
 psx_next_service_cycle=UINT64_MAX;
 g_psx_cyc_batch=0;g_psx_cyc_batch_limit=64;g_psx_cyc_bb_defer=1;
 local=0;g_psx_cyc_local_acc=mode?&local:NULL;
}
static void pend(unsigned mode,unsigned cycles) {
 if(mode==0)g_psx_cyc_batch+=cycles;
 else if(mode==1)local+=cycles;
 else{g_psx_cyc_batch+=cycles-cycles/2;local+=cycles/2;}
}
int main(void) {
 CPUState cpu={0};
 /* Authored timing equivalent of MULT / six one-cycle ops / MFLO, the shape
  * R4's movie player runs at 0x80114DE4..0x80114E00 (multiplicand 320, so
  * latency 7). MULT's base cycle ends at 1001, so the product is ready at
  * 1008, when MFLO issues: no stall. Reading the published clock alone armed
  * the deadline at 1007 from a stale 1000 and then stalled 7 more cycles. */
 reset(0);
 CHECK(psx_mult_latency_s(320)==7);
 psx_cyc_step(&cpu,0);psx_muldiv_set(&cpu,psx_mult_latency_s(320));
 for(int i=0;i<6;i++)psx_cyc_step(&cpu,0);
 psx_cyc_step(&cpu,0);psx_muldiv_stall(&cpu);psx_cyc_batch_flush();
 CHECK(psx_cycle_count==1008);
 /* Every latency class (MULT 7/10/14, DIV 37), work pending at the op from
  * the batch, a local accumulator or both, and every elapsed distance to the
  * stall across the deadline, including the one-cycle retract. */
 const uint32_t lat[4]={psx_mult_latency_s(0xFFFFFFFDu),psx_mult_latency_s(0x1000u),
                        psx_mult_latency_u(0x80000000u),37u};
 CHECK(lat[0]==7 && lat[1]==10 && lat[2]==14);
 for(unsigned l=0;l<4;l++)for(unsigned mode=0;mode<3;mode++)
 for(unsigned at_set=0;at_set<=8;at_set++)for(unsigned elapsed=0;elapsed<=45;elapsed++)
 for(unsigned give=0;give<=3;give+=3) {
  memset(&cpu,0,sizeof(cpu));
  reset(mode);pend(mode,at_set);
  psx_muldiv_set(&cpu,lat[l]);
  const uint64_t deadline=1000+at_set+lat[l];
  CHECK(cpu.muldiv_ts_done==deadline && psx_cycle_count==1000+at_set);
  CHECK(g_psx_cyc_batch==0 && local==0);
  pend(mode,elapsed);
  cpu.read_absorb_which=5;cpu.read_absorb[5]=(uint8_t)give;
  psx_muldiv_stall(&cpu);
  const uint64_t now=1000+at_set+elapsed;
  uint64_t want=now,want_done=deadline;unsigned want_give=give;
  if(deadline==now+1)want_done=deadline-1;         /* retract, no advance */
  else if(deadline>now){unsigned stall=(unsigned)(deadline-now);
   want=deadline;want_give=give>stall?give-stall:0;}
  CHECK(psx_cycle_count==want && cpu.muldiv_ts_done==want_done);
  CHECK(cpu.read_absorb[5]==want_give);
  CHECK(g_psx_cyc_batch==0 && local==0);
 }
 printf("PASS %u production mult/div pending-charge/deadline checks\n",checks);return 0;
}

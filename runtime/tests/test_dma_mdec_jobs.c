/* Actual DMA + MDEC: partial publication, live input, cancellation, wrapping,
 * FIFO stalls and same-build active-transfer restore. Unused devices abort
 * through fixture_link.py; RAM stores retain channel/destination provenance. */
#include "dma.h"
#include "mdec.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t psx_cycle_count, psx_next_service_cycle, s_frame_count;
int psx_in_device_service, g_event_step_conservative, g_ls_replay_active;
int g_psx_render_pass_active, g_psx_guest_time_frozen;
uint32_t g_psx_cyc_batch, g_psx_cyc_batch_limit;
uint32_t g_psx_ram_size=0x200000u, g_psx_ram_mask=0x1FFFFFu;
uint32_t i_stat, g_debug_current_func_addr=0x80012340u;
uint32_t g_debug_last_store_pc=0x80012344u;
uint64_t g_io_openbus_reads, g_io_openbus_writes;
extern int g_dma_exec_depth, g_dma_cur_ch;
extern uint32_t g_dma_cur_madr, g_dma_initiator_pc;
extern uint32_t dma_snapshot_bytes(void);
extern void dma_snapshot_write(uint8_t *p);
extern int dma_snapshot_read(const uint8_t *p,uint32_t len);

static uint8_t ram[0x200000u], saved_ram[0x200000u], final_ram[0x200000u];
static unsigned stores, irq_calls, other_events, failures;
static uint64_t other_due;
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);++failures; } } while(0)
#define CH0 0x1F801080u
#define CH1 0x1F801090u
#define DPCR 0x1F8010F0u
#define DICR 0x1F8010F4u
#define MDATA 0x1F801820u
#define MCTRL 0x1F801824u

uint8_t *memory_get_ram_ptr(void) {return ram;}
uint32_t psx_read_word(uint32_t address) {
    uint32_t value;memcpy(&value,ram+(address&0x1FFFFCu),4);return value;
}
void psx_write_word(uint32_t address,uint32_t value) {
    if(g_dma_exec_depth) {
        CHECK(g_dma_cur_ch==1);
        CHECK(g_dma_cur_madr==(address&0x1FFFFCu));
        CHECK(g_dma_initiator_pc==g_debug_last_store_pc);
        ++stores;
    }
    memcpy(ram+(address&0x1FFFFCu),&value,4);
}
void psx_devices_service_to_now(void) {}
void psx_advance_cycles_slow(uint32_t cycles) {psx_cycle_count+=cycles;dma_advance(cycles);}
int psx_get_in_exception(void) {return 0;}
int debug_server_fmv_quiet(void) {return 1;}
void event_ring_record_aux(uint16_t kind,uint8_t detail,uint32_t aux) {(void)kind;(void)detail;(void)aux;}
void psx_irq_raise(uint32_t bit,uint32_t detail) {(void)detail;i_stat|=bit;++irq_calls;}

static void reset(void) {
    memset(ram,0xA5,sizeof ram);stores=irq_calls=other_events=0;
    psx_cycle_count=0;psx_next_service_cycle=UINT64_MAX;other_due=73;
    i_stat=0;dma_init();mdec_init();mdec_write(MCTRL,0x60000000u);
    dma_write(DPCR,0x88u);
    dma_write(DICR,(1u<<23)|(3u<<16));
}
static void decode(unsigned depth) {
    mdec_write(MDATA,0x20000006u|(depth<<27));
    for(unsigned i=0;i<6;i++)mdec_dma_write_word(0xFE000000u);
}
static void kick(unsigned channel,uint32_t address,uint32_t words,int decrement) {
    uint32_t base=channel?CH1:CH0;
    dma_write(base,address);dma_write(base+4,0x10000u|words);
    dma_write(base+8,0x01000200u|(channel?0u:1u)|(decrement?2u:0u));
}
static void step(uint32_t cycles) {
    psx_cycle_count+=cycles;dma_advance(cycles);
    while(psx_cycle_count>=other_due) {++other_events;other_due+=73;}
}
static void finish(unsigned channel) {
    uint32_t base=channel?CH1:CH0;
    unsigned limit=100000;
    while(dma_read(base+8)&(1u<<24)) {
        CHECK(limit--!=0);if(!limit)break;
        uint32_t next=dma_cycles_to_internal_event();
        CHECK(next>0 && next<=448u);if(next==0 || next==UINT32_MAX)break;
        uint64_t other=other_due-psx_cycle_count;
        if(other<next)next=(uint32_t)other;
        step(next?next:1);
    }
    CHECK(!(dma_read(base+8)&(1u<<24)));
}
static void output_case(uint32_t address,unsigned words,int decrement,unsigned depth) {
    reset();decode(depth);kick(1,address,words,decrement);finish(1);
    CHECK(psx_cycle_count==(uint64_t)words*14u);
    CHECK(stores==words && irq_calls>0);
    CHECK(other_events==psx_cycle_count/73u);
    CHECK(dma_get_dicr()&(1u<<25));
    for(unsigned i=0;i<words;i++) {
        uint32_t at=(address+(decrement?0u-i*4u:i*4u))&0x1FFFFCu;
        CHECK(psx_read_word(at)==(depth==3?0x42104210u:0x80808080u));
    }
    MDECDebugState state;mdec_debug_get_state(&state);
    CHECK(state.dma_out_words==words && state.dma_read_underflows==0);
}
static void stalled_and_cancelled(void) {
    reset();kick(1,0x1000u,71,0);
    CHECK(dma_cycles_to_internal_event()==UINT32_MAX);
    step(1000);CHECK(stores==0);
    decode(2);finish(1);CHECK(stores==71);
    CHECK(psx_cycle_count==1000u+71u*14u);
    kick(1,0x2000u,121,0);step(37u*14u);CHECK(stores==108);
    dma_write(CH1+8,0);unsigned before=stores;step(1000);CHECK(stores==before);
    kick(1,0x3000u,84,0);finish(1);CHECK(stores==192);
    MDECDebugState state;mdec_debug_get_state(&state);
    CHECK(state.dma_out_words==192 && !state.dma_read_underflows);
    CHECK(state.output_pos==0 && state.output_size==0);
}
static void disabled_channel(void) {
    reset();decode(2);kick(1,0x1000,71,0);step(37u*14u);
    CHECK(stores==37);dma_write(DPCR,0x8u);
    unsigned before=stores;step(1000);CHECK(stores==before);
    CHECK(dma_cycles_to_internal_event()==UINT32_MAX);
    dma_write(DPCR,0x88u);finish(1);
    CHECK(stores==71 && psx_cycle_count==1000u+71u*14u);
}
static void partial_snapshot(int input) {
    reset();
    if(input) {
        psx_write_word(0x100,0x30000060u);
        for(unsigned i=1;i<=96;i++)psx_write_word(0x100+4*i,0xFE000000u);
        kick(0,0x100,97,0);step(37);
        MDECDebugState partial;mdec_debug_get_state(&partial);
        CHECK(partial.busy && partial.input_count>0);
        CHECK(partial.input_count<partial.expected_halfwords);
        CHECK(dma_read(CH0+8)&(1u<<24));
    } else {decode(2);kick(1,0x1000,192,0);step(37u*14u);CHECK(stores==37);}
    uint32_t dn=dma_snapshot_bytes(),mn=mdec_snapshot_bytes();
    uint8_t *ds=malloc(dn),*ms=malloc(mn);CHECK(ds && ms);if(!ds || !ms)exit(1);
    dma_snapshot_write(ds);mdec_snapshot_write(ms);memcpy(saved_ram,ram,sizeof ram);
    uint64_t saved_clock=psx_cycle_count,saved_other_due=other_due;
    unsigned saved_stores=stores,saved_events=other_events;
    finish(input?0:1);
    if(input){kick(1,0x1000,192,0);finish(1);}
    memcpy(final_ram,ram,sizeof ram);uint64_t final_clock=psx_cycle_count;
    MDECDebugState expected;mdec_debug_get_state(&expected);
    memcpy(ram,saved_ram,sizeof ram);psx_cycle_count=saved_clock;other_due=saved_other_due;
    stores=saved_stores;other_events=saved_events;i_stat=0;
    CHECK(mdec_snapshot_read(ms,mn));CHECK(dma_snapshot_read(ds,dn));
    finish(input?0:1);
    if(input){kick(1,0x1000,192,0);finish(1);}
    MDECDebugState actual;mdec_debug_get_state(&actual);
    CHECK(!memcmp(ram,final_ram,sizeof ram));CHECK(psx_cycle_count==final_clock);
    CHECK(actual.dma_out_words==expected.dma_out_words);
    CHECK(actual.output_pos==expected.output_pos && actual.output_size==expected.output_size);
    free(ds);free(ms);
}
static void scale_table(void) {
    mdec_write(MDATA,0x60000000u);
    for(unsigned i=0;i<32;i++)mdec_dma_write_word(0x20002000u);
}
static void live_input(void) {
    uint32_t original[192];
    reset();scale_table();decode(2);
    CHECK(mdec_dma_read_words(original,192)==192);
    reset();scale_table();psx_write_word(0x100,0x30000006u);
    for(unsigned i=1;i<=6;i++)psx_write_word(0x100+4*i,0xFE000000u);
    kick(0,0x100,7,0);step(2);
    /* The final input word has not been served in either policy. Its live
     * rewrite is consumed, not a snapshot taken at the kick. */
    psx_write_word(0x118,0xFE000100u);finish(0);
    CHECK(psx_cycle_count==7 && mdec_dma_read_ready());
    uint32_t live[192];CHECK(mdec_dma_read_words(live,192)==192);
    reset();scale_table();mdec_write(MDATA,0x30000006u);
    for(unsigned i=0;i<6;i++)mdec_dma_write_word(i==5?0xFE000100u:0xFE000000u);
    uint32_t expected[192];CHECK(mdec_dma_read_words(expected,192)==192);
    CHECK(!memcmp(live,expected,sizeof live));
    CHECK(memcmp(live,original,sizeof live)!=0);
}
int main(void) {
    output_case(0x1000,192,0,2);output_case(0x1000,7,0,2);
    output_case(0x1FFFFCu,71,0,2);output_case(8,71,1,2);
    output_case(0x1000,128,0,3);output_case(0x1000,17,0,3);
    stalled_and_cancelled();disabled_channel();partial_snapshot(0);partial_snapshot(1);live_input();
    if(failures)return 1;
    puts("PASS actual MDEC/DMA jobs: data/provenance/tails/wrap/decrement/live input/stall/cancel/partial restore");
    return 0;
}

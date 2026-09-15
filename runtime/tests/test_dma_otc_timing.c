/* Synthetic controller test: no BIOS, retail code, or disc is needed.
 * Include the implementation to isolate channel 6 from unrelated devices. */
#include "../src/dma.c"
#include <assert.h>
uint64_t s_frame_count;
uint64_t psx_cycle_count, psx_next_service_cycle;
int psx_in_device_service, g_event_step_conservative, g_ls_replay_active;
uint32_t g_psx_cyc_batch, g_psx_cyc_batch_limit;
uint32_t i_stat, g_debug_current_func_addr, g_debug_last_store_pc;
static uint32_t ram[0x80000], writes, irqs;
void psx_devices_service_to_now(void) { advance_source_otc(); }
void psx_advance_cycles_slow(uint32_t n) { psx_cycle_count+=n; advance_source_otc(); }
void psx_write_word(uint32_t addr, uint32_t value) { ram[(addr & 0x1ffffc)/4]=value; writes++; }
void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)detail; i_stat|=1u<<bit; irqs++; }
void event_ring_record_aux(uint16_t kind,uint8_t src,uint32_t value) { (void)kind; (void)src; (void)value; }
static void setup(uint32_t count, uint64_t phase) {
    dma_init();
    memset(ram,0xCC,sizeof(ram)); writes=irqs=i_stat=0;
    psx_cycle_count=phase; psx_next_service_cycle=0;
    channels[6].madr=0x100000; channels[6].bcr=count; channels[6].chcr=0x11000002;
    dpcr|=8u<<24;
    dicr=(1u<<23)|(1u<<22);
}
static void tick(uint32_t cycles) { psx_cycle_count+=cycles; advance_source_otc(); }
int main(void) {
    setup(1024,0);
    execute_ch6_otc();
    assert(writes==1024 && psx_cycle_count==0 && !(channels[6].chcr&(1u<<24)));
    for(uint32_t phase=0;phase<128;phase++) {
        setup(1024,phase); start_source_otc();
        assert(writes==64 && otc_source.remaining==960 && irqs==0);
        assert(ram[0x100000/4]==0xFFFFC && ram[(0x100000-64*4)/4]==0xCCCCCCCC);
        uint64_t expected=((phase+960+127)/128)*128;
        tick((uint32_t)(expected-psx_cycle_count)-1);
        assert(otc_source.remaining && (channels[6].chcr&(1u<<24)) && irqs==0);
        tick(1); assert(writes==1024 && !otc_source.remaining && irqs==1);
        assert(ram[(0x100000-1023*4)/4]==0xFFFFFF);
        tick(1000); assert(writes==1024 && irqs==1);
    }
    for(uint32_t n=1;n<=65;n++) {
        setup(n,127); start_source_otc();
        assert(writes==(n<64?n:64));
        assert((otc_source.remaining==0)==(n<=64));
        if(n==65) { tick(1); assert(writes==65 && irqs==1); }
    }
    setup(0,0); start_source_otc(); tick(65536);
    assert(writes==65536 && !otc_source.remaining && irqs==1);
    setup(1024,43); otc_source_model=1; execute_ch6_otc();
    assert(writes==1024 && irqs==1 && psx_cycle_count==1024);
    assert(dma_snapshot_read(NULL,0)==0); /* source model rejects restore before reading */
    puts("PASS: default, source start budget, all 128 phases, partial RAM, completion/IRQ, short/zero counts, CPU wait, restore guard");
    return 0;
}

/* Source GPU projection is inactive in this isolated controller fixture. */
int source_gpu_runtime_active(void) {return 0;}
int mdec_source_active(void) {return 0;}
void source_gpu_runtime_dma_write(void) {}

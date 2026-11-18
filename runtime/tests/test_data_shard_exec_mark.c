/* A data shard replay writes guest RAM and must mark the written pages
 * executable, as CD-DMA delivery does. The mark takes a physical address.
 * The test includes the owning source and records the marks it makes. */
#include "../src/data_shards.c"

static uint8_t ram[DS_RAM_BYTES];
static uint8_t scratch[DS_SP_BYTES];
static uint32_t mark_phys, mark_len, marks;

uint8_t *memory_get_ram_ptr(void) { return ram; }
uint8_t *memory_get_scratchpad_ptr(void) { return scratch; }
void psx_write_word(uint32_t addr, uint32_t val) { (void)addr; (void)val; }
void psx_write_byte(uint32_t addr, uint8_t val) {
    if (addr >= 0x80000000u && addr < 0x80000000u + DS_RAM_BYTES)
        ram[addr - 0x80000000u] = val;
}
void dirty_ram_mark_executable_range(uint32_t phys, uint32_t len) {
    mark_phys = phys; mark_len = len; marks++;
}
void psx_check_interrupts_at(CPUState *cpu, uint32_t pc) { (void)cpu; (void)pc; }

int main(void) {
    static CPUState cpu;
    uint8_t bytes[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    DsRange wr = {0x12340u, 8u};
    DsShard s;
    memset(&s, 0, sizeof s);
    s.nwr = 1; s.wr = &wr; s.wr_bytes = bytes; s.wr_total = 8u;
    ds_apply(&cpu, &s);
    if (ram[0x12340u] != 1u || ram[0x12347u] != 8u) {
        puts("FAIL: replay did not write RAM");
        return 1;
    }
    if (marks != 1u || mark_phys != 0x12340u || mark_len != 8u) {
        printf("FAIL: executable mark at 0x%08X len %u (marks %u), want 0x00012340 len 8\n",
               (unsigned)mark_phys, (unsigned)mark_len, (unsigned)marks);
        return 1;
    }
    puts("PASS: data shard replay marks its RAM writes executable by physical address");
    return 0;
}

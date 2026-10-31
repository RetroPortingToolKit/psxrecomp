/* Actual writer/reader, using the existing hardware doubles. Track allocation
 * failures and compare deterministic wire output across selected builds. */
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
static unsigned allocation_calls;
static int fail_allocation;
static void *capture_malloc(size_t n) {
    ++allocation_calls;
    if (fail_allocation) { fail_allocation=0; return NULL; }
    return malloc(n);
}
static void *capture_realloc(void *p,size_t n) {
    ++allocation_calls;
    if (fail_allocation) { fail_allocation=0; return NULL; }
    return realloc(p,n);
}
#define malloc capture_malloc
#define realloc capture_realloc
#define main handoff_main
#define PSX_CAPTURE_WRITER_ONLY 1
#include "test_boot_state_handoff.c"
#undef main
#undef malloc
#undef realloc

static uint64_t hash(uint64_t h,const uint8_t *p,size_t n) {
    for(size_t i=0;i<n;i++) h=(h^p[i])*UINT64_C(1099511628211);
    return h;
}

int main(void) {
    uint64_t digest=UINT64_C(1469598103934665603);
    assert(handoff_main()==0);
    for(unsigned expanded=0;expanded<2;expanded++)
    for(unsigned arena=0;arena<3;arena++)
    for(unsigned tracked=0;tracked<2;tracked++) {
        static const uint32_t arena_sizes[]={0,1,12};
        CPUState cpu={0};uint8_t *data=NULL;size_t capacity=0,len=0;
        ram_bytes=expanded ? sizeof ram : 2*1024*1024;
        arena_bytes=0;pending_arena_bytes=(int)arena_sizes[arena];vram_tracking=(int)tracked;
        memset(ram,0x63,ram_bytes);memset(spad,0x24,sizeof spad);
        memset(spuram,0x44,sizeof spuram);memset(arenas,0x19,sizeof arenas);
        for(size_t i=0;i<sizeof vram/sizeof vram[0];i++)vram[i]=(uint16_t)(i^0x1234);
        dirty_rows=512;memset(dirty_mask,0xff,sizeof dirty_mask);boot_state_vram_mirror_reset();
        cpu.pc=0x80050044;cpu.gpr[4]=0xabcdef01;psx_cycle_count=12345;
        cpu.gte_ts_done=psx_cycle_count+37;fntrace_restore_game_started(1);
        cd_frames=0;pending_cd_frames=173; /* sizing must follow catch-up */
        unsigned before=allocation_calls;
        assert(boot_state_capture_raw(&cpu,123,0x801b0000,&data,&capacity,&len));
        assert(data && len<=capacity && cd_frames==173);
#if PSX_NATIVE_CAPTURE_JOBS
        assert(allocation_calls-before==(arena_bytes&1 && !tracked ? 2u : 1u)); /* odd full VRAM stages once */
        assert(capacity<len+65536u);
#endif
        uint64_t first=hash(0,data,len);
        before=allocation_calls;
        uint8_t *previous=data;size_t previous_capacity=capacity,previous_len=len;
        assert(boot_state_capture_raw(&cpu,123,0x801b0000,&data,&capacity,&len));
        assert(len==previous_len && hash(0,data,len)==first);
#if PSX_NATIVE_CAPTURE_JOBS
        assert(data==previous && capacity==previous_capacity);
        assert(allocation_calls-before==(arena_bytes&1 && !tracked ? 1u : 0u));
#endif
        digest=hash(digest,data,len);
        /* Failure retains the owned allocation and reports no partial blob. */
        previous=data;previous_capacity=capacity;
#if PSX_NATIVE_CAPTURE_JOBS
        capacity=1; /* valid lower bound forces the actual reserve path */
#endif
        fail_allocation=1;len=999;
        assert(!boot_state_capture_raw(&cpu,123,0x801b0000,&data,&capacity,&len));
        assert(!fail_allocation && data==previous && len==0);
#if PSX_NATIVE_CAPTURE_JOBS
        assert(capacity==1);capacity=previous_capacity;
#else
        assert(capacity==previous_capacity);
#endif
        assert(hash(0,data,previous_len)==first);
        /* Current-build load restores the high RAM extent, device input and
         * unaligned arena, not merely the serializer's byte count. */
        memset(ram,0,ram_bytes);memset(spad,0,sizeof spad);memset(spuram,0,sizeof spuram);
        memset(vram,0,sizeof vram);memset(arenas,0,sizeof arenas);cd_frames=0;cpu.pc=0;
        assert(boot_state_load_buffer(data,previous_len,123,0x801b0000,&cpu));
        assert(cpu.pc==0x80050044 && cpu.gpr[4]==0xabcdef01);
        assert(ram[0]==0x63 && ram[ram_bytes-1]==0x63 && spad[511]==0x24 && spuram[100]==0x44);
        assert(vram[1234]==(uint16_t)(1234^0x1234) && cd_frames==173);
        if(arena_bytes)assert(arenas[arena_bytes-1]==0x19);
        free(data);
    }
#if PSX_NATIVE_CAPTURE_JOBS
    {
        BsOut fake={0};fake.len=SIZE_MAX-8;
        assert(!bs_raw_payload(&fake,BS_SEC_SPU,1) && !fake.data && !fake.cap);
    }
#endif
    printf("capture wire digest %016llx\n",(unsigned long long)digest);
    return 0;
}

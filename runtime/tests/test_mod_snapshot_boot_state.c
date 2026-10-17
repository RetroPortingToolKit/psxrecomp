/* Real boot_state + host registry; reuse the counted hardware fixture and
 * its no-provider compatibility tests, rather than stub the serializer. */
#define PSX_TEST_SNAPSHOT_PACKAGE_LOOKUP 1
#define main boot_state_handoff_tests
#include "test_boot_state_handoff.c"
#undef main

static int packages_active, guard_active, providers_ready = 1, writes_fail;
static uint32_t a_value = 42, b_value = 71;
static int a_reads, b_reads, observers;
static CPUState *restoring_cpu;
static const uint8_t *expected_image;
static uint32_t expected_image_len;
static int accept_restored_pc(uint32_t pc) {
    assert(pc==0x80050044 && ram[0x100]==0xab && a_value==42 && b_value==71);
    return 1;
}
static int refuse_restored_pc(uint32_t pc) {
    assert(accept_restored_pc(pc)); return 0;
}

int mod_runtime_snapshot_package_active(const char *id) {
    if(!strcmp(id,"test.guard"))return guard_active;
    return packages_active && (!strcmp(id,"test.a") || !strcmp(id,"test.b"));
}
static int guard_validate(const uint8_t *p,uint32_t len) {return p&&len==1&&p[0]==1;}
static uint32_t a_bytes(void) { return 4; }
static uint32_t b_bytes(void) { return 1024; } /* Large enough to exercise zlib. */
static int provider_write(uint8_t *out, uint32_t len, uint32_t value) {
    if (writes_fail || len < 4) return 0;
    memset(out,0,len); u32(out,value); return 1;
}
static int a_write(uint8_t *out,uint32_t len) { return provider_write(out,len,a_value); }
static int b_write(uint8_t *out,uint32_t len) { return provider_write(out,len,b_value); }
static int a_validate(const uint8_t *p,uint32_t len) {
    return p && len == 4 && p[0] != 0xff;
}
static int b_validate(const uint8_t *p,uint32_t len) {
    return p && len == 1024 && p[0] != 0xff;
}
static void a_read(const uint8_t *p,uint32_t len) {
    assert(len==4 && ram[0x100]==0xab && restoring_cpu->pc==0x80050044);
    a_value=test_u32(p); ++a_reads;
}
static void b_read(const uint8_t *p,uint32_t len) {
    assert(len==1024 && ram[0x100]==0xab && restoring_cpu->pc==0x80050044);
    b_value=test_u32(p); ++b_reads;
}
static int ready(void) { return providers_ready; }
static void observe(const uint8_t *p,uint32_t len) {
    assert(p==expected_image && len==expected_image_len);
    assert(a_value==42 && b_value==71 && ram[0x100]==0xab);
    ++observers;
}
static void assert_refused_untouched(uint8_t *image,size_t len,CPUState *cpu) {
    const int old_devices=device_restores, old_a_reads=a_reads, old_b_reads=b_reads;
    const int old_observers=observers;
    cpu->pc=0xdeadbeef; cpu->gpr[4]=0x98765432;
    ram[0x100]=0x55; spad[0]=0x66; spuram[0]=0x77;
    i_stat=0x1234; a_value=98; b_value=99;
    assert(!boot_state_load_buffer(image,len,123,0x801b0000,cpu));
    assert(cpu->pc==0xdeadbeef && cpu->gpr[4]==0x98765432);
    assert(ram[0x100]==0x55 && spad[0]==0x66 && spuram[0]==0x77 && i_stat==0x1234);
    assert(a_value==98 && b_value==99 && device_restores==old_devices);
    assert(a_reads==old_a_reads && b_reads==old_b_reads && observers==old_observers);
}

int main(void) {
    const PSXModSnapshotProvider a={1,a_bytes,a_write,a_validate,a_read,ready};
    const PSXModSnapshotProvider b={3,b_bytes,b_write,b_validate,b_read,NULL};
    CPUState cpu={0};
    uint8_t *legacy=NULL;
    size_t legacy_len=0;
    /* Exercise vanilla v9 raw/compressed and both handoff phases first. */
    assert(!boot_state_handoff_tests());
    cpu.pc=0x80050044;
    assert(boot_state_save_buffer_raw(&cpu,123,0x801b0000,&legacy,&legacy_len));
    assert(legacy[4]==9);
    assert(psx_mod_register_snapshot_guard("test.guard",NULL,1,guard_validate));
    for(unsigned enabled=0;enabled<2;++enabled) {
        guard_active=enabled;
        assert(boot_state_load_buffer(legacy,legacy_len,123,0x801b0000,&cpu));
    }
    /* Import an actual old-format host section and its layout cookie, without
     * retaining the constant provider in new saves. Corrupt imports stay atomic. */
    uint8_t *old=malloc(legacy_len+55);assert(old);memcpy(old,legacy,legacy_len);
    uint8_t *section=old+legacy_len,*payload=section+16;
    memset(section,0,55);u32(section,BS_SEC_MOD_HOST);u32(section+8,39);
    u32(payload,0x504e534du);u32(payload+4,1);u32(payload+8,1);
    u32(payload+16,1);u32(payload+20,10);u32(payload+24,1);
    memcpy(payload+28,"test.guard",10);payload[38]=1;
    u32(old+4,10);u32(old+28,test_u32(old+28)+1);
    uint32_t guard_cookie=2166136261u;const char *id="test.guard";
    do {guard_cookie=(guard_cookie^(uint8_t)*id)*16777619u;}while(*id++);
    for(unsigned i=0;i<4;++i)guard_cookie=(guard_cookie^(uint8_t)(1u>>(8*i)))*16777619u;
    u32(old+32,test_u32(legacy+32)^guard_cookie);
    for(unsigned enabled=0;enabled<2;++enabled) {
        guard_active=enabled;cpu.pc=0;
        assert(boot_state_load_buffer(old,legacy_len+55,123,0x801b0000,&cpu));
        assert(cpu.pc==0x80050044);
        payload[38]=0;assert_refused_untouched(old,legacy_len+55,&cpu);payload[38]=1;
    }
    free(old);
    assert(psx_mod_register_snapshot_provider("test.b",&b));
    assert(psx_mod_register_snapshot_provider("test.a",&a));
    assert(psx_mod_register_snapshot_loaded_observer("test.a",observe));
    assert(psx_mod_register_snapshot_loaded_observer("test.b",observe));
    packages_active=1; restoring_cpu=&cpu;
    assert_refused_untouched(legacy,legacy_len,&cpu);
    /* Even a forged current cookie cannot make an incomplete legacy state
     * acceptable to an enabled provider set. */
    u32(legacy+32,boot_state_layout_cookie());
    assert_refused_untouched(legacy,legacy_len,&cpu);
    free(legacy);
    for (int compressed=0;compressed<2;++compressed) {
        uint8_t *saved=NULL,*broken,*reordered;
        size_t len=0, host_offset=0, host_framed=0;
        uint8_t *host;
        uint32_t count;
        cpu.pc=0x80050044; cpu.gpr[4]=0x12345678; ram[0x100]=0xab;
        a_value=42; b_value=71;
        assert(compressed ? boot_state_save_buffer(&cpu,123,0x801b0000,&saved,&len)
                          : boot_state_save_buffer_raw(&cpu,123,0x801b0000,&saved,&len));
        assert(saved[4]==10);
        count=test_u32(saved+28);
        host=test_find_section(saved,len,BS_SEC_MOD_HOST,&host_offset,&host_framed);
        assert(host && host_offset+host_framed==len);
        assert(test_u32(saved+host_offset+4)==(compressed ? BOOT_STATE_SEC_ZLIB : 0));
        cpu.pc=0; ram[0x100]=0; a_value=b_value=0;
        expected_image=saved; expected_image_len=(uint32_t)len;
        const int before_observers=observers;
        assert(boot_state_load_buffer(saved,len,123,0x801b0000,&cpu));
        assert(a_value==42 && b_value==71 && observers==before_observers+1);
        /* Final dispatch checks run against restored guest and host state.
         * A refused load and its silent rollback do not split diagnostics. */
        assert(!boot_state_load_buffer_checked(saved,len,123,0x801b0000,&cpu,refuse_restored_pc));
        assert(observers==before_observers+1);
        assert(boot_state_load_buffer_quiet(saved,len,123,0x801b0000,&cpu));
        assert(observers==before_observers+1);
        assert(boot_state_load_buffer_checked(saved,len,123,0x801b0000,&cpu,accept_restored_pc));
        assert(observers==before_observers+2);
        /* The host section may come first: its read still sees the restored
         * CPU/RAM, and notification sees both completed host providers. */
        reordered=malloc(len); assert(reordered);
        memcpy(reordered,saved,BOOT_STATE_HEADER_WIRE_BYTES);
        memcpy(reordered+BOOT_STATE_HEADER_WIRE_BYTES,saved+host_offset,host_framed);
        memcpy(reordered+BOOT_STATE_HEADER_WIRE_BYTES+host_framed,
               saved+BOOT_STATE_HEADER_WIRE_BYTES,host_offset-BOOT_STATE_HEADER_WIRE_BYTES);
        cpu.pc=0; ram[0x100]=0; a_value=b_value=0;
        expected_image=reordered;
        assert(boot_state_load_buffer(reordered,len,123,0x801b0000,&cpu));
        assert(observers==before_observers+3);
        free(reordered);
        broken=malloc(len+host_framed); assert(broken);
        memcpy(broken,saved,len);
        assert_refused_untouched(broken,len-1,&cpu); /* Late truncated section. */
        /* Missing and duplicate host sections both refuse before restoring
         * an earlier CPU/RAM/device or provider. */
        u32(broken+28,count-1);
        assert_refused_untouched(broken,host_offset,&cpu);
        memcpy(broken,saved,len); memcpy(broken+len,saved+host_offset,host_framed);
        u32(broken+28,count+1);
        assert_refused_untouched(broken,len+host_framed,&cpu);
        memcpy(broken,saved,len); u32(broken+4,9);
        assert_refused_untouched(broken,len,&cpu); /* Old-reader downgrade. */
        memcpy(broken,saved,len);
        if (!compressed) {
            /* Provider A is valid; B has an invalid value. */
            broken[host_offset+16+56]=0xff;
        } else {
            /* Corrupt the zlib checksum of the last section. */
            broken[len-1]^=0x80;
        }
        assert_refused_untouched(broken,len,&cpu);
        packages_active=0;
        memcpy(broken,saved,len); u32(broken+32,boot_state_layout_cookie());
        assert_refused_untouched(broken,len,&cpu); /* Unknown/disabled providers. */
        packages_active=1;
        providers_ready=0;
        assert_refused_untouched(saved,len,&cpu);
        uint8_t *failed=(uint8_t *)(uintptr_t)1; size_t failed_len=99;
        assert(!boot_state_save_buffer_raw(&cpu,123,0x801b0000,&failed,&failed_len));
        assert(!failed && !failed_len);
        providers_ready=1;
        writes_fail=1;
        assert(!boot_state_save_buffer_raw(&cpu,123,0x801b0000,&failed,&failed_len));
        assert(!failed && !failed_len);
        writes_fail=0;
        free(broken); free(saved);
    }
    puts("host snapshots: raw/zlib roundtrip, guest-before-host order, exact observers and atomic refusal passed");
    return 0;
}

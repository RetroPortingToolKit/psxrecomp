/* Include implementation to inspect owned backing storage without pulling in
 * hardware routing. Section GC discards unrelated memory access functions. */
#include "memory.c"
int g_psx_render_pass_active = 0;
static void check(int ok,const char* text){if(!ok){fprintf(stderr,"FAIL %s\n",text);exit(1);}}
int main(void){
    check(psx_mod_memory_snapshot_bytes()==0 && psx_mod_memory_layout_cookie()==0,"vanilla unchanged");
    psx_mod_memory_snapshot_write(NULL);
    check(psx_mod_memory_alloc(13,16)==0x9f000000u,"CPU allocation");
    check(psx_mod_gpu_dma_memory_alloc(5u*1024u*1024u,16)==PSX_MOD_GPU_DMA_GUEST_BASE,"beyond old four MiB");
    check(psx_mod_gpu_dma_resolve_address(0xd00000u)==0x100000u,"unallocated tag retains retail folding");
    uint32_t size=psx_mod_memory_snapshot_bytes(),cookie=psx_mod_memory_layout_cookie();
    uint8_t* saved=(uint8_t*)malloc(size);check(saved!=NULL,"test buffer");
    memset(mod_memory,0x12,mod_memory_used);memset(mod_gpu_dma_memory,0x34,mod_gpu_dma_memory_used);
    psx_mod_memory_snapshot_write(saved);
    memset(mod_memory,0x56,mod_memory_used);memset(mod_gpu_dma_memory,0x78,mod_gpu_dma_memory_used);
    check(!psx_mod_memory_snapshot_read(saved,size-1),"truncation rejected");
    saved[0]=99;
    check(!psx_mod_memory_snapshot_read(saved,size) && mod_memory[0]==0x56,"bad version no mutation");
    saved[0]=1;saved[8]++;
    check(!psx_mod_memory_snapshot_read(saved,size) && mod_gpu_dma_memory[0]==0x78,"bad layout no mutation");
    saved[8]--;
    check(psx_mod_memory_snapshot_read(saved,size),"restore");
    for(uint32_t i=0;i<mod_memory_used;++i)check(mod_memory[i]==0x12,"CPU bytes restored");
    for(uint32_t i=0;i<mod_gpu_dma_memory_used;++i)check(mod_gpu_dma_memory[i]==0x34,"DMA bytes restored");
    check(psx_mod_memory_layout_cookie()==cookie,"stable layout identity");
    check(psx_mod_gpu_dma_memory_alloc(16,16)!=0 && psx_mod_memory_layout_cookie()!=cookie,"layout change detected");
    check(!psx_mod_memory_snapshot_read(saved,size),"old layout rejected");
    /* Render passes write mod arenas through a first-write page journal and
     * put them back with the rest of the pass restore. */
    {
        const uint64_t before = render_pass_mod_arenas_hash();
        check(render_pass_mod_store(PSX_MOD_GPU_DMA_GUEST_BASE + 8u, 0xAABBCCDDu, 4) == 1 &&
              mod_gpu_dma_memory[8] == 0xDD && mod_gpu_dma_memory[11] == 0xAA,
              "pass store reaches the GPU-DMA arena");
        check(render_pass_mod_store(PSX_MOD_GPU_DMA_GUEST_BASE + 4096u - 2u, 0x1234u, 2) == 1,
              "pass store at a page edge");
        check(render_pass_mod_store(0x9f000004u, 0x77u, 1) == 1 && mod_memory[4] == 0x77,
              "pass store reaches mod memory");
        check(render_pass_mod_store(0x80001000u, 1u, 4) == 0, "main RAM left to the RAM path");
        check(render_pass_mod_arenas_hash() != before, "hash sees the pass writes");
        render_pass_mod_arenas_rollback();
        check(mod_gpu_dma_memory[8] == 0x34 && mod_gpu_dma_memory[4094] == 0x34 &&
              mod_memory[4] == 0x12 && render_pass_mod_arenas_hash() == before,
              "rollback restores every journaled page");
        check(render_pass_mod_store(PSX_MOD_GPU_DMA_GUEST_BASE + 8u, 0x1u, 4) == 1 &&
              (render_pass_mod_arenas_rollback(), mod_gpu_dma_memory[8] == 0x34),
              "a second pass journals again");
    }
    // Replaying an earlier draw snapshot must still roll back to the completed
    // live frame, including pages the replay never otherwise writes.
    size = psx_mod_memory_snapshot_bytes();
    saved = (uint8_t*)realloc(saved, size);check(saved != NULL,"new-layout test buffer");
    psx_mod_memory_snapshot_write(saved);
    memset(mod_memory,0xA5,mod_memory_used);memset(mod_gpu_dma_memory,0x5A,mod_gpu_dma_memory_used);
    {
        const uint64_t completed = render_pass_mod_arenas_hash();
        g_psx_render_pass_active = 1;
        check(psx_mod_memory_snapshot_read(saved,size),"draw-entry arena restore in a pass");
        check(mod_memory[0]==0x12 && mod_gpu_dma_memory[0]==0x34,"earlier draw-entry bytes visible");
        check(render_pass_mod_store(PSX_MOD_GPU_DMA_GUEST_BASE,0xDEADBEEFu,4)==1,"replay writes restored arena");
        g_psx_render_pass_active = 0;
        render_pass_mod_arenas_rollback();
        check(render_pass_mod_arenas_hash()==completed,"rollback restores complete live arena after earlier snapshot");
    }
    free(saved);puts("enhancement snapshot checks passed");return 0;
}

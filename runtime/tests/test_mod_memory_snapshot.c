/* Include implementation to inspect owned backing storage without pulling in
 * hardware routing. Section GC discards unrelated memory access functions. */
#include "memory.c"
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
    free(saved);puts("enhancement snapshot checks passed");return 0;
}

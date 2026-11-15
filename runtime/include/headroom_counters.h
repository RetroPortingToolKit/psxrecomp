#ifndef PSX_HEADROOM_COUNTERS_H
#define PSX_HEADROOM_COUNTERS_H
#include <stdint.h>

/* Host-lifetime counts only; never simulation/snapshot state. The disabled
 * build is an overhead control, not a runtime arm/disarm operation. */
#if defined(PSX_DISABLE_HEADROOM_COUNTERS) && PSX_DISABLE_HEADROOM_COUNTERS
#define PSX_HR_INC(value) ((void)0)
#define PSX_HR_SET(value, replacement) ((void)0)
#define PSX_HEADROOM_RECORD_FRAME() ((void)0)
#else
#define PSX_HR_INC(value) (++(value))
#define PSX_HR_SET(value, replacement) ((value)=(replacement))
#define PSX_HEADROOM_RECORD_FRAME() psx_headroom_record_frame()
#endif

enum { PSX_HR_REQUEST_OTHER, PSX_HR_REQUEST_GPU_HOLD,
       PSX_HR_REQUEST_MMIO, PSX_HR_REQUEST_OBSERVATION, PSX_HR_REQUEST_COUNT };
enum { PSX_HR_DEADLINE_VBLANK, PSX_HR_DEADLINE_TIMER, PSX_HR_DEADLINE_CD,
       PSX_HR_DEADLINE_DMA, PSX_HR_DEADLINE_SIO, PSX_HR_DEADLINE_SPU,
       PSX_HR_DEADLINE_CAP, PSX_HR_DEADLINE_SAMPLE, PSX_HR_DEADLINE_UNKNOWN,
       PSX_HR_DEADLINE_EARLY, PSX_HR_DEADLINE_COUNT };

#define PSX_HR_FIELDS(X) \
 X(DISPATCH,dispatch) X(BLOCK_EDGES,block_edges) X(SLOW_EDGES,slow_edges) \
 X(CHECK_IRQ,check_irq) X(CHECK_IRQ_AT,check_irq_at) \
 X(IRQ_FAST_SR,irq_fast_sr) X(IRQ_FAST_NONE,irq_fast_none) \
 X(IRQ_MID,irq_mid) X(IRQ_EVAL,irq_eval) X(IRQ_DELIVER,irq_deliver) \
 X(DEV_SVC,dev_svc) \
 X(SVC_OTHER,service_request_other) X(SVC_GPU,service_request_gpu_hold) \
 X(SVC_MMIO,service_request_mmio) X(SVC_OBSERVE,service_request_observation) \
 X(DL_VBLANK,deadline_vblank) X(DL_TIMER,deadline_timer) X(DL_CD,deadline_cd) \
 X(DL_DMA,deadline_dma) X(DL_SIO,deadline_sio) X(DL_SPU,deadline_spu) \
 X(DL_CAP,deadline_cap) X(DL_SAMPLE,deadline_cycle_sample) \
 X(DL_UNKNOWN,deadline_unknown) X(DL_EARLY,service_before_deadline) \
 X(LOAD_INLINE_RAM,load_inline_ram) X(LOAD_INLINE_SCRATCH,load_inline_scratch) \
 X(LOAD_HELPER_RAM,load_helper_ram) X(LOAD_HELPER_SCRATCH,load_helper_scratch) \
 X(LOAD_HELPER_MMIO,load_helper_mmio) X(LOAD_HELPER_OTHER,load_helper_other) \
 X(STORE_INLINE_RAM,store_inline_ram) X(STORE_INLINE_SCRATCH,store_inline_scratch) \
 X(STORE_HELPER_RAM,store_helper_ram) X(STORE_HELPER_SCRATCH,store_helper_scratch) \
 X(STORE_HELPER_MMIO,store_helper_mmio) X(STORE_HELPER_OTHER,store_helper_other) \
 X(PGXP_ALU,pgxp_alu) X(PGXP_LOAD,pgxp_load) X(PGXP_STORE,pgxp_store) \
 X(PGXP_COP2,pgxp_cop2) X(PGXP_MULDIV,pgxp_muldiv) \
 X(PGXP_BATCHES,pgxp_plain_batches) X(PGXP_REGISTERS,pgxp_plain_registers) \
 X(PGXP_CAPTURED,pgxp_native_words_captured) X(PGXP_PUBLISHED,pgxp_native_words_published) \
 X(PGXP_SXY,pgxp_native_sxy_imports) X(PGXP_CLOBBER,pgxp_native_clobbers) \
 X(PGXP_GPR,pgxp_native_gpr_imports) \
 X(DMA_HOLDS,dma_holds) X(DMA_CYCLES,dma_hold_cycles) X(DMA_HOST_US,dma_hold_host_us) \
 X(DMA_WORDS,dma_hold_words) X(DMA_STEPS,dma_hold_steps) \
 X(DMA_EXC_COUNT,dma_exc_holds) X(DMA_EXC_CYCLES,dma_exc_hold_cycles) X(DMA_EXC_US,dma_exc_hold_host_us) \
 X(NATIVE_LISTS,native_lists) X(NATIVE_NODES,native_nodes) X(NATIVE_WORDS,native_words) \
 X(NATIVE_DIRECT,native_direct_words) X(NATIVE_OBSERVED,native_observed_words) \
 X(NATIVE_SERVICES,native_service_requests) X(NATIVE_FALLBACKS,native_fallbacks) \
 X(VALID_QUERIES,validity_queries) X(VALID_HITS,validity_hits) X(VALID_VERIFIES,validity_verifies) \
 X(STATIC_ENTRIES,static_fast_entries) X(STATIC_MISSES,static_fast_misses) X(MEMCMP,text_memcmp_calls) \
 X(SPU_BLOCKS,spu_render_blocks) X(SPU_QUERIES,spu_sample_queries) \
 X(SPU_CHECKS,spu_sample_checks) X(SPU_PUMPS,spu_sample_pumps) \
 X(MEM_INLINE_LOAD_RAM_U8,mem_inline_load_ram_u8) \
 X(MEM_INLINE_LOAD_RAM_U16,mem_inline_load_ram_u16) \
 X(MEM_INLINE_LOAD_RAM_U32,mem_inline_load_ram_u32) \
 X(MEM_INLINE_LOAD_SCRATCH_U8,mem_inline_load_scratch_u8) \
 X(MEM_INLINE_LOAD_SCRATCH_U16,mem_inline_load_scratch_u16) \
 X(MEM_INLINE_LOAD_SCRATCH_U32,mem_inline_load_scratch_u32) \
 X(MEM_INLINE_STORE_RAM_U8,mem_inline_store_ram_u8) \
 X(MEM_INLINE_STORE_RAM_U16,mem_inline_store_ram_u16) \
 X(MEM_INLINE_STORE_RAM_U32,mem_inline_store_ram_u32) \
 X(MEM_INLINE_STORE_SCRATCH_U8,mem_inline_store_scratch_u8) \
 X(MEM_INLINE_STORE_SCRATCH_U16,mem_inline_store_scratch_u16) \
 X(MEM_INLINE_STORE_SCRATCH_U32,mem_inline_store_scratch_u32) \
 X(MEM_HELPER_LOAD_RAM_U8,mem_helper_load_ram_u8) \
 X(MEM_HELPER_LOAD_RAM_U16,mem_helper_load_ram_u16) \
 X(MEM_HELPER_LOAD_RAM_U32,mem_helper_load_ram_u32) \
 X(MEM_HELPER_LOAD_SCRATCH_U8,mem_helper_load_scratch_u8) \
 X(MEM_HELPER_LOAD_SCRATCH_U16,mem_helper_load_scratch_u16) \
 X(MEM_HELPER_LOAD_SCRATCH_U32,mem_helper_load_scratch_u32) \
 X(MEM_HELPER_LOAD_MMIO_U8,mem_helper_load_mmio_u8) \
 X(MEM_HELPER_LOAD_MMIO_U16,mem_helper_load_mmio_u16) \
 X(MEM_HELPER_LOAD_MMIO_U32,mem_helper_load_mmio_u32) \
 X(MEM_HELPER_LOAD_OTHER_U8,mem_helper_load_other_u8) \
 X(MEM_HELPER_LOAD_OTHER_U16,mem_helper_load_other_u16) \
 X(MEM_HELPER_LOAD_OTHER_U32,mem_helper_load_other_u32) \
 X(MEM_HELPER_STORE_RAM_U8,mem_helper_store_ram_u8) \
 X(MEM_HELPER_STORE_RAM_U16,mem_helper_store_ram_u16) \
 X(MEM_HELPER_STORE_RAM_U32,mem_helper_store_ram_u32) \
 X(MEM_HELPER_STORE_SCRATCH_U8,mem_helper_store_scratch_u8) \
 X(MEM_HELPER_STORE_SCRATCH_U16,mem_helper_store_scratch_u16) \
 X(MEM_HELPER_STORE_SCRATCH_U32,mem_helper_store_scratch_u32) \
 X(MEM_HELPER_STORE_MMIO_U8,mem_helper_store_mmio_u8) \
 X(MEM_HELPER_STORE_MMIO_U16,mem_helper_store_mmio_u16) \
 X(MEM_HELPER_STORE_MMIO_U32,mem_helper_store_mmio_u32) \
 X(MEM_HELPER_STORE_OTHER_U8,mem_helper_store_other_u8) \
 X(MEM_HELPER_STORE_OTHER_U16,mem_helper_store_other_u16) \
 X(MEM_HELPER_STORE_OTHER_U32,mem_helper_store_other_u32) \
 X(MEM_RUNTIME_FAST_LOAD_RAM_U8,mem_runtime_fast_load_ram_u8) \
 X(MEM_RUNTIME_FAST_LOAD_RAM_U16,mem_runtime_fast_load_ram_u16) \
 X(MEM_RUNTIME_FAST_LOAD_RAM_U32,mem_runtime_fast_load_ram_u32) \
 X(SVC_GPU_IMPLICIT,service_gpu_hold_implicit)

enum {
#define PSX_HR_FIELD(id, name) PSX_HR_##id,
    PSX_HR_FIELDS(PSX_HR_FIELD)
#undef PSX_HR_FIELD
    PSX_HR_COUNT
};
#define PSX_HEADROOM_CAP 4096u
typedef struct PsxHeadroomFrame {
    uint64_t seq, frame, cycles;
    uint64_t resets[2]; /* bit i: source counter decreased since previous frame */
    uint64_t values[PSX_HR_COUNT];
} PsxHeadroomFrame;

#ifdef __cplusplus
extern "C" {
#endif
extern uint64_t psx_hr_service_requests[PSX_HR_REQUEST_COUNT];
extern uint64_t psx_hr_service_deadlines[PSX_HR_DEADLINE_COUNT];
extern uint64_t psx_hr_interrupt_at_calls;
extern uint32_t psx_hr_gpu_hold_depth;
extern uint64_t psx_hr_gpu_hold_implicit_services;
extern uint64_t psx_hr_runtime_fast_loads[3]; /* u8/u16/u32; legacy value leaves */
#if defined(PSX_COSIM) || defined(PSX_OVERLAY_DLL_BUILD)
#define PSX_HR_RUNTIME_FAST_LOAD(width_index) ((void)0)
#else
#define PSX_HR_RUNTIME_FAST_LOAD(width_index) PSX_HR_INC(psx_hr_runtime_fast_loads[width_index])
#endif
/* Context covers the charge as well as the explicit drain. It changes only
 * observability; synchronous restore/soft-exit cleanup may clear it early. */
#define PSX_HR_GPU_HOLD_BEGIN() PSX_HR_INC(psx_hr_gpu_hold_depth)
#define PSX_HR_GPU_HOLD_END() PSX_HR_SET(psx_hr_gpu_hold_depth, \
    psx_hr_gpu_hold_depth ? psx_hr_gpu_hold_depth - 1u : 0u)
extern const char *const psx_headroom_field_names[PSX_HR_COUNT];
void psx_headroom_totals(PsxHeadroomFrame *out);
void psx_headroom_record_frame(void);
uint64_t psx_headroom_sequence(void);
int psx_headroom_get(uint64_t seq, PsxHeadroomFrame *out);
int psx_headroom_enabled(void);
#ifdef __cplusplus
}
#endif
#endif

/* Fixed-device absolute-time observation path. Included by psx_cycles.c.
 * Each device retains its own elapsed interval. Unrelated MMIO does not walk
 * quiet devices; real events are still visited in dependency order. These
 * clocks are host bookkeeping: full snapshot barriers materialize all state,
 * and boot/restore re-anchor every clock to the restored guest epoch. */
enum { OBS_SIO, OBS_CD, OBS_DMA, OBS_TIMER, OBS_FRAME, OBS_SPU, OBS_COUNT };
#define OBS_BIT(n) (1u << (n))
#define OBS_ALL ((1u << OBS_COUNT) - 1u)
static uint64_t s_observed_cycle[OBS_COUNT];
static uint64_t s_materialized_cycle;

static void observation_anchor(uint64_t cycle) {
    for (unsigned i = 0; i < OBS_COUNT; ++i) s_observed_cycle[i] = cycle;
    s_materialized_cycle = cycle;
}

/* Evaluate relative device APIs in their own time domain. CD response/lid
 * deadlines and SPU sample phase use the absolute guest clock internally. */
static uint64_t observation_due(unsigned device, uint32_t mask) {
    const uint64_t live = psx_cycle_count;
    const uint64_t clock = s_observed_cycle[device];
    uint32_t distance = UINT32_MAX;
    psx_cycle_count = clock;
    switch (device) {
    case OBS_SIO: distance = sio_cycles_to_irq(mask); break;
    case OBS_CD: if (mask & (1u << IRQ_CDROM))
                     distance = cdrom_cycles_to_internal_event(); break;
    case OBS_DMA: distance = mask == UINT32_MAX ? dma_cycles_to_internal_event()
                                              : dma_cycles_to_deliverable_irq(mask); break;
    case OBS_TIMER: distance = timers_cycles_to_irq(mask); break;
    case OBS_FRAME: distance = interrupts_cycles_to_vblank(); break;
    case OBS_SPU: distance = psx_spu_sample_event_cycles_to_next(); break;
    }
    psx_cycle_count = live;
    return distance == UINT32_MAX ? UINT64_MAX : clock + (distance ? distance : 1u);
}

static void observation_advance(unsigned mask, uint64_t target) {
    for (unsigned i = 0; i < OBS_COUNT; ++i) {
        if (!(mask & OBS_BIT(i)) || s_observed_cycle[i] >= target) continue;
        const uint64_t elapsed = target - s_observed_cycle[i];
        /* Normal intervals are frame-bounded, including IRQ-masked devices.
         * Keep integer narrowing safe during explicitly long host advances. */
        const uint32_t cycles = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
        s_observed_cycle[i] += cycles;
        psx_cycle_count = s_observed_cycle[i];
        switch (i) {
        case OBS_SIO: sio_advance(cycles); break;
        case OBS_CD: cdrom_advance(cycles); break;
        case OBS_DMA: dma_advance(cycles); break;
        case OBS_TIMER: timers_advance(cycles); break;
        case OBS_FRAME: interrupts_advance_cycles(cycles); break;
        case OBS_SPU: psx_spu_sample_event_service(); break;
        }
    }
}

static void observation_catch_up(uint64_t target, unsigned observed) {
    for (;;) {
        uint64_t event = UINT64_MAX;
        unsigned due = 0;
        for (unsigned i = 0; i < OBS_COUNT; ++i) {
            const uint64_t at = observation_due(i, UINT32_MAX);
            if (at < event) { event = at; due = OBS_BIT(i); }
            else if (at == event) due |= OBS_BIT(i);
        }
        if (event > target) break;
        /* Before CD publishes readiness, DMA consumes only the old interval.
         * Credit the boundary cycle after CD, exactly like D-1 + 1 stepping.
         * Frame callbacks can save/restore, mix audio or inspect any device;
         * materialize all of them before crossing the frame boundary. */
        if (due & OBS_BIT(OBS_CD)) due |= OBS_BIT(OBS_DMA);
        if (due & OBS_BIT(OBS_SPU)) due |= OBS_BIT(OBS_CD) | OBS_BIT(OBS_DMA);
        if (due & OBS_BIT(OBS_FRAME)) due = OBS_ALL;
        if (event) observation_advance(due, event - 1u);
        observation_advance(due, event);
        psx_cycle_count = event;
        interrupts_service_scheduled_events();
    }
    observation_advance(observed, target);
    psx_cycle_count = target;
    /* This watermark now means processed global timeline, not a claim that
     * every quiet device was materialized. Public full barriers use OBS_ALL. */
    s_devices_synced_cycle = target;
    if (observed == OBS_ALL) s_materialized_cycle = target;
}

static unsigned observation_mmio_mask(uint32_t addr, int write) {
    if (addr >= 0x1F801040u && addr <= 0x1F80105Fu) return OBS_BIT(OBS_SIO);
    if (addr >= 0x1F801070u && addr <= 0x1F801077u) return write ? OBS_ALL : 0;
    if (addr >= 0x1F801080u && addr <= 0x1F8010FFu)
        return OBS_BIT(OBS_DMA) | ((write || dma_cdrom_transfer_active()) ? OBS_BIT(OBS_CD) : 0u);
    if (addr >= 0x1F801100u && addr <= 0x1F80112Fu) return OBS_BIT(OBS_TIMER);
    if (addr >= 0x1F801800u && addr <= 0x1F801803u)
        return OBS_BIT(OBS_CD) | OBS_BIT(OBS_DMA);
    /* GPU mode changes affect the timer divider; DMA and MDEC share FIFOs. */
    if (addr >= 0x1F801810u && addr <= 0x1F801827u)
        return OBS_BIT(OBS_DMA) | ((write && (addr & ~3u) == 0x1F801814u) ? OBS_BIT(OBS_TIMER) : 0u);
    if (addr >= 0x1F801C00u && addr <= 0x1F801FFFu)
        return OBS_BIT(OBS_CD) | OBS_BIT(OBS_DMA) | OBS_BIT(OBS_SPU);
    return 0;
}

/* The entry barrier has already recomputed the schedule. Plain register
 * reads do not invalidate it; retaining that result avoids an otherwise
 * unconditional slow block edge after every such observation. Keep reads
 * with device side effects conservative (I_STAT ticks SIO; CD/SPU/FIFOs can
 * consume or publish state), as well as every write. */
static int observation_read_keeps_deadline(uint32_t addr) {
    if (addr >= 0x1F801000u && addr <= 0x1F80103Fu) return 1;
    if (addr >= 0x1F801060u && addr <= 0x1F801063u) return 1;
    if (addr >= 0x1F801074u && addr <= 0x1F801077u) return 1;
    if (addr >= 0x1F801080u && addr <= 0x1F8010FFu) return 1;
#ifdef PSX_ENABLE_BLOCK_CYCLES
    /* MODE reads clear only TARGET/OVERFLOW flags, not IRQ enable/request
     * or counter state. Untimed diagnostic reads can advance the counter. */
    if (addr >= 0x1F801100u && addr <= 0x1F80112Fu) return 1;
#endif
    return 0;
}

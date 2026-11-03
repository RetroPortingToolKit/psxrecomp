#pragma once
// Shared Neversoft renderer extensions. Layout supplies retail code guards,
// globals, mod identifiers and counter names; this never changes simulation.
#include "mod_plugins.h"
#include "cpu_state.h"
#include "pgxp.h"
#include <cstdint>
#include <cstring>

struct PSXNeversoftPacketLengthSite { uint32_t address, word; };

template<class Layout> struct PSXNeversoftRenderer {
    static constexpr uint32_t ArenaSize = 0x60000u, StockSize = 0x17000u;
    static constexpr uint32_t MaxFar = 16380u;
    inline static uint32_t arena = 0, peak = 0, packet_corrections = 0;
    inline static bool extended = true;

    static bool boot_contract() {
        return psx_mod_read_word(Layout::AllocA) == Layout::AllocWord &&
               psx_mod_read_word(Layout::AllocB) == Layout::AllocWord &&
               psx_mod_read_word(Layout::LimitPc + 4) == 0x34846F00u;
    }
    static void subdivision_precision(CPUState* cpu, uint32_t) {
        // Runs after the original scratchpad SXY store, before RGB2 transfer.
        const uint32_t address = cpu->gpr[5];
        if (address < 0x1F800000u || address > 0x1F8003FCu || (address & 3u) ||
            psx_mod_read_word(Layout::PrecisionPc - 4) != 0xACB00000u) return;
        const uint32_t stored = psx_mod_read_word(address);
        if (stored != cpu->gpr[16]) return;
        if (pgxp_store_flagged_gte_sxy(address, 14, cpu->gte_data[14], stored))
            psx_mod_counter_add(Layout::PrecisionCounter, 1);
    }
    static void packet_length(CPUState* cpu, uint32_t reg) {
        if (!arena) return;
        const uint32_t address = cpu->gpr[reg] & 0x00FFFFFFu;
        const uint32_t base = arena & 0x00FFFFFFu;
        if (address < base || address >= base + 2 * ArenaSize) return;
        // The stored DMA tag retains its link; only the subsequent byte-count
        // calculation excludes the expanded aperture's address bits 22/23.
        cpu->gpr[reg] &= 0xFF000000u;
        ++packet_corrections;
    }
    static void packet_length_t2(CPUState* cpu, uint32_t) { packet_length(cpu, 10); }
    static void packet_length_s2(CPUState* cpu, uint32_t) { packet_length(cpu, 18); }
    static void fog(CPUState* cpu, uint32_t) {
        const int32_t start = int32_t(cpu->gpr[5]), range = int32_t(cpu->gpr[6]);
        if (!extended || start < 0 || range <= 0 || uint64_t(start) + range >= MaxFar ||
            psx_mod_read_word(Layout::FogPc) != 0x00803821u) return;
        uint32_t span = 1;
        while (span < uint32_t(range) && span < 16384u) span <<= 1;
        if (span >= 16384u) return;
        if (span < 8192u) span <<= 1;
        uint32_t begin = uint32_t(start) * 2u;
        if (begin > MaxFar - span) begin = MaxFar - span;
        if (begin < uint32_t(start) || begin + span < uint32_t(start) + uint32_t(range)) return;
        cpu->gpr[5] = begin;
        cpu->gpr[6] = span;
        psx_mod_counter_add(Layout::FogCounter, 1);
    }
    static void swap(CPUState*, uint32_t) {
        if (!arena || !boot_contract()) return;
        const uint32_t a = psx_mod_read_word(Layout::BufferA + 0x74);
        const uint32_t b = psx_mod_read_word(Layout::BufferB + 0x74);
        if (a == arena && b == arena + ArenaSize) return;
        if (a < 0x80010000u || a > 0x80200000u - StockSize ||
            b < 0x80010000u || b > 0x80200000u - StockSize) return;
        for (const auto& site : Layout::PacketLengthSites)
            if (psx_mod_read_word(site.address) != site.word) return;
        psx_mod_write_word(Layout::BufferA + 0x74, arena);
        psx_mod_write_word(Layout::BufferB + 0x74, arena + ArenaSize);
        psx_mod_counter_add(Layout::ArenaCounter, 1);
    }
    static int limit(CPUState* cpu, uint32_t) {
        if (!arena || !boot_contract()) return 0;
        const uint32_t cur = psx_mod_read_word(Layout::Current);
        if (cur != Layout::BufferA && cur != Layout::BufferB) return 0;
        const uint32_t base = psx_mod_read_word(cur + 0x74);
        if (base != arena && base != arena + ArenaSize) return 0;
        const uint32_t end = (base + ArenaSize - 0x100u) & 0x7FFFFFFFu;
        psx_mod_write_word(Layout::Limit, end);
        cpu->gpr[4] = ArenaSize - 0x100u;
        cpu->gpr[5] = cur;
        cpu->gpr[3] = 0x7FFFFFFFu;
        cpu->gpr[2] = end;
        return 1;
    }
    static void submit(CPUState*, uint32_t) {
        if (packet_corrections) {
            psx_mod_counter_add(Layout::PacketCounter, packet_corrections);
            packet_corrections = 0;
        }
        const uint32_t cur = psx_mod_read_word(Layout::Current);
        if (cur != Layout::BufferA && cur != Layout::BufferB) return;
        const uint32_t base = psx_mod_read_word(cur + 0x74);
        const uint32_t used = (psx_mod_read_word(Layout::Cursor) & 0x7FFFFFFFu) -
                              (base & 0x7FFFFFFFu);
        const uint32_t size = arena && (base == arena || base == arena + ArenaSize) ? ArenaSize : StockSize;
        if (used > size) return;
        if (used + 0x200 >= size) psx_mod_counter_add(Layout::FullCounter, 1);
        if (used > peak) {
            psx_mod_counter_add(Layout::PeakCounter, used - peak);
            peak = used;
        }
    }
    static void activate() {
        char value[16] = "";
        extended = !psx_mod_option_value(Layout::Package, "draw-distance", "distance", value, sizeof value) ||
                   std::strcmp(value, "original") != 0;
        arena = extended ? psx_mod_alloc_gpu_dma_memory(2 * ArenaSize, 8) : 0;
        peak = 0;
        packet_corrections = 0;
    }
    static void register_plugins() {
        psx_mod_register_activation_plugin(Layout::Plugin, activate);
        psx_mod_register_function_entry_plugin(Layout::Plugin, Layout::FogPc, fog);
        psx_mod_register_function_entry_plugin(Layout::Plugin, Layout::SwapPc, swap);
        psx_mod_register_function_filter_plugin(Layout::Plugin, Layout::LimitPc, limit);
        psx_mod_register_function_entry_plugin(Layout::Plugin, Layout::SubmitPc, submit);
        for (const auto& site : Layout::PacketLengthSites)
            psx_mod_register_instruction_plugin(Layout::Plugin, site.address, site.word,
                site.word == 0x000A5582u ? packet_length_t2 : packet_length_s2);
        psx_mod_register_instruction_plugin(Layout::PrecisionPlugin, Layout::PrecisionPc,
            0x4819B000u, subdivision_precision);
    }
};

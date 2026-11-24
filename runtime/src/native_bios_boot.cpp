#include "native_bios_boot.h"
#include "native_bios_services.h"

#include "cpu_state.h"
#include "disc_boot_image.h"
#include "psx_memory.h"

#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

// PSX kernel globals and structure sizes, also used by the native event and
// thread services. The pinned OpenBIOS source (kernel/globals.h,
// common/kernel/threads.h and kernel/events.h) documents this guest ABI.
constexpr uint32_t kHeapBegin = 0xe000;
constexpr uint32_t kKernelEnd = 0x10000;
constexpr uint32_t kThreadBytes = 0xc0;
constexpr uint32_t kEventBytes = 0x1c;
constexpr uint32_t kHandlerBytes = 8;
constexpr uint32_t kHandlerCount = 4;

bool user_ram_span(uint32_t address, uint32_t bytes, uint32_t *offset = nullptr) {
    const uint32_t segment = address & 0xe0000000u;
    if (segment != 0 && segment != 0x80000000u && segment != 0xa0000000u)
        return false;
    uint32_t physical;
    if (!psx_ram_resolve(address, bytes, &physical) || physical < kKernelEnd)
        return false;
    if (offset) *offset = physical;
    return true;
}

void write_payload(uint32_t address, const uint8_t *bytes, uint32_t size) {
    uint32_t i = 0;
    for (; i + 4 <= size; i += 4) {
        const uint32_t word = uint32_t(bytes[i]) | (uint32_t(bytes[i + 1]) << 8) |
            (uint32_t(bytes[i + 2]) << 16) | (uint32_t(bytes[i + 3]) << 24);
        psx_host_write_word(address + i, word);
    }
    for (; i < size; ++i) psx_host_write_byte(address + i, bytes[i]);
}

void clear_span(uint32_t address, uint32_t size) {
    uint32_t i = 0;
    for (; i + 4 <= size; i += 4) psx_host_write_word(address + i, 0);
    for (; i < size; ++i) psx_host_write_byte(address + i, 0);
}

} // namespace

bool psx_prepare_native_bios_boot(CPUState &cpu, const std::string &disc,
                                 std::string &error) {
    uint32_t bytes = 0;
    std::string name;
    std::optional<PsxDiscBootHeader> header;
    std::optional<PsxDiscSystemConfig> config;
    std::unique_ptr<uint8_t, decltype(&std::free)> payload(
        psx_read_disc_boot_image("", disc, &bytes, &name, &header, &config), &std::free);
    if (!payload || !header || !config) {
        error = "cannot read a valid disc boot executable and SYSTEM.CNF";
        return false;
    }

    const uint64_t tables_size = uint64_t(kHandlerCount) * kHandlerBytes +
        sizeof(uint32_t) + uint64_t(config->threads) * kThreadBytes +
        uint64_t(config->events) * kEventBytes;
    if (!config->threads || !config->events || tables_size > kKernelEnd - kHeapBegin) {
        error = "SYSTEM.CNF kernel tables do not fit in native kernel RAM";
        return false;
    }
    if ((header->text_address & 3u) || (header->fill_size && (header->fill_address & 3u)) ||
        !user_ram_span(header->text_address, header->text_size) ||
        (header->fill_size && !user_ram_span(header->fill_address, header->fill_size)) ||
        (config->stack & 3u) || config->stack < 4 || !user_ram_span(config->stack - 4, 4)) {
        error = "disc executable or initial stack lies outside contiguous game RAM";
        return false;
    }

    // The SDK boot operation uses SYSTEM.CNF STACK for the initial SP/FP.
    // Kernel globals point to the native provider's guest-resident tables;
    // no copied firmware instructions are needed for these structures.
    const uint32_t handlers = kHeapBegin;
    const uint32_t process = handlers + kHandlerCount * kHandlerBytes;
    const uint32_t threads = process + sizeof(uint32_t);
    const uint32_t events = threads + config->threads * kThreadBytes;
    clear_span(kHeapBegin, static_cast<uint32_t>(tables_size));
    psx_host_write_word(0x60, psx_ram_live_bytes() >> 20);
    psx_host_write_word(0x64, 0);
    psx_host_write_word(0x68, 0xff);
    // The standard vector points to a designated native entry. A title may
    // replace this vector, as it can with firmware boot.
    psx_host_write_word(0x80, 0x3c1a8000u);  // lui k0, 0x8000
    psx_host_write_word(0x84, 0x275a0190u);  // addiu k0, k0, 0x190
    psx_host_write_word(0x88, 0x03400008u);  // jr k0
    psx_host_write_word(0x8c, 0);
    psx_host_write_word(PSX_NATIVE_EXCEPTION_JUMP_PTR, 0);
    psx_host_write_word(PSX_NATIVE_SIO_AUTO_ACK, 0);
    for (uint32_t timer = 0; timer < 4; ++timer)
        psx_host_write_word(PSX_NATIVE_TIMER_AUTO_ACK + timer * 4, 1);
    psx_host_write_word(0x100, handlers);
    psx_host_write_word(0x104, kHandlerCount * kHandlerBytes);
    psx_host_write_word(0x108, process);
    psx_host_write_word(0x10c, sizeof(uint32_t));
    psx_host_write_word(0x110, threads);
    psx_host_write_word(0x114, config->threads * kThreadBytes);
    psx_host_write_word(0x120, events);
    psx_host_write_word(0x124, config->events * kEventBytes);
    psx_host_write_word(process, threads);
    for (uint32_t i = 0; i < config->threads; ++i)
        psx_host_write_word(threads + i * kThreadBytes, i ? 0x1000u : 0x4000u);

    write_payload(header->text_address, payload.get(), header->text_size);
    if (header->fill_size) clear_span(header->fill_address, header->fill_size);

    std::memset(cpu.gpr, 0, sizeof(cpu.gpr));
    cpu.gpr[4] = 1;  // boot argc
    cpu.gpr[28] = header->global_pointer;
    cpu.gpr[29] = config->stack;
    cpu.gpr[30] = config->stack;
    cpu.gpr[31] = PSX_NATIVE_BOOT_RETURN;
    cpu.hi = cpu.lo = 0;
    // Boot enters its executable from the kernel's critical section with
    // the hardware IRQ masked at the CPU and COP2 available.
    cpu.cop0[12] = 0x40000400u;
    cpu.cop0[13] = 0;
    cpu.pc = header->entry_pc;
    error.clear();
    return true;
}

#pragma once

#include "native_bios_services.h"

#include <cstdint>
#include <string>

struct CPUState;

/* Load the configured disc executable directly and initialize the native
 * kernel's guest tables and initial register state. Call only after RAM,
 * device and text-image guard setup. Invalid inputs leave guest state intact.
 * This prepares the launch state; the native kernel service provider owns
 * interrupt and vector dispatch. */
bool psx_prepare_native_bios_boot(CPUState &cpu, const std::string &disc,
                                 std::string &error);

constexpr uint32_t PSX_NATIVE_BOOT_RETURN = 0x80000000u | PSX_NATIVE_BOOT_RETURN_PHYS;

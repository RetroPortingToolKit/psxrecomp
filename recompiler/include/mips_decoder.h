#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include "ps1_exe_parser.h"
#include "mips_recomp/decoder.h"
namespace PSXRecomp {
using InstrFormat=mips_recomp::InstrFormat;
using DecodedInstruction=mips_recomp::DecodedInstruction;
class MipsDecoder {
public:
    // Decode a single instruction word at a given address
    // address is used to compute branch/jump targets
    static DecodedInstruction decode(uint32_t instr, uint32_t address = 0);

    // Decode all instructions in a PS1 executable sequentially
    static std::vector<DecodedInstruction> decode_executable(
        const PS1Executable& exe
    );

    // Compute branch target: address + 4 + (imm16 << 2)
    static uint32_t compute_branch_target(uint32_t address, int16_t imm16);

    // Compute jump target: (address & 0xF0000000) | (target26 << 2)
    static uint32_t compute_jump_target(uint32_t address, uint32_t target26);

    // Check if instruction is a NOP (0x00000000 = SLL $zero,$zero,0)
    static bool is_nop(uint32_t instr);

    // Get MIPS register name string ("$zero", "$at", "$v0", etc.)
    static const char* register_name(uint8_t reg);

    // Get mnemonic string for a decoded instruction
    static const char* get_mnemonic(const DecodedInstruction& instr);

};

} // namespace PSXRecomp

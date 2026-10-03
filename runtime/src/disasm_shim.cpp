#include "psx_disasm.h"
#include "mips_decoder.h"

#include <cstdio>
#include <cstring>

/* C-linkage bridge so the C debug server can use the C++ MIPS decoder that the
 * recompiler already ships (recompiler/src/mips_decoder.cpp). */

/* GTE data registers (COP2 rd index) — psx-spx. */
static const char* const kCop2Data[32] = {
    "VXY0", "VZ0", "VXY1", "VZ1", "VXY2", "VZ2", "RGB", "OTZ",
    "IR0", "IR1", "IR2", "IR3", "SXY0", "SXY1", "SXY2", "SXYP",
    "SZ0", "SZ1", "SZ2", "SZ3", "RGB0", "RGB1", "RGB2", "RES1",
    "MAC0", "MAC1", "MAC2", "MAC3", "IRGB", "ORGB", "LZCS", "LZCR"
};

/* GTE control registers (COP2 rd index) — psx-spx. */
static const char* const kCop2Ctrl[32] = {
    "RT11RT12", "RT13RT21", "RT22RT23", "RT31RT32", "RT33", "TRX", "TRY", "TRZ",
    "L11L12", "L13L21", "L22L23", "L31L32", "L33", "RBK", "GBK", "BBK",
    "LR1LR2", "LR3LG1", "LG2LG3", "LB1LB2", "LB3", "RFC", "GFC", "BFC",
    "OFX", "OFY", "H", "DQA", "DQB", "ZSF3", "ZSF4", "FLAG"
};

static const char* gte_cmd_name(uint32_t raw) {
    switch (raw & 0x3Fu) {
        case 0x01: return "RTPS";
        case 0x06: return "NCLIP";
        case 0x0C: return "OP";
        case 0x10: return "DPCS";
        case 0x11: return "INTPL";
        case 0x12: return "MVMVA";
        case 0x13: return "NCDS";
        case 0x14: return "CDP";
        case 0x16: return "NCDT";
        case 0x1B: return "NCCS";
        case 0x1C: return "CC";
        case 0x1E: return "NCS";
        case 0x20: return "NCT";
        case 0x28: return "SQR";
        case 0x29: return "DCPL";
        case 0x2A: return "DPCT";
        case 0x2D: return "AVSZ3";
        case 0x2E: return "AVSZ4";
        case 0x30: return "RTPT";
        case 0x3D: return "GPF";
        case 0x3E: return "GPL";
        case 0x3F: return "NCCT";
        default:   return "GTE";
    }
}

extern "C" int psx_disasm_one(uint32_t word, uint32_t addr, char* out, int cap) {
    if (!out || cap <= 0) return 0;
    using PSXRecomp::DecodedInstruction;
    using PSXRecomp::InstrFormat;
    using PSXRecomp::MipsDecoder;

    DecodedInstruction d = MipsDecoder::decode(word, addr);
    const char* m = d.mnemonic ? d.mnemonic : "???";
    auto rn = [](uint8_t r) { return MipsDecoder::register_name(r); };
    uint8_t cop = d.rd & 0x1F;

    int n;
    if (d.format == InstrFormat::UNKNOWN) {
        n = std::snprintf(out, (size_t)cap, ".word 0x%08X", word);
    } else if (std::strcmp(m, "JR") == 0) {
        n = std::snprintf(out, (size_t)cap, "%-8s %s", m, rn(d.rs));
    } else if (std::strcmp(m, "JALR") == 0) {
        n = std::snprintf(out, (size_t)cap, "%-8s %s, %s", m, rn(d.rd), rn(d.rs));
    } else if (std::strcmp(m, "SLL") == 0 || std::strcmp(m, "SRL") == 0 ||
               std::strcmp(m, "SRA") == 0) {
        n = std::snprintf(out, (size_t)cap, "%-8s %s, %s, %d", m, rn(d.rd), rn(d.rt),
                          d.shamt);
    } else if (std::strcmp(m, "MFHI") == 0 || std::strcmp(m, "MFLO") == 0) {
        n = std::snprintf(out, (size_t)cap, "%-8s %s", m, rn(d.rd));
    } else if (std::strcmp(m, "MTHI") == 0 || std::strcmp(m, "MTLO") == 0) {
        n = std::snprintf(out, (size_t)cap, "%-8s %s", m, rn(d.rs));
    } else if (std::strcmp(m, "SYSCALL") == 0 || std::strcmp(m, "BREAK") == 0) {
        n = std::snprintf(out, (size_t)cap, "%s", m);
    } else if (std::strcmp(m, "MULT") == 0 || std::strcmp(m, "MULTU") == 0 ||
               std::strcmp(m, "DIV") == 0 || std::strcmp(m, "DIVU") == 0) {
        n = std::snprintf(out, (size_t)cap, "%-8s %s, %s", m, rn(d.rs), rn(d.rt));
    } else if (std::strcmp(m, "SLLV") == 0 || std::strcmp(m, "SRLV") == 0 ||
               std::strcmp(m, "SRAV") == 0) {
        n = std::snprintf(out, (size_t)cap, "%-8s %s, %s, %s", m, rn(d.rd), rn(d.rt), rn(d.rs));
    } else if (d.format == InstrFormat::COP0) {
        if (std::strcmp(m, "MFC0") == 0 || std::strcmp(m, "MTC0") == 0)
            n = std::snprintf(out, (size_t)cap, "%-8s %s, $%u", m, rn(d.rt), (unsigned)cop);
        else if (std::strcmp(m, "RFE") == 0)
            n = std::snprintf(out, (size_t)cap, "%s", m);
        else
            n = std::snprintf(out, (size_t)cap, ".word 0x%08X", word);
    } else if (d.format == InstrFormat::COP2) {
        if (std::strcmp(m, "MFC2") == 0 || std::strcmp(m, "MTC2") == 0) {
            n = std::snprintf(out, (size_t)cap, "%-8s %s, $%s", m, rn(d.rt),
                              kCop2Data[cop]);
        } else if (std::strcmp(m, "CFC2") == 0 || std::strcmp(m, "CTC2") == 0) {
            n = std::snprintf(out, (size_t)cap, "%-8s %s, $%s", m, rn(d.rt),
                              kCop2Ctrl[cop]);
        } else if (d.rs & 0x10) { /* GTE command word */
            n = std::snprintf(out, (size_t)cap, "%-8s 0x%07X", gte_cmd_name(d.raw),
                              d.raw & 0x1FFFFFFu);
        } else {
            n = std::snprintf(out, (size_t)cap, ".word 0x%08X", word);
        }
    } else if (d.format == InstrFormat::J) {
        n = std::snprintf(out, (size_t)cap, "%-8s 0x%08X", m, d.jump_target);
    } else if (d.is_branch && d.opcode != 0x04 && d.opcode != 0x05) {
        n = std::snprintf(out, (size_t)cap, "%-8s %s, 0x%08X", m, rn(d.rs), d.branch_target);
    } else if (d.is_branch) {
        n = std::snprintf(out, (size_t)cap, "%-8s %s, %s, 0x%08X", m, rn(d.rs),
                          rn(d.rt), d.branch_target);
    } else if (d.opcode == 0x0F) { /* LUI */
        n = std::snprintf(out, (size_t)cap, "%-8s %s, 0x%04X", m, rn(d.rt),
                          (unsigned)d.uimm16);
    } else if (d.is_load || d.is_store) {
        const char* reg = (d.opcode == 0x32 || d.opcode == 0x3A) ? kCop2Data[d.rt & 31] : rn(d.rt);
        const char* prefix = (d.opcode == 0x32 || d.opcode == 0x3A) ? "$" : "";
        n = std::snprintf(out, (size_t)cap, "%-8s %s%s, %d(%s)", m, prefix, reg,
                          (int)d.imm16, rn(d.rs));
    } else if (d.format == InstrFormat::R || d.format == InstrFormat::SPECIAL) {
        n = std::snprintf(out, (size_t)cap, "%-8s %s, %s, %s", m, rn(d.rd), rn(d.rs),
                          rn(d.rt));
    } else if (d.opcode >= 0x0C && d.opcode <= 0x0E) {
        n = std::snprintf(out, (size_t)cap, "%-8s %s, %s, 0x%04X", m, rn(d.rt), rn(d.rs), (unsigned)d.uimm16);
    } else {
        n = std::snprintf(out, (size_t)cap, "%-8s %s, %s, %d", m, rn(d.rt), rn(d.rs),
                          (int)d.imm16);
    }
    if (n < 0) { out[0] = '\0'; return 0; }
    return (n < cap) ? n : (cap - 1);
}

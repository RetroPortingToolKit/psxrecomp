#include "mips_decoder.h"
namespace PSXRecomp {
DecodedInstruction MipsDecoder::decode(uint32_t word,uint32_t pc){return mips_recomp::MipsDecoder::decode(word,pc);}
uint32_t MipsDecoder::compute_branch_target(uint32_t pc,int16_t offset){return mips_recomp::MipsDecoder::compute_branch_target(pc,offset);}
uint32_t MipsDecoder::compute_jump_target(uint32_t pc,uint32_t target){return mips_recomp::MipsDecoder::compute_jump_target(pc,target);}
bool MipsDecoder::is_nop(uint32_t word){return mips_recomp::MipsDecoder::is_nop(word);}
const char* MipsDecoder::register_name(uint8_t reg){return mips_recomp::MipsDecoder::register_name(reg);}
const char* MipsDecoder::get_mnemonic(const DecodedInstruction& instruction){return mips_recomp::MipsDecoder::get_mnemonic(instruction);}
std::vector<DecodedInstruction> MipsDecoder::decode_executable(
    const PS1Executable& exe)
{
    std::vector<DecodedInstruction> result;
    const uint32_t word_count = exe.code_size() / 4;
    result.reserve(word_count);

    for (uint32_t addr = exe.load_address(); addr < exe.end_address(); addr += 4) {
        auto word = exe.read_word(addr);
        if (!word.has_value()) break;
        result.push_back(decode(*word, addr));
    }

    return result;
}


} // namespace PSXRecomp

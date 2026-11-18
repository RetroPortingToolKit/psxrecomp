// Regression: a PS-X EXE read straight from a disc image is sector-padded
// (ISO 9660 records / CD extents), so the file is longer than the program its
// header declares. The BIOS loader reads exactly header.file_size bytes; the
// parser must bound the input the same way instead of rejecting it, or the
// owned-input setup can never feed the boot program from the player's disc
// (Azure Dreams SLUS_006.14: 524288 bytes on disc, 346112-byte program).
// A file shorter than the header declares is still an error.
#include "ps1_exe_parser.h"

using PSXRecomp::PS1ExeHeader;
using PSXRecomp::PS1ExeParser;

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static std::vector<uint8_t> make_exe(uint32_t program_bytes, size_t total_bytes) {
    std::vector<uint8_t> buffer(total_bytes, 0xEE);
    PS1ExeHeader header;
    std::memset(&header, 0, sizeof(header));
    std::memcpy(header.magic, "PS-X EXE", 8);
    header.initial_pc = 0x80010000;
    header.load_address = 0x80010000;
    header.file_size = program_bytes;
    header.initial_sp = 0x801FFF00;
    std::memcpy(buffer.data(), &header, sizeof(header));
    // Distinguishable program bytes so a mis-bounded copy is visible.
    for (uint32_t i = 0; i < program_bytes && 2048 + i < total_bytes; ++i)
        buffer[2048 + i] = static_cast<uint8_t>(i & 0xFF);
    return buffer;
}

static int fail(const char* what) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    return 1;
}

int main() {
    const uint32_t program = 8192;
    std::string error;

    // Exact-length file: unchanged behaviour.
    auto exact = PS1ExeParser::parse_buffer(make_exe(program, 2048 + program), error);
    if (!exact) return fail(("exact-length EXE rejected: " + error).c_str());
    if (exact->code_data.size() != program) return fail("exact-length code size");

    // Sector-padded file (what a disc extraction yields): accepted, bounded to
    // the header length, and the program bytes are the same as the exact file.
    auto padded = PS1ExeParser::parse_buffer(make_exe(program, 2048 + program + 2048 * 3 + 100), error);
    if (!padded) return fail(("sector-padded EXE rejected: " + error).c_str());
    if (padded->code_data.size() != program) return fail("padded code size not bounded to header length");
    if (padded->code_data != exact->code_data) return fail("padded program bytes differ from exact program bytes");
    if (padded->header.file_size != program) return fail("padded header file_size changed");

    // Truncated file: still rejected, and the message names the mismatch.
    error.clear();
    auto truncated = PS1ExeParser::parse_buffer(make_exe(program, 2048 + program - 4), error);
    if (truncated) return fail("truncated EXE accepted");
    if (error.find("File size mismatch") == std::string::npos) return fail("truncated EXE error message changed");

    std::puts("PASS: sector-padded PS-X EXE bounded to header length; truncated EXE rejected");
    return 0;
}

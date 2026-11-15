#include "../src/pgxp_dataflow.h"
#include <cstdio>
#include <cstdlib>
using namespace PSXRecomp;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#c);std::abort(); } } while(0)
static uint32_t i(unsigned op,unsigned rs,unsigned rt,unsigned imm=0) {return op<<26|rs<<21|rt<<16|imm;}
static uint32_t r(unsigned rs,unsigned rt,unsigned rd,unsigned fn,unsigned sh=0) {return rs<<21|rt<<16|rd<<11|sh<<6|fn;}
int main() {
    // GTE -> split halves -> arithmetic -> repack -> memory: every precision
    // dependency remains, including an in-place destination/source alias.
    PgxpFlowBlock geometry{0,{{0,0x48087000},{4,r(0,8,9,3,16)},
        {8,i(9,9,9,1)},{12,r(0,9,9,0,16)},{16,r(8,9,8,0x25)},
        {20,i(0x2B,4,8)}},{},true};
    CHECK(pgxp_dead_alu_flow({geometry}).empty());
    // Two paths overwrite the control temporary before their common caller
    // boundary. Integer branches consume values, not coordinate metadata.
    std::vector<PgxpFlowBlock> diamond{
        {0,{{0,i(0x0F,0,10,7)},{4,i(4,8,0)}},{16,32}},
        {16,{{16,i(9,0,10,2)}},{48}},
        {32,{{32,i(9,0,10,3)}},{48}},
        {48,{{48,i(9,0,10,4)}},{},true}};
    auto dead=pgxp_dead_alu_flow(diamond);
    CHECK(dead==std::set<uint32_t>({0,16,32}));
    // One path publishes the old value: the reaching definition must survive.
    diamond[2].instructions={{32,i(0x2B,4,10)}};
    dead=pgxp_dead_alu_flow(diamond);CHECK(!dead.count(0));
    // Loop-carried coordinate arithmetic is live; a boolean used only by the
    // loop's integer branch is shadow-dead once both exits overwrite it.
    std::vector<PgxpFlowBlock> loop{
        {0,{{0,i(9,8,8,1)},{4,i(0x0A,8,10,4)},{8,i(5,10,0)}},{0,16}},
        {16,{{16,i(0x2B,4,8)},{20,i(9,0,10,0)}},{},true}};
    dead=pgxp_dead_alu_flow(loop);CHECK(dead==std::set<uint32_t>({4}));
    // Unknown code and explicit mod/caller boundaries may inspect every GPR.
    PgxpFlowBlock opaque{0,{{0,i(9,0,8,1)},{4,0,true},{8,i(9,0,8,2)}},{},true};
    CHECK(pgxp_dead_alu_flow({opaque}).empty());
    PgxpFlowBlock external{0,{{0,i(9,0,8,1)}},{0xDEAD}};
    CHECK(pgxp_dead_alu_flow({external}).empty());
    // Protected delay clones and source-validating HI/LO operations stay full.
    PgxpFlowBlock delay{0,{{0,i(9,0,8,1),false,false},{4,i(9,0,8,2)}},{},true};
    CHECK(pgxp_dead_alu_flow({delay}).empty());
    PgxpFlowBlock hilo{0,{{0,r(8,0,0,0x11)},{4,r(0,0,9,0x10)},
        {8,i(9,0,9,0)}},{},true};
    CHECK(pgxp_dead_alu_flow({hilo}).empty());
    CHECK(!pgxp_flow_effect(i(0x23,4,8)).pure_alu);
    CHECK(!pgxp_flow_effect(i(0x2B,4,8)).pure_alu);
    PgxpFlowBlock partial{0,{{0,r(9,0,8,0x21)},{4,i(0x22,4,8,1)},
        {8,i(0x26,4,8,2)},{12,i(0x2B,4,8)}},{},true};
    CHECK(pgxp_dead_alu_flow({partial}).empty()); // pre-merge rt is a precision input
    CHECK(pgxp_flow_effect(i(0x22,4,8,1)).uses==(UINT64_C(1)<<8));
    CHECK(pgxp_flow_effect(i(0x26,4,8,2)).uses==(UINT64_C(1)<<8));
    std::puts("PASS: precision CFG joins/loops/GTE arithmetic/aliases/unknown edges/delay/HI-LO");
}

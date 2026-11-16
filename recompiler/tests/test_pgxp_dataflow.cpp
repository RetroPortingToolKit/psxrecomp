#include "../src/pgxp_dataflow.h"
#include "../src/pgxp_operation_flow.h"
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
    // Remove the intermediate metadata lifetimes completely. The final full
    // write remains, even if all guest words happen to be equal coordinates.
    PgxpFlowBlock lifetimes{0,{{0,i(9,0,8,7)},{4,r(8,0,8,0x21)},
        {8,i(0x0F,0,8,0x1234)}},{},true};
    dead=pgxp_dead_alu_flow({lifetimes});
    CHECK(dead==std::set<uint32_t>({0,4}));
    CHECK(pgxp_elided_alu_flow({lifetimes},dead)==dead);
    // A load or explicit observer can inspect the old/equal-word shadow:
    // retain its destructive queue write before those actual boundaries.
    PgxpFlowBlock observed{0,{{0,i(9,0,8,7)},{4,i(0x23,4,9)},
        {8,i(0x0F,0,8,0x1234)}},{},true};
    dead=pgxp_dead_alu_flow({observed});CHECK(dead.count(0));
    CHECK(pgxp_elided_alu_flow({observed},dead).empty());
    observed.instructions[1]={4,i(9,0,9,1),true};
    CHECK(pgxp_elided_alu_flow({observed},{0}).empty());
    // A CFG join is an observation boundary even when the function-wide
    // demand proof found the original write dead along every successor.
    CHECK(pgxp_elided_alu_flow(diamond,pgxp_dead_alu_flow(diamond)).empty());
    CHECK(pgxp_elided_alu_flow({geometry},pgxp_dead_alu_flow({geometry})).empty());
    // Entire integer operation: canonical memory admission, no precision
    // operand, exact LUI and one final plain GPR receipt after all arithmetic.
    std::vector<PgxpFlowInstruction> integer{{0,i(15,0,12,0x8000)},
        {4,i(35,12,2,0x2000)},{8,0},{12,i(9,2,2,7)},
        {16,i(10,2,3,0x7fff)},{20,0x03e00008},{24,0}};
    auto operation=pgxp_integer_operation(integer);
    CHECK(operation.valid && operation.reads==0 && operation.loads==1 && operation.alu==3);
    CHECK(operation.exact==(1u<<12) && operation.plain==((1u<<2)|(1u<<3)));
    CHECK(operation.memory.size()==1 && operation.memory[0].address=="2147491840u");
    auto mmio=integer;mmio[0]={0,i(15,0,12,0x1f80)};
    CHECK(!pgxp_integer_operation(mmio).valid); // known MMIO never adds an always-refused path
    auto hazardous=integer;hazardous[2]={8,i(9,2,2,1)};
    CHECK(!pgxp_integer_operation(hazardous).valid); // architectural load-delay input
    auto transported=integer;transported.insert(transported.end()-2,{19,i(43,4,2)});
    CHECK(pgxp_integer_operation(transported).valid); // static native store barrier is a no-op
    auto scalar=integer;scalar[0]={0,r(8,9,0,0x18)};
    CHECK(!pgxp_integer_operation(scalar).valid); // caller-live HI/LO not guessed plain
    auto observer=integer;observer[0].opaque=true;
    CHECK(!pgxp_integer_operation(observer).valid);
    // Real interprocedural roots: a no-GTE callee may store the caller's GTE
    // output. Memory transport and unknown calls remain distinct from geometry.
    PgxpOperationFunction caller;caller.address=0;
    caller.blocks={{0,{{0,0x48087000},{4,0x0c000040},{8,0x03e00008}},{},true}};
    PgxpOperationFunction callee;callee.address=0x100;
    callee.blocks={{0x100,{{0x100,i(43,4,8)},{0x104,0x03e00008}},{},true}};
    auto summaries=pgxp_operation_summaries({caller,callee});
    CHECK(summaries.at(0).stored&PgxpGteRoot);
    CHECK(!summaries.at(0x100).gte && summaries.at(0x100).stored==(UINT64_C(1)<<8));
    callee.blocks[0].instructions={{0x100,i(35,5,8)},{0x104,0},
        {0x108,i(43,4,8)},{0x10c,0x03e00008},{0x110,0}};
    summaries=pgxp_operation_summaries({caller,callee});
    CHECK(summaries.at(0x100).stored&PgxpMemoryRoot); // no-GTE LW/SW is NOT plain
    caller.blocks[0].instructions[1].word=0x0c000080;
    CHECK(pgxp_operation_summaries({caller,callee}).at(0).opaque);
    std::puts("PASS: precision CFG joins/loops/GTE arithmetic/aliases/unknown edges/delay/HI-LO/lifetime boundaries");
}

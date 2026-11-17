#include "../src/pgxp_dataflow.h"
#include "../src/pgxp_operation_flow.h"
#include "../src/pgxp_demand_regions.h"
#include <cstdio>
#include <cstdlib>
using namespace PSXRecomp;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#c);std::abort(); } } while(0)
static uint32_t i(unsigned op,unsigned rs,unsigned rt,unsigned imm=0) {return op<<26|rs<<21|rt<<16|imm;}
static uint32_t r(unsigned rs,unsigned rt,unsigned rd,unsigned fn,unsigned sh=0) {return rs<<21|rt<<16|rd<<11|sh<<6|fn;}
// Pure CFG proofs use the normal C++ exception/unwind profile, not the
// runtime receipt fixture. Keep these assertions separate from memory effects.
static void geometry_control_demand() {
    using namespace PSXRecomp;
    constexpr uint64_t all=((UINT64_C(1)<<34)-1u)&~UINT64_C(1);
    constexpr uint64_t control=(UINT64_C(1)<<29)|(UINT64_C(1)<<31);
    const std::vector<PgxpFlowBlock> blocks={
        {0x1000,{{0x1000,0x27bdffe8},{0x1004,0xafbf0010},
                 {0x1008,0x48087000},{0x100c,0xac880000}}, {0x1010}},
        {0x1010,{{0x1010,0x0c000800}}, {0x1018},true},
        {0x1018,{{0x1018,0x8fbf0010},{0x101c,0},
                 {0x1020,0x03e00008},{0x1024,0x27bd0018}}, {},true}};
    const auto frame=pgxp_control_frame(blocks,0x1000);
    CHECK(frame.valid && frame.bytes==24 && frame.offset==16);
    const auto demand=pgxp_geometry_demand(blocks,0x1000,all&~control,{{0x2000,all&~control}});
    for(auto pc:frame.sites())CHECK(demand.dead.count(pc));
    CHECK(!demand.dead.count(0x1008) && !demand.dead.count(0x100c)); // GTE -> packet
    // Unknown callees and actual SP-as-coordinate/opaque observers are roots.
    const auto unknown=pgxp_geometry_demand(blocks,0x1000,all&~control,{});
    CHECK(!unknown.dead.count(0x1000));
    auto escaped=blocks;escaped[0].instructions[2].word=0x489d4800; // MTC2 SP,IR1
    CHECK(!pgxp_control_frame(escaped,0x1000).valid);
    auto observed=blocks;observed[0].instructions[0].opaque=true;
    CHECK(!pgxp_control_frame(observed,0x1000).valid);
    auto unbalanced=blocks;unbalanced[2].instructions.back().word=0x27bd0014;
    CHECK(!pgxp_control_frame(unbalanced,0x1000).valid);
}
static void scratch_operand_demand() {
    PgxpRegionState state{};unsigned dst=0;PgxpRegionValue value;
    CHECK(pgxp_region_definition(0x3c0e1f80u,state,dst,value,true));state[dst]=value;
    // A real frame save publishes pending receipts but cannot change t6's
    // proven native integer. This fact is separate from pending precision.
    pgxp_region_publish_facts(state);CHECK(!state[14].known&&state[14].constant);
    CHECK(pgxp_region_step({0x1000,0x91c202b4u},state,true)); // LBU v0,2B4(t6)
    CHECK(state[2].bounded&&state[2].maximum==255&&!state[2].flags[1]);
    CHECK(pgxp_region_step({0x1004,0x00021080u},state,true)); // SLL v0,2
    CHECK(state[2].maximum==1020);
    CHECK(pgxp_region_step({0x1008,0x004e1021u},state,true)); // ADDU v0,t6
    CHECK(state[2].minimum==0x1f800000u&&state[2].maximum==0x1f8003fcu);
    CHECK(state[2].flags[0]==0&&state[2].flags[1]==3);
    uint32_t publish=~0u;
    CHECK(pgxp_region_operand_boundary(0xc8460000u,state,&publish)&&!publish); // LWC2 gte6,(v0)
    CHECK(state[2].known); // EA is not a precision operand
    CHECK(!pgxp_region_scratch_memory(0xc8460004u,state)); // full word crosses scratch end
    CHECK(pgxp_region_operand_boundary(0xadC20000u,state,&publish)&&(publish&(1u<<2))); // SW v0,(t6)
    CHECK(!state[2].known&&state[14].constant);
    // Crossing scratch end, partial merges and MMIO are full boundaries.
    CHECK(!pgxp_region_operand_boundary(0x89c20001u,state));
    state[14].value=0x1f810000u;CHECK(!pgxp_region_scratch_memory(0x91c20000u,state));
    // A loaded canonical source must first consume an old deferred alias.
    state={};state[14].known=true;state[14].constant=true;state[14].value=0x1f800000u;
    state[3].known=true;state[3].incoming_add=true;state[3].source=2;state[3].addend=1;
    CHECK(pgxp_region_operand_boundary(0x8dc20000u,state,&publish)&&(publish&(1u<<3)));
    CHECK(!state[2].known&&!state[3].known);
    // A loop's unvisited backedge is not an unknown value. The dominating
    // LUI survives a quiet backedge; a real overwrite/opaque entry ends it.
    const std::vector<PgxpFlowBlock> loop={
        {0x1000,{{0x1000,0x3c0e1f80u}},{0x1010}},
        {0x1010,{{0x1010,0x91c202b4u},{0x1014,0x00021080u},{0x1018,0x004e1021u},
                   {0x101c,0xc8460000u}},{0x1010,0x1020}},
        {0x1020,{}, {},true}};
    const auto plan=pgxp_region_plan(loop,{0x1000},{},true);
    CHECK(plan.entry.at(0x1010)[14].constant&&plan.definitions>=4);
    auto clobbered=loop;clobbered[1].instructions.insert(clobbered[1].instructions.begin(),{0x100c,0x01a07021u});
    CHECK(!pgxp_region_plan(clobbered,{0x1000},{},true).entry.at(0x1020)[14].constant);
    auto opaque=loop;opaque[1].instructions.insert(opaque[1].instructions.begin(),{0x100c,0x0c000800u,true});
    CHECK(!pgxp_region_plan(opaque,{0x1000},{},true).entry.at(0x1020)[14].constant);
    const auto canonical=pgxp_region_plan(loop,{0x1000},{},false);
    CHECK(!canonical.entry.at(0x1020)[2].known);
}
int main() {
    geometry_control_demand();scratch_operand_demand();
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
    // Incoming affine receipts cannot survive a CFG edge that publishes an
    // equal-bit replacement of their old canonical source. Keep this compiler
    // analysis check in its normal C++ target, outside runtime no-unwind tests.
    {
        const std::vector<PSXRecomp::PgxpFlowBlock> join={
            {0x1000,{{0x1000,0x24620001u,false}},{0x1010,0x1020}},
            {0x1010,{{0x1010,0x3c038000u,false}},{0x1030}},
            {0x1020,{}, {0x1030}}, {0x1030,{}, {}}};
        const auto plan=PSXRecomp::pgxp_region_plan(join,{0x1000});
        CHECK(plan.definitions==2 && !plan.entry.at(0x1010)[2].known && !plan.entry.at(0x1030)[2].known);
    }
    std::puts("PASS: precision CFG joins/loops/GTE arithmetic/aliases/unknown edges/delay/HI-LO/lifetime boundaries");
}

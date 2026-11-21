/* Execute a real emitted vertex loop across IRQ -> ChangeThread -> fresh
 * native-function re-entry. The existing exception fixture owns only guest
 * RAM/kernel dispatch; production interrupts/traps and pipeline are linked. */
#define FX_REAL_LOAD_DELAY 1
#include "rfe_guest_fixture.h"
#include "psx_load_delay.h"

#define ENTRY 0x80010000u
#define LOOP 0x80010008u
#define HANDLER 0x80020000u
#define MAIN 0x80030000u
#define EXIT 0x80040000u
#define DATA 0x80050000u
#define CT_SYS 0x80000650u
#define CT_RET 0x80002104u
#define MAIN_TCB (FX_TCB+0xc0u)
#define SAVE 0x70000u
static unsigned mode_irq, irqs, mains, exits, writes;
static uint32_t log_values[4];

int psx_game_text_native_ok(uint32_t pc) { (void)pc; return 1; }
int psx_mod_try_function_replacement(CPUState *cpu,uint32_t pc) { (void)cpu;(void)pc;return 0; }
uint32_t psx_cyc_load_word(CPUState *cpu,uint32_t addr,uint32_t rt,uint32_t mask) {
    (void)cpu;(void)rt;(void)mask;return fx_read(addr);
}
uint32_t gte_read_data(CPUState *cpu,uint8_t reg) {
    (void)cpu;
    if(reg==17) {
        if(mode_irq && !irqs) { psx_cycle_count+=1000u;i_stat|=8u; }
        return 0x2222u;
    }
    FX_CHECK(reg==18);return 0x3333u;
}
void gte_write_data(CPUState *cpu,uint8_t reg,uint32_t value) {
    (void)cpu;FX_CHECK(reg==0 && writes<4);log_values[writes++]=value;
}

#include PSX_LOAD_DELAY_GENERATED

int psx_syscall(CPUState *,uint32_t);
static void change(CPUState *cpu,uint32_t tcb) {
    cpu->gpr[4]=3;cpu->gpr[5]=tcb;cpu->gpr[31]=CT_RET;cpu->pc=CT_SYS;
    (void)psx_syscall(cpu,0);
    FX_FAIL("ChangeThread returned into its abandoned host frame");
}
static void start(CPUState *cpu) {
    cpu->gpr[4]=DATA;cpu->gpr[9]=0x4444u;
    func_80010000(cpu);
}
static void resume(CPUState *cpu) {
    FX_CHECK(cpu->gpr[8]==0x2222u && irqs==1u && mains==1u);
    func_80010000(cpu);
}
static void handler(CPUState *cpu) {
    ++irqs;i_stat&=~8u;
    /* The exception saved the architectural loaded value, not the C-local
     * old operand. This is the original author's IRQ/TCB handoff gap. */
    FX_CHECK(fx_read(FX_TCB_REG(8))==0x2222u);
    for(uint32_t off=8;off<=0x98;off+=4)fx_write(SAVE+off,fx_read(FX_TCB+off));
    change(cpu,MAIN_TCB);
}
static void main_thread(CPUState *cpu) {
    ++mains;FX_CHECK(irqs==1u && writes==1u);
    for(uint32_t off=8;off<=0x98;off+=4)fx_write(FX_TCB+off,fx_read(SAVE+off));
    change(cpu,FX_TCB);
}
static void exit_thread(CPUState *cpu) {
    FX_CHECK(cpu->gpr[8]==0x2222u && cpu->gpr[9]==0x3333u && cpu->gpr[10]==0x8888u);
    ++exits;cpu->pc=0;
}
static void ct_tail(CPUState *cpu) { cpu->pc=cpu->gpr[31]; }
static void ct_return(CPUState *cpu) { cpu->pc=0; }
static const FxEntry entries[]={
    {FX_VECTOR,fx_k_exception},{FX_K_RET,fx_k_ret},{FX_K_RFE,fx_k_return_from_exception},
    {ENTRY,start},{LOOP,resume},{HANDLER,handler},{MAIN,main_thread},{EXIT,exit_thread},
    {CT_SYS+4,ct_tail},{CT_RET,ct_return}
};
int main(int argc,char **argv) {
    CPUState cpu;(void)argc;(void)argv;
#ifdef FX_NO_IRQ
    mode_irq=0;
#else
    mode_irq=1;
#endif
    fx_boot(&cpu,entries,sizeof entries/sizeof entries[0],HANDLER,ENTRY,0x80147000u,EXIT);
    fx_write(DATA,0x1111u);
    fx_write(0x114,2u*FX_TCB_BYTES);
    fx_write(MAIN_TCB,0x4000);
    fx_write(MAIN_TCB+0x08+29*4,0x801ff000u);
    fx_write(MAIN_TCB+0x88,MAIN);
    fx_write(MAIN_TCB+0x94,0x404);
    i_mask=8u;
    psx_scheduler_run(&cpu);
    FX_CHECK(exits==1u && writes==2u && log_values[0]==0x1111u);
    FX_CHECK(log_values[1]==(mode_irq?0x2222u:0x1111u));
    FX_CHECK(irqs==mode_irq && mains==mode_irq);
    puts("PASS: actual emitted loop, IRQ TCB save, ChangeThread exit and fresh native re-entry");
    return 0;
}

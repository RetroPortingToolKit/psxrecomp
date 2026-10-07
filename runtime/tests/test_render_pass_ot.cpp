#include "render_pass_ot.hpp"
#include <cstdio>
#include <cstdlib>
static uint32_t ram[2*1024*1024/4];
extern "C" {
int g_psx_render_pass_active=1;
uint32_t memory_get_ram_bytes(){return sizeof ram;}
uint32_t psx_mod_read_word(uint32_t p){return ram[(p&0x1FFFFFFF)/4];}
void psx_mod_write_word(uint32_t p,uint32_t v){ram[(p&0x1FFFFFFF)/4]=v;}
// Parser lengths used by these fixtures. Runtime supplies its actual GPU decoder.
int gpu_gp0_command_word_count(uint8_t op) {
    if(op==0x20)return 4;if(op==0xA0)return 3;if(op==0x48)return -1;
    if(op==0 || (op>=0xE1 && op<=0xE6))return 1;
    return 0;
}
}
static void check(bool ok,const char* name){if(!ok){std::fprintf(stderr,"FAIL: %s\n",name);std::exit(1);}}
static void put(uint32_t p,uint32_t v){psx_mod_write_word(p,v);}
static uint32_t get(uint32_t p){return psx_mod_read_word(p);}
int main() {
    constexpr uint32_t slot=0x10000,world=0x20000,late=0x21000,next=0x22000;
    put(slot,world);put(world,0x00FFFFFF);
    PSXOTReplay ot;check(ot.capture(slot,1),"capture heads");
    put(slot,late);put(late,0x01000000|world);put(late+4,0xE1000123);
    check(ot.preserve_late(),"capture late prefix");
    put(slot,next);put(late,0);put(late+4,0);ot.apply_late();
    check(get(slot)==late && get(late)==(0x01000000|next) && get(late+4)==0xE1000123,"late follows regenerated world");
    put(slot,late);put(late,late);check(!ot.preserve_late(),"cycle bounded");
    PSXOTReplay hud;
    check(hud.capture_new_list(slot),"standalone late HUD head");
    put(slot,late);put(late,0x01FFFFFF);put(late+4,0xE1000123);
    check(hud.preserve_late(),"capture complete HUD prefix");
    put(slot,0xFFFFFF);put(late,0);put(late+4,0);hud.apply_late();
    check(get(slot)==late && get(late)==0x01FFFFFF && get(late+4)==0xE1000123,"restore standalone HUD");
    constexpr uint32_t p=0x30000;
    put(p,0x07FFFFFF);put(p+4,0x20010203);put(p+8,0xE3000042);
    put(p+12,0xE4000012);put(p+16,0xE50000AB);
    put(p+20,0xE3000000|(256u<<10));put(p+24,0xE400013F|(495u<<10));
    put(p+28,0xE5000000|(258u<<11));
    check(PSXOTReplay::retarget_y(p,256,0,240),"retarget packet stream");
    check(get(p+8)==0xE3000042 && get(p+12)==0xE4000012 && get(p+16)==0xE50000AB,"coordinate payload is never decoded as opcode");
    check(get(p+20)==0xE3000000 && get(p+24)==(0xE400013F|(239u<<10)) && get(p+28)==(0xE5000000|(2u<<11)),"environment preserves crop and shake");
    put(p,0x02031000);put(p+4,0xE3000000);put(p+8,0xE400013F|(239u<<10));
    put(0x31000,0x01FFFFFF);put(0x31004,0xF1000000);
    check(!PSXOTReplay::retarget_y(p,0,256,240) && get(p+4)==0xE3000000,"invalid tail makes no partial edits");
    put(p,0x07FFFFFF);put(p+4,0xA0000000);put(p+8,0);put(p+12,0x00010002);
    put(p+16,0xE3000123);put(p+20,0xE3000000);put(p+24,0xE400013F|(239u<<10));put(p+28,0xE5000000);
    check(PSXOTReplay::retarget_y(p,0,256,240) && get(p+16)==0xE3000123,"image data is not a command");
    g_psx_render_pass_active=0;
    check(!PSXOTReplay::retarget_y(p,256,0,240),"outside sandbox refused");
    std::puts("ALL PASS");
}

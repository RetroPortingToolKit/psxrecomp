#include "host_osd.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
int psx_rewind_needs_present(void) {return 0;}
int psx_savestate_menu_needs_present(void) {return 0;}
#define CHECK(x) do {if(!(x)) {fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}} while(0)
int main(void) {
    const uint32_t *pixels;int w,h;
    host_osd_set_diagnostics("P1 UP CROSS F1","P2 START F1");
    CHECK(host_osd_image(&pixels,&w,&h) && h==40 && pixels);
    host_osd_set_status("Netplay connected");
    CHECK(host_osd_image(&pixels,&w,&h) && h==56);
    host_osd_push("Game saved",5000);
    CHECK(host_osd_image(&pixels,&w,&h) && h==56);
    // Toast replaces status; both diagnostic rows remain visible below it.
    unsigned white=0;for(int y=24;y<56;++y)for(int x=0;x<w;++x)white+=pixels[y*w+x]==0xFFFFFFFFu;
    CHECK(white>100);
    host_osd_set_diagnostics(NULL,NULL);
    CHECK(host_osd_image(&pixels,&w,&h) && h==24);
    char long_row[160];memset(long_row,'X',sizeof long_row-1);long_row[sizeof long_row-1]=0;
    host_osd_set_diagnostics(long_row,"P2 -");
    CHECK(host_osd_image(&pixels,&w,&h) && h==56 && w<=1032);
    // Without diagnostics a long toast breaks into lines (8x8 font at 2x,
    // 2 pixels between lines) no wider than 38 columns.
    host_osd_set_diagnostics(NULL,NULL);
    host_osd_push("Save state not loaded: it was made by a different build",5000);
    CHECK(host_osd_image(&pixels,&w,&h) && h==(2*2+2*8+2)*2 && w<=(2*2+38*8)*2);
    host_osd_push("Save state not loaded: the file was written by another build of this game",5000);
    CHECK(host_osd_image(&pixels,&w,&h) && h==(2*2+3*8+2*2)*2 && w<=(2*2+38*8)*2);
    host_osd_push("Game saved",5000);
    CHECK(host_osd_image(&pixels,&w,&h) && h==24);
    puts("host OSD diagnostic/status/toast coexistence checks passed");return 0;
}

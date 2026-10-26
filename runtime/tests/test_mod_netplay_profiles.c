#include "mod_netplay.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>
static PSXModNetplayProfile base={"two","two-v1",0,0,1,1,0,0,0,0,0,0,0,0,0,2,0};
static PSXModNetplayProfile four={"four","four-v1",0,0,1,1,"renderer",0,1,"roster","coop","cameras","individual",1,3,4,"p4"};
static const PSXModNetplayProfile *wanted;
static unsigned players=4;
static const PSXModNetplayProfile *select_profile(void) {return wanted;}
static unsigned select_players(void) {return psx_mod_netplay_profile()==&four?players:2;}
int main(void) {
    char a[65],b[65];
    assert(psx_mod_register_netplay_profile(&four));
    assert(psx_mod_register_netplay_profile(&base));
    assert(psx_mod_netplay_profile()==&base);
    assert(psx_mod_netplay_profile_count()==2 && psx_mod_netplay_profile_at(2)==0);
    wanted=&four;psx_mod_netplay_set_profile_selector(select_profile);
    psx_mod_netplay_set_player_count_selector(select_players);
    assert(psx_mod_netplay_profile()==&four);
    assert(!strcmp(psx_mod_netplay_version("native"),"four-v1"));
    assert(psx_mod_netplay_content_identity("",a));
    players=3;assert(psx_mod_netplay_content_identity("",b));assert(strcmp(a,b));
    psx_mod_netplay_set_active(1);wanted=&base;
    assert(psx_mod_netplay_profile()==&four); /* no mid-match profile switch */
    psx_mod_netplay_set_active(0);assert(psx_mod_netplay_profile()==&base);
    assert(psx_mod_netplay_player_count()==2);
    PSXModNetplayProfile invalid=base;invalid.plugin_id="invalid";invalid.min_players=5;invalid.max_players=4;
    assert(!psx_mod_register_netplay_profile(&invalid));
    invalid=base;invalid.plugin_id="invalid";invalid.optional_last_seat_option="p4";
    assert(!psx_mod_register_netplay_profile(&invalid));
    invalid=base;invalid.plugin_id="invalid";invalid.select_when_feature_enabled=1;
    assert(!psx_mod_register_netplay_profile(&invalid));
    return 0;
}

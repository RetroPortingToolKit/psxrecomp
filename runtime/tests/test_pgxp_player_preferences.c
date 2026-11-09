#include "pgxp_session.h"
#include <stdio.h>
#include <stdlib.h>

static void check(int value,const char* message) {
    if (!value) { fprintf(stderr,"FAIL: %s\n",message);exit(1); }
}
int main(void) {
    PSXPgxpSessionInputs in={0};
    in.env_geometry=in.env_texture=in.env_cpu_mode=in.env_culling=-1;
    in.mod_only=1;in.video_geometry=in.video_texture=1;
#if defined(PSX_PGXP_HAS_PLAYER_PREFERENCES)
    in.player_geometry_set=in.player_texture_set=1;
#endif
    PSXPgxpSessionArm arm=psx_pgxp_session_resolve(&in);
    check(arm.geometry && arm.texture,"explicit player visual preferences work independently of a disabled mod");
    check(!arm.cpu_mode && !arm.culling,"visual preferences do not arm CPU propagation or culling");
#if defined(PSX_PGXP_HAS_PLAYER_PREFERENCES)
    in.player_geometry_set=in.player_texture_set=0;
#endif
    in.mod_enabled=in.mod_corrections_set=1;in.mod_geometry=0;in.mod_texture=1;
    arm=psx_pgxp_session_resolve(&in);
    check(!arm.geometry && arm.texture,"existing independent mod corrections retain precedence when player preferences are absent");
#if defined(PSX_PGXP_HAS_PLAYER_PREFERENCES)
    in.player_geometry_set=1;in.video_geometry=0;
#endif
    in.mod_geometry=1;in.mod_cpu_mode=1;
    arm=psx_pgxp_session_resolve(&in);
    check(!arm.geometry && arm.texture && arm.cpu_mode,"explicit geometry off preserves independent texture/CPU requests");
#if defined(PSX_PGXP_HAS_PLAYER_PREFERENCES)
    in.player_geometry_set=in.player_texture_set=1;
#endif
    in.video_geometry=in.video_texture=1;in.env_geometry=0;in.env_texture=0;
    arm=psx_pgxp_session_resolve(&in);
    check(!arm.geometry && !arm.texture,"environment overrides remain authoritative");
    in.env_geometry=in.env_texture=-1;in.netplay=1;in.mod_culling=1;
    arm=psx_pgxp_session_resolve(&in);
    check(!arm.culling,"netplay still excludes precise culling");
    puts("PASS: player PGXP visual preferences, existing mod choices, environment and netplay policy");
}

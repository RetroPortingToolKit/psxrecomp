#include "vr_pose_math.h"
#include <math.h>
#include <stdio.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL line %d\n",__LINE__);return 1;}}while(0)
int main(void) {
    const double q[4]={0,0,0,1},p[3]={-.0335,0,0},origin[3]={0,0,0};
    const double f[4]={-atan(1),atan(1),atan(.5),-atan(.5)};
    PSXModRenderView v;
    CHECK(vr_pose_to_view(q,p,q,origin,f,1000,512,240,&v));
    CHECK(v.translation[0]==34 && v.translation[1]==0 && v.translation[2]==0);
    CHECK(v.rotation_q12[0]==4096 && v.rotation_q12[4]==4096 && v.rotation_q12[8]==4096);
    CHECK(v.fx_q16==256*65536 && v.fy_q16==240*65536);
    CHECK(v.cx_delta_q16==0 && v.cy_delta_q16==0);
    double move[3]={0,.1,-.2};
    CHECK(vr_pose_to_view(q,move,q,origin,f,1000,512,240,&v));
    CHECK(v.translation[1]==100 && v.translation[2]==-200);
    const double yaw[4]={0,sin(.2),0,cos(.2)};
    CHECK(vr_pose_to_view(yaw,origin,q,origin,f,1000,512,240,&v));
    CHECK(v.rotation_q12[2]>0 && v.rotation_q12[6]<0);
    CHECK(vr_pose_to_view(yaw,move,yaw,move,f,1000,512,240,&v));
    CHECK(v.rotation_q12[0]==4096 && v.translation[0]==0 && v.translation[2]==0);
    double asymmetric[4]={-atan(.8),atan(1.2),atan(.7),-atan(.3)};
    CHECK(vr_pose_to_view(q,origin,q,origin,asymmetric,1000,512,240,&v));
    CHECK(v.cx_delta_q16==-3355443 && v.cy_delta_q16==3145728);
    double bad[4]={0,0,0,0};CHECK(!vr_pose_to_view(bad,p,q,origin,f,1000,512,240,&v));
    CHECK(!vr_pose_to_view(q,p,q,origin,f,NAN,512,240,&v));
    double r[9],t[3];
    CHECK(vr_pose_to_transform(q,move,q,origin,1000,r,t));
    CHECK(r[0]==1 && r[4]==1 && r[8]==1 && t[1]==-100 && t[2]==200);
    CHECK(vr_pose_to_transform(yaw,move,q,origin,1000,r,t));
    CHECK(r[2]<0 && r[6]>0); /* XR +Y rotation aims to PSX left. */
    CHECK(vr_pose_to_view(yaw,move,q,origin,f,1000,512,240,&v));
    for(int row=0;row<3;row++)for(int col=0;col<3;col++) {
        double n=0;for(int k=0;k<3;k++)n+=v.rotation_q12[row*3+k]/4096.0*r[k*3+col];
        CHECK(fabs(n-(row==col))<.0005);
    }
    for(int row=0;row<3;row++) {
        double n=v.translation[row];
        for(int k=0;k<3;k++)n+=v.rotation_q12[row*3+k]/4096.0*t[k];
        CHECK(fabs(n)<.6); /* View's integer translation quantizes the inverse. */
    }
    CHECK(vr_pose_to_transform(yaw,move,yaw,move,1000,r,t));
    CHECK(fabs(r[0]-1)<1e-10 && fabs(r[8]-1)<1e-10 && t[0]==0 && t[2]==0);
    CHECK(!vr_pose_to_transform(bad,p,q,origin,1000,r,t));
    CHECK(!vr_pose_to_transform(q,p,q,origin,NAN,r,t));
    puts("PASS: metric poses, axis signs, inverse controller transform, recenter and FOV");return 0;
}

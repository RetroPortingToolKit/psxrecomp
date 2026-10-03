#include "vr_pose_math.h"
#include <math.h>
#include <string.h>
static int matrix(const double q[4], double m[9]) {
    double n = 0; for(int i=0;i<4;i++) { if(!isfinite(q[i])) return 0; n+=q[i]*q[i]; }
    if(n < .5 || n > 1.5) return 0;
    double x=q[0]/sqrt(n),y=q[1]/sqrt(n),z=q[2]/sqrt(n),w=q[3]/sqrt(n);
    m[0]=1-2*(y*y+z*z);m[1]=2*(x*y-z*w);m[2]=2*(x*z+y*w);
    m[3]=2*(x*y+z*w);m[4]=1-2*(x*x+z*z);m[5]=2*(y*z-x*w);
    m[6]=2*(x*z-y*w);m[7]=2*(y*z+x*w);m[8]=1-2*(x*x+y*y);
    return 1;
}
int vr_pose_to_view(const double q[4], const double p[3],
                    const double oq[4], const double op[3],
                    const double f[4], double units, int w, int h,
                    PSXModRenderView *out) {
    double e[9],o[9];const int sign[3]={1,-1,-1};
    if(!out || !matrix(q,e) || !matrix(oq,o) || !isfinite(units) ||
       units < 1 || units > 65536 || w<=0 || h<=0 || w>1024 || h>512) return 0;
    for(int i=0;i<3;i++) if(!isfinite(p[i]) || !isfinite(op[i])) return 0;
    for(int i=0;i<4;i++) if(!isfinite(f[i]) || fabs(f[i])>=1.56) return 0;
    double l=tan(f[0]),r=tan(f[1]),u=tan(f[2]),d=tan(f[3]);
    if(r-l<=.05 || u-d<=.05) return 0;
    PSXModRenderView v={0};v.struct_size=sizeof v;v.projection=1;
    for(int row=0;row<3;row++) {
        for(int col=0;col<3;col++) {
            double a=0;for(int k=0;k<3;k++)a+=e[k*3+row]*o[k*3+col];
            v.rotation_q12[row*3+col]=(int32_t)lround(a*sign[row]*sign[col]*4096);
        }
        double a=0;for(int k=0;k<3;k++)a+=e[k*3+row]*(op[k]-p[k]);
        a*=sign[row]*units;if(fabs(a)>65536) return 0;
        v.translation[row]=(int32_t)lround(a);
    }
    double fx=w/(r-l),fy=h/(u-d),cx=-l*fx-w*.5,cy=u*fy-h*.5;
    double pix[4]={fx,fy,cx,cy};
    for(int i=0;i<4;i++) if(!isfinite(pix[i]) || fabs(pix[i])>32767) return 0;
    v.fx_q16=(int32_t)lround(fx*65536);v.fy_q16=(int32_t)lround(fy*65536);
    v.cx_delta_q16=(int32_t)lround(cx*65536);v.cy_delta_q16=(int32_t)lround(cy*65536);
    *out=v;return 1;
}
int vr_pose_to_transform(const double q[4],const double p[3],
                         const double oq[4],const double op[3],double units,
                         double rotation[9],double translation[3]) {
    double e[9],o[9],r[9],t[3];const int sign[3]={1,-1,-1};
    if(!rotation || !translation || !matrix(q,e) || !matrix(oq,o) ||
       !isfinite(units) || units<1 || units>65536)return 0;
    for(int i=0;i<3;i++)if(!isfinite(p[i]) || !isfinite(op[i]))return 0;
    for(int row=0;row<3;row++) {
        for(int col=0;col<3;col++) {
            double a=0;for(int k=0;k<3;k++)a+=o[k*3+row]*e[k*3+col];
            r[row*3+col]=a*sign[row]*sign[col];
        }
        double a=0;for(int k=0;k<3;k++)a+=o[k*3+row]*(p[k]-op[k]);
        t[row]=a*sign[row]*units;if(!isfinite(t[row]) || fabs(t[row])>65536)return 0;
    }
    memcpy(rotation,r,sizeof r);memcpy(translation,t,sizeof t);return 1;
}

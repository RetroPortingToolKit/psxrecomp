#pragma once
#include <math.h>
#include "mod_plugins.h"
static inline void xr_surface_rotate(const float q[4],const float p[3],float out[3]) {
    float t[3]={2*(q[1]*p[2]-q[2]*p[1]),2*(q[2]*p[0]-q[0]*p[2]),2*(q[0]*p[1]-q[1]*p[0])};
    out[0]=p[0]+q[3]*t[0]+q[1]*t[2]-q[2]*t[1];
    out[1]=p[1]+q[3]*t[1]+q[2]*t[0]-q[0]*t[2];
    out[2]=p[2]+q[3]*t[2]+q[0]*t[1]-q[1]*t[0];
}
/* Six clip-space vertices with UV, before the renderer's storage-row flip.
 * Surface and eye use raw LOCAL poses; recenter is already embodied in both.
 * Hide a surface crossing the near plane rather than producing invalid UVs. */
static inline int xr_surface_vertices_head(const PSXModOpenXRSurface *v,const float eye[7],
                                      const float fov[4],const float head[3],float out[36]) {
    const int corners[6][2]={{0,0},{1,0},{1,1},{0,0},{1,1},{0,1}};
    float inverse[4]={-eye[3],-eye[4],-eye[5],eye[6]};
    float l=tanf(fov[0]),r=tanf(fov[1]),u=tanf(fov[2]),d=tanf(fov[3]);
    if(!(r>l && u>d))return 0;
    float right[3]={1,0,0},up[3]={0,1,0};
    if(v->flags&PSX_MOD_OPENXR_SURFACE_FACE_HEAD) {
        float n[3];float length=0;
        for(int j=0;j<3;j++){n[j]=head[j]-v->position_m[j];length+=n[j]*n[j];}
        if(!isfinite(length) || length<.0004f)return 0;
        length=sqrtf(length);for(int j=0;j<3;j++)n[j]/=length;
        float horizontal=sqrtf(n[0]*n[0]+n[2]*n[2]);
        if(horizontal>.0001f){right[0]=n[2]/horizontal;right[2]=-n[0]/horizontal;}
        up[0]=n[1]*right[2];up[1]=n[2]*right[0]-n[0]*right[2];up[2]=-n[1]*right[0];
    }
    for(int i=0;i<6;i++) {
        float local[3]={(corners[i][0]-.5f)*v->size_m[0],(corners[i][1]-.5f)*v->size_m[1],0};
        float world[3],camera[3];
        if(v->flags&PSX_MOD_OPENXR_SURFACE_FACE_HEAD)
            for(int j=0;j<3;j++)world[j]=local[0]*right[j]+local[1]*up[j];
        else xr_surface_rotate(v->orientation_xyzw,local,world);
        for(int j=0;j<3;j++)world[j]+=v->position_m[j]-eye[j];
        xr_surface_rotate(inverse,world,camera);
        if(!isfinite(camera[2]) || camera[2]>=-.02f)return 0;
        out[i*6]=2*camera[0]/(r-l)+(r+l)*camera[2]/(r-l);
        out[i*6+1]=2*camera[1]/(u-d)+(u+d)*camera[2]/(u-d);
        out[i*6+2]=0;out[i*6+3]=-camera[2];
        out[i*6+4]=corners[i][0];out[i*6+5]=corners[i][1];
    }
    return 1;
}
static inline int xr_surface_vertices(const PSXModOpenXRSurface *v,const float eye[7],const float fov[4],float out[36]) {
    return xr_surface_vertices_head(v,eye,fov,eye,out);
}

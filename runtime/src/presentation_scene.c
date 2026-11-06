#include "presentation_scene.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int psx_presentation_phase(uint64_t earlier,uint64_t newer,uint64_t ready,
    uint64_t now,double *alpha) {
    if(!alpha || newer<=earlier || ready>now || now<earlier)return 0;
    *alpha=now>=newer?1.0:(double)(now-earlier)/(double)(newer-earlier);
    return 1;
}

typedef struct Resource { size_t offset, size; } Resource;
typedef struct Object { PsxPresentationObject data; uint32_t first; } Object;
struct PsxPresentationScene {
    Object *objects;
    PsxPresentationVertex *vertices;
    Resource *resources;
    unsigned char *bytes;
    uint32_t *lookup;
    uint32_t object_cap, vertex_cap, resource_cap, lookup_cap;
    uint32_t object_count, vertex_count, resource_count;
    size_t byte_cap, byte_count;
    uint64_t epoch, tick;
    PsxPresentationCamera camera;
    int building, sealed, failed;
};
static int finite_array(const double *a, unsigned n) {
    for (unsigned i=0;i<n;++i) if (!isfinite(a[i])) return 0;
    return 1;
}
static int pose_copy(PsxPresentationPose *to, const PsxPresentationPose *from) {
    if (!finite_array(from->position,3) || !finite_array(from->rotation,4) ||
        !finite_array(from->scale,3)) return 0;
    double n=0;
    for (unsigned i=0;i<4;++i) n+=from->rotation[i]*from->rotation[i];
    if (!isfinite(n) || n<1e-24) return 0;
    *to=*from; n=sqrt(n);
    for (unsigned i=0;i<4;++i) to->rotation[i]/=n;
    for (unsigned i=0;i<3;++i) if (from->scale[i]==0) return 0;
    return 1;
}
static int camera_copy(PsxPresentationCamera *to,const PsxPresentationCamera *from) {
    if (!from || !pose_copy(&to->pose,&from->pose) ||
        !isfinite(from->focal_x) || !isfinite(from->focal_y) ||
        !isfinite(from->center_x) || !isfinite(from->center_y) ||
        !isfinite(from->near_z) || from->focal_x<=0 || from->focal_y<=0 ||
        from->near_z<=0) return 0;
    for(unsigned i=0;i<3;++i) if(from->pose.scale[i]!=1) return 0;
    PsxPresentationPose pose=to->pose; *to=*from; to->pose=pose; return 1;
}
PsxPresentationScene *psx_presentation_scene_create(uint32_t objects,
    uint32_t vertices,uint32_t resources,size_t bytes) {
    if (!objects || !vertices || !resources || !bytes || objects>(1u<<20) ||
        vertices>(1u<<24) || resources>(1u<<20)) return NULL;
    PsxPresentationScene *s=calloc(1,sizeof *s);
    if (!s) return NULL;
    s->object_cap=objects; s->vertex_cap=vertices; s->resource_cap=resources;
    s->byte_cap=bytes; s->lookup_cap=1;
    while(s->lookup_cap<objects*2) s->lookup_cap*=2;
    s->objects=calloc(objects,sizeof *s->objects);
    s->vertices=calloc(vertices,sizeof *s->vertices);
    s->resources=calloc(resources,sizeof *s->resources);
    s->bytes=malloc(bytes); s->lookup=calloc(s->lookup_cap,sizeof *s->lookup);
    if(!s->objects || !s->vertices || !s->resources || !s->bytes || !s->lookup) {
        psx_presentation_scene_destroy(s); return NULL;
    }
    return s;
}
void psx_presentation_scene_destroy(PsxPresentationScene *s) {
    if(!s)return;
    free(s->objects);free(s->vertices);free(s->resources);free(s->bytes);
    free(s->lookup);free(s);
}
int psx_presentation_scene_begin(PsxPresentationScene *s,uint64_t epoch,
    uint64_t tick,const PsxPresentationCamera *camera) {
    if(!s)return 0;
    s->building=1; s->sealed=0; s->failed=0;
    s->object_count=s->vertex_count=s->resource_count=0;s->byte_count=0;
    memset(s->lookup,0,s->lookup_cap*sizeof *s->lookup);
    s->epoch=epoch;s->tick=tick;
    if(!camera_copy(&s->camera,camera)) {s->failed=1;return 0;}
    return 1;
}
int psx_presentation_scene_resource(PsxPresentationScene *s,
    const void *bytes,size_t size,uint32_t *index) {
    if(!s || !s->building || s->sealed || s->failed)return 0;
    if(!bytes || !size || !index || s->resource_count==s->resource_cap ||
        size>s->byte_cap-s->byte_count) {s->failed=1;return 0;}
    *index=s->resource_count++;
    s->resources[*index]=(Resource){s->byte_count,size};
    memcpy(s->bytes+s->byte_count,bytes,size);s->byte_count+=size;return 1;
}
static uint32_t slot_for(const PsxPresentationScene *s,uint64_t id) {
    id^=id>>33;id*=UINT64_C(0xff51afd7ed558ccd);id^=id>>33;
    return (uint32_t)id&(s->lookup_cap-1);
}
static const Object *find(const PsxPresentationScene *s,uint64_t id) {
    uint32_t slot=slot_for(s,id);
    while(s->lookup[slot]) {
        const Object *o=&s->objects[s->lookup[slot]-1];
        if(o->data.identity==id)return o;
        slot=(slot+1)&(s->lookup_cap-1);
    }
    return NULL;
}
int psx_presentation_scene_append(PsxPresentationScene *s,
    const PsxPresentationObject *o) {
    PsxPresentationPose pose;
    if(!s || !s->building || s->sealed || s->failed)return 0;
    if(!o || !o->identity || !o->vertices || !o->vertex_count ||
        o->vertex_count%3 || o->vertex_count>s->vertex_cap-s->vertex_count ||
        s->object_count==s->object_cap || o->resource>=s->resource_count ||
        (o->flags&~(PSX_PRESENTATION_CUT|PSX_PRESENTATION_NATIVE|PSX_PRESENTATION_AFFINE_UV)) ||
        !pose_copy(&pose,&o->pose) || find(s,o->identity)) {
        s->failed=1;return 0;
    }
    for(uint32_t i=0;i<o->vertex_count;++i) {
        if(!finite_array(o->vertices[i].position,3) ||
            !isfinite(o->vertices[i].uv[0]) || !isfinite(o->vertices[i].uv[1])) {
            s->failed=1;return 0;
        }
    }
    Object *dst=&s->objects[s->object_count];
    dst->data=*o;dst->data.pose=pose;dst->first=s->vertex_count;
    memcpy(s->vertices+s->vertex_count,o->vertices,o->vertex_count*sizeof *o->vertices);
    dst->data.vertices=s->vertices+s->vertex_count;
    s->vertex_count+=o->vertex_count;
    uint32_t slot=slot_for(s,o->identity);
    while(s->lookup[slot])slot=(slot+1)&(s->lookup_cap-1);
    s->lookup[slot]=++s->object_count;return 1;
}
int psx_presentation_scene_seal(PsxPresentationScene *s) {
    if(!s || !s->building || s->failed)return 0;
    s->sealed=1;return 1;
}
const void *psx_presentation_scene_resource_data(const PsxPresentationScene *s,
    uint32_t index,size_t *size) {
    if(size)*size=0;
    if(!s || !s->sealed || index>=s->resource_count)return NULL;
    if(size)*size=s->resources[index].size;
    return s->bytes+s->resources[index].offset;
}
uint32_t psx_presentation_scene_vertices(const PsxPresentationScene *s) {
    return s && s->sealed ? s->vertex_count:0;
}
static void pose_mix(const PsxPresentationPose *a,const PsxPresentationPose *b,
    double t,PsxPresentationPose *out) {
    if(t==0){*out=*a;return;}if(t==1){*out=*b;return;}
    for(unsigned i=0;i<3;++i) {
        out->position[i]=a->position[i]*(1-t)+b->position[i]*t;
        out->scale[i]=a->scale[i]*(1-t)+b->scale[i]*t;
    }
    double dot=0,sign=1,wa,wb,n=0;
    for(unsigned i=0;i<4;++i)dot+=a->rotation[i]*b->rotation[i];
    if(dot<0){sign=-1;dot=-dot;}
    if(dot>1)dot=1;
    if(dot>.9995){wa=1-t;wb=t;}
    else {double angle=acos(dot),inv=1/sin(angle);
        wa=sin((1-t)*angle)*inv;wb=sin(t*angle)*inv;}
    for(unsigned i=0;i<4;++i){
        out->rotation[i]=wa*a->rotation[i]+wb*sign*b->rotation[i];
        n+=out->rotation[i]*out->rotation[i];
    }
    n=sqrt(n);for(unsigned i=0;i<4;++i)out->rotation[i]/=n;
}
static void rotate(const double q[4],const double v[3],double out[3],int inverse) {
    double sign=inverse?-1:1,x=q[1]*sign,y=q[2]*sign,z=q[3]*sign;
    double t[3]={2*(y*v[2]-z*v[1]),2*(z*v[0]-x*v[2]),2*(x*v[1]-y*v[0])};
    out[0]=v[0]+q[0]*t[0]+y*t[2]-z*t[1];
    out[1]=v[1]+q[0]*t[1]+z*t[0]-x*t[2];
    out[2]=v[2]+q[0]*t[2]+x*t[1]-y*t[0];
}
int psx_presentation_pose_from_matrix(PsxPresentationPose *out,
    const double m[9],const double translation[3],int rigid) {
    if(!out || !m || !translation || !finite_array(m,9) || !finite_array(translation,3))return 0;
    PsxPresentationPose p={0};double r[9];
    for(unsigned col=0;col<3;++col) {
        double length=hypot(hypot(m[col],m[3+col]),m[6+col]);
        /* Native Q12 camera construction may accumulate several integer
         * normalization steps. Bound length roundoff to 16 Q12 units; the
         * independent orthogonality/shear check below remains stricter. */
        if(!isfinite(length) || length<1e-12 || (rigid && fabs(length-1)>16.0/4096.0))return 0;
        p.scale[col]=rigid?1:length;
        for(unsigned row=0;row<3;++row)r[row*3+col]=m[row*3+col]/length;
    }
    for(unsigned a=0;a<3;++a)for(unsigned b=a+1;b<3;++b) {
        double dot=0;for(unsigned row=0;row<3;++row)dot+=r[row*3+a]*r[row*3+b];
        if(fabs(dot)>.002)return 0;
    }
    const double det=r[0]*(r[4]*r[8]-r[5]*r[7])-r[1]*(r[3]*r[8]-r[5]*r[6])+r[2]*(r[3]*r[7]-r[4]*r[6]);
    if(!isfinite(det) || fabs(det)<.99 || (rigid && det<0))return 0;
    if(det<0) {p.scale[0]=-p.scale[0];for(unsigned row=0;row<3;++row)r[row*3]=-r[row*3];}
    double *q=p.rotation,s;
    const double trace=r[0]+r[4]+r[8];
    if(trace>0) {
        s=2*sqrt(trace+1);q[0]=s/4;q[1]=(r[7]-r[5])/s;q[2]=(r[2]-r[6])/s;q[3]=(r[3]-r[1])/s;
    } else if(r[0]>r[4] && r[0]>r[8]) {
        s=2*sqrt(1+r[0]-r[4]-r[8]);q[0]=(r[7]-r[5])/s;q[1]=s/4;q[2]=(r[1]+r[3])/s;q[3]=(r[2]+r[6])/s;
    } else if(r[4]>r[8]) {
        s=2*sqrt(1+r[4]-r[0]-r[8]);q[0]=(r[2]-r[6])/s;q[1]=(r[1]+r[3])/s;q[2]=s/4;q[3]=(r[5]+r[7])/s;
    } else {
        s=2*sqrt(1+r[8]-r[0]-r[4]);q[0]=(r[3]-r[1])/s;q[1]=(r[2]+r[6])/s;q[2]=(r[5]+r[7])/s;q[3]=s/4;
    }
    for(unsigned i=0;i<3;++i)p.position[i]=translation[i];
    return pose_copy(out,&p);
}
int psx_presentation_pose_point(const PsxPresentationPose *source,
    const double point[3],int inverse,double result[3]) {
    PsxPresentationPose p;double v[3],rotated[3];
    if(!source || !point || !result || !pose_copy(&p,source) || !finite_array(point,3))return 0;
    if(inverse) {
        for(unsigned i=0;i<3;++i)v[i]=point[i]-p.position[i];
        rotate(p.rotation,v,rotated,1);
        for(unsigned i=0;i<3;++i)result[i]=rotated[i]/p.scale[i];
    } else {
        for(unsigned i=0;i<3;++i)v[i]=point[i]*p.scale[i];
        rotate(p.rotation,v,rotated,0);
        for(unsigned i=0;i<3;++i)result[i]=rotated[i]+p.position[i];
    }
    return finite_array(result,3);
}
int psx_presentation_scene_sample(const PsxPresentationScene *a,
    const PsxPresentationScene *b,double alpha,PsxPresentationCamera *camera,
    PsxPresentationSample *out,uint32_t capacity) {
    if(!b || !b->sealed || !camera || !out || capacity<b->vertex_count ||
        !isfinite(alpha) || alpha<0 || alpha>1)return 0;
    int pair=a && a->sealed && a->epoch==b->epoch && a->tick<b->tick;
    *camera=b->camera;
    if(pair)pose_mix(&a->camera.pose,&b->camera.pose,alpha,&camera->pose);
    /* Projection changes are discontinuities, not a guessed zoom animation. */
    if(pair && (a->camera.focal_x!=b->camera.focal_x ||
        a->camera.focal_y!=b->camera.focal_y || a->camera.center_x!=b->camera.center_x ||
        a->camera.center_y!=b->camera.center_y || a->camera.near_z!=b->camera.near_z)) {
        pair=0;*camera=b->camera;
    }
    uint32_t n=0;
    for(uint32_t i=0;i<b->object_count;++i) {
        const Object *o=&b->objects[i],*p=pair?find(a,o->data.identity):NULL;
        int match=p && p->data.lifetime==o->data.lifetime &&
            p->data.topology==o->data.topology &&
            p->data.vertex_count==o->data.vertex_count &&
            !((p->data.flags^o->data.flags)&PSX_PRESENTATION_AFFINE_UV) &&
            !(o->data.flags&(PSX_PRESENTATION_CUT|PSX_PRESENTATION_NATIVE)) &&
            !(p->data.flags&PSX_PRESENTATION_NATIVE);
        if(match) for(unsigned k=0;k<3;++k)
            if((p->data.pose.scale[k]<0)!=(o->data.pose.scale[k]<0))match=0;
        PsxPresentationPose pose=o->data.pose;
        if(match)pose_mix(&p->data.pose,&o->data.pose,alpha,&pose);
        for(uint32_t j=0;j<o->data.vertex_count;++j) {
            const PsxPresentationVertex *v=&o->data.vertices[j];
            double local[3],world[3],relative[3];
            for(unsigned k=0;k<3;++k) {
                local[k]=(match?p->data.vertices[j].position[k]*(1-alpha)+
                    v->position[k]*alpha:v->position[k])*pose.scale[k];
            }
            rotate(pose.rotation,local,world,0);
            for(unsigned k=0;k<3;++k)
                relative[k]=world[k]+pose.position[k]-camera->pose.position[k];
            PsxPresentationSample *s=&out[n++];
            rotate(camera->pose.rotation,relative,s->view,1);
            if(!finite_array(s->view,3))return 0;
            s->identity=o->data.identity;s->vertex_index=j;s->resource=o->data.resource;
            s->uv[0]=v->uv[0];s->uv[1]=v->uv[1];s->rgba=v->rgba;
            s->flags=o->data.flags;
            s->interpolated=match;
        }
    }
    return 1;
}

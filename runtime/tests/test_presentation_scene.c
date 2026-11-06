#include "presentation_scene.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static PsxPresentationPose identity(void) {
    PsxPresentationPose p={{0,0,0},{1,0,0,0},{1,1,1}};return p;
}
static PsxPresentationCamera camera(void) {
    PsxPresentationCamera c={0};c.pose=identity();
    c.focal_x=c.focal_y=256;c.center_x=192;c.center_y=120;c.near_z=1;return c;
}
static PsxPresentationObject object(uint64_t id,PsxPresentationVertex *v) {
    PsxPresentationObject o={0};o.identity=id;o.lifetime=1;o.topology=1;
    o.pose=identity();o.vertices=v;o.vertex_count=3;return o;
}
static PsxPresentationScene *scene(uint64_t epoch,uint64_t tick,
    PsxPresentationCamera *c) {
    PsxPresentationScene *s=psx_presentation_scene_create(4,12,2,32);assert(s);
    assert(psx_presentation_scene_begin(s,epoch,tick,c));
    unsigned char tex[]={1,2,3,4};uint32_t index=99;
    assert(psx_presentation_scene_resource(s,tex,sizeof tex,&index));assert(index==0);
    memset(tex,0,sizeof tex);return s;
}
static void close_to(double a,double b){assert(fabs(a-b)<1e-9);}
int main(void) {
    double phase=-1;
    /* Native 30 Hz endpoints with a 60 Hz midpoint: only generate when the
     * next scene is available early enough; never delay the native timeline. */
    assert(psx_presentation_phase(1000,34000,17000,17500,&phase));close_to(phase,.5);
    assert(!psx_presentation_phase(1000,34000,18000,17500,&phase));
    assert(psx_presentation_phase(1000,34000,40000,40000,&phase));close_to(phase,1);
    assert(!psx_presentation_phase(34000,1000,0,17500,&phase));
    assert(!psx_presentation_phase(1000,34000,0,500,&phase));
    assert(psx_presentation_phase(UINT64_MAX-100,UINT64_MAX,0,UINT64_MAX-50,&phase));
    close_to(phase,.5);
    PsxPresentationCamera c=camera(),sample_camera;
    PsxPresentationVertex vertices[3]={{{1,0,10},{0,0},0xffffffff},
        {{0,1,10},{1,0},0xffffffff},{{0,0,10},{0,1},0xffffffff}};
    PsxPresentationSample result[12];
    PsxPresentationScene *a=scene(1,10,&c),*b=scene(1,12,&c);
    PsxPresentationObject o=object(100,vertices),other=object(200,vertices);
    other.pose.position[0]=100;
    assert(psx_presentation_scene_append(a,&o));
    assert(psx_presentation_scene_append(a,&other));assert(psx_presentation_scene_seal(a));
    /* Reverse draw order. The animation, translation and camera each move. */
    other.pose.position[0]=120;assert(psx_presentation_scene_append(b,&other));
    o.pose.position[0]=10;vertices[0].position[1]=4;
    assert(psx_presentation_scene_append(b,&o));assert(psx_presentation_scene_seal(b));
    vertices[0].position[0]=999; /* must not mutate either sealed snapshot */
    size_t size=0;const unsigned char *tex=psx_presentation_scene_resource_data(b,0,&size);
    assert(tex && size==4 && tex[0]==1 && tex[3]==4);
    assert(psx_presentation_scene_vertices(b)==6);
    assert(psx_presentation_scene_sample(a,b,.5,&sample_camera,result,12));
    assert(result[0].identity==200 && result[3].identity==100);
    close_to(result[0].view[0],111);close_to(result[3].view[0],6);
    close_to(result[3].view[1],2);assert(result[3].interpolated);
    assert(psx_presentation_scene_sample(a,b,0,&sample_camera,result,12));
    close_to(result[3].view[0],1);close_to(result[3].view[1],0);
    assert(psx_presentation_scene_sample(a,b,1,&sample_camera,result,12));
    close_to(result[3].view[0],11);close_to(result[3].view[1],4);
    assert(!psx_presentation_scene_sample(a,b,NAN,&sample_camera,result,12));
    assert(!psx_presentation_scene_sample(a,b,-.1,&sample_camera,result,12));
    assert(!psx_presentation_scene_sample(a,b,1.1,&sample_camera,result,12));
    assert(!psx_presentation_scene_sample(a,b,.5,&sample_camera,result,5));
    /* Recycled object address with a new lifetime must not pair. */
    PsxPresentationScene *d=scene(1,14,&c);o.lifetime=2;
    assert(psx_presentation_scene_append(d,&o));assert(psx_presentation_scene_seal(d));
    assert(psx_presentation_scene_sample(b,d,.5,&sample_camera,result,12));
    assert(!result[0].interpolated);close_to(result[0].view[0],1009);
    /* Camera-to-world transforms are inverted for view coordinates. */
    PsxPresentationCamera moving=c;moving.pose.position[0]=20;
    PsxPresentationScene *e=scene(1,16,&moving);
    o.lifetime=1;o.pose.position[0]=0;vertices[0].position[0]=1;
    assert(psx_presentation_scene_append(e,&o));assert(psx_presentation_scene_seal(e));
    assert(psx_presentation_scene_sample(a,e,.5,&sample_camera,result,12));
    close_to(sample_camera.pose.position[0],10);close_to(result[0].view[0],-9);
    /* Epoch reset/camera cut must not mix either the camera or the object. */
    PsxPresentationScene *cut=scene(2,18,&moving);
    assert(psx_presentation_scene_append(cut,&o));assert(psx_presentation_scene_seal(cut));
    assert(psx_presentation_scene_sample(a,cut,.5,&sample_camera,result,12));
    close_to(result[0].view[0],-19);assert(!result[0].interpolated);
    /* Rotation halfway from 0 to 180 degrees must keep the radius. */
    PsxPresentationScene *r=scene(1,20,&c);
    o.pose.rotation[0]=0;o.pose.rotation[3]=1;
    assert(psx_presentation_scene_append(r,&o));assert(psx_presentation_scene_seal(r));
    assert(psx_presentation_scene_sample(a,r,.5,&sample_camera,result,12));
    /* Vertex animation also contributes two units along local Y. */
    close_to(result[0].view[0],-2);close_to(result[0].view[1],1);
    close_to(result[0].view[2],10);
    /* Antipodal quaternions represent the same rotation. */
    PsxPresentationScene *anti=scene(1,21,&c);
    o.pose.rotation[0]=-1;o.pose.rotation[3]=0;
    assert(psx_presentation_scene_append(anti,&o));assert(psx_presentation_scene_seal(anti));
    assert(psx_presentation_scene_sample(a,anti,.5,&sample_camera,result,12));
    close_to(result[0].view[0],1);
    /* Mirrored meshes are supported; changing handedness cuts instead of
     * collapsing the mesh through zero scale halfway through an interval. */
    PsxPresentationScene *mirror=scene(1,22,&c);
    o.pose=identity();o.pose.scale[0]=-1;
    assert(psx_presentation_scene_append(mirror,&o));assert(psx_presentation_scene_seal(mirror));
    assert(psx_presentation_scene_sample(a,mirror,.5,&sample_camera,result,12));
    assert(!result[0].interpolated);close_to(result[0].view[0],-1);
    assert(psx_presentation_scene_sample(mirror,mirror,1,&sample_camera,result,12));
    close_to(result[0].view[0],-1);
    /* Duplicate identity poisons the whole frame rather than mispairing. */
    PsxPresentationScene *bad=scene(1,22,&c);
    assert(psx_presentation_scene_append(bad,&o));
    assert(!psx_presentation_scene_append(bad,&o));assert(!psx_presentation_scene_seal(bad));
    assert(!psx_presentation_scene_sample(a,bad,.5,&sample_camera,result,12));
    /* Bounds, invalid transforms, invalid cameras and immutable sealing. */
    assert(!psx_presentation_scene_append(b,&o));
    assert(psx_presentation_scene_begin(bad,1,23,&c));
    o.pose.rotation[0]=0;
    assert(!psx_presentation_scene_append(bad,&o));assert(!psx_presentation_scene_seal(bad));
    assert(psx_presentation_scene_begin(bad,1,24,&c));
    unsigned char bytes[33]={0};uint32_t ix;
    assert(!psx_presentation_scene_resource(bad,bytes,sizeof bytes,&ix));
    assert(!psx_presentation_scene_seal(bad));
    moving.pose.position[0]=NAN;
    assert(!psx_presentation_scene_begin(bad,1,25,&moving));
    psx_presentation_scene_destroy(a);psx_presentation_scene_destroy(b);
    psx_presentation_scene_destroy(d);psx_presentation_scene_destroy(e);
    psx_presentation_scene_destroy(cut);psx_presentation_scene_destroy(r);
    psx_presentation_scene_destroy(anti);psx_presentation_scene_destroy(bad);
    psx_presentation_scene_destroy(mirror);
    puts("presentation_scene: geometry, identity, resources, cuts and bounds PASS");return 0;
}

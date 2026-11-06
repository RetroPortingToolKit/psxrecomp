#include "presentation_draw.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void near(double a, double b) { assert(fabs(a-b)<1e-6); }
static PsxPresentationCamera camera(void) {
    PsxPresentationCamera c={0};
    c.pose.rotation[0]=1;
    c.pose.scale[0]=c.pose.scale[1]=c.pose.scale[2]=1;
    c.focal_x=c.focal_y=c.near_z=1;
    return c;
}
static PsxPresentationScene *scene(uint64_t epoch, uint64_t tick,
    PsxPresentationVertex *vertices, uint32_t flags, double x) {
    PsxPresentationScene *s=psx_presentation_scene_create(2,6,1,4);
    PsxPresentationCamera c=camera();
    assert(s && psx_presentation_scene_begin(s,epoch,tick,&c));
    uint32_t resource; const unsigned char texture[]={255,0,0,255};
    assert(psx_presentation_scene_resource(s,texture,4,&resource));
    PsxPresentationObject o={0};o.identity=42;o.lifetime=7;o.topology=2;
    o.pose=c.pose;o.pose.position[0]=x;
    o.vertex_count=3;o.vertices=vertices;o.flags=flags;o.resource=resource;
    assert(psx_presentation_scene_append(s,&o));
    assert(psx_presentation_scene_seal(s));return s;
}
static const PsxPresentationTriangle *build(PsxPresentationDraw *draw,
    PsxPresentationScene *s,const PsxPresentationViewport *p,uint32_t want) {
    assert(psx_presentation_draw_build(draw,NULL,s,1,p));
    uint32_t count=99;
    const PsxPresentationTriangle *tri=psx_presentation_draw_triangles(draw,&count);
    assert(tri && count==want);return tri;
}
static void all_inside(const PsxPresentationTriangle *tri,uint32_t count,
    const PsxPresentationViewport *p) {
    for(uint32_t i=0;i<count;++i) for(unsigned j=0;j<3;++j) {
        const PsxPresentationDrawVertex *v=&tri[i].vertices[j];
        assert(v->x>=p->left-1e-9 && v->x<=p->right+1e-9);
        assert(v->y>=p->top-1e-9 && v->y<=p->bottom+1e-9);
        assert(v->reciprocal_z>0 && v->reciprocal_z<=1+1e-9);
    }
}
int main(void) {
    PsxPresentationViewport viewport={-10,-10,10,10};
    PsxPresentationDraw *d=psx_presentation_draw_create(6,12);
    assert(d && !psx_presentation_draw_create(0,1));
    uint32_t count=99;
    assert(!psx_presentation_draw_triangles(d,&count) && count==0);
    PsxPresentationVertex vertices[3]={
        {{0,0,.5},{0,0},0xFF000000},
        {{2,0,2},{1,0},0xFF0000FF},
        {{0,2,2},{0,1},0xFF00FF00}};
    PsxPresentationScene *perspective=scene(1,1,vertices,0,0);
    const PsxPresentationTriangle *t=build(d,perspective,&viewport,2);
    all_inside(t,2,&viewport);
    int found=0;
    for(unsigned i=0;i<2;++i) for(unsigned j=0;j<3;++j) {
        const PsxPresentationDrawVertex *v=&t[i].vertices[j];
        if(fabs(v->x-2.0/3)<1e-9 && fabs(v->y)<1e-9) {
            near(v->reciprocal_z,1);near(v->uv[0],1.0/3);
            near(v->color[0],2.0/3);found=1;
        }
        assert(t[i].identity==42 && t[i].resource==0);
    }
    assert(found); // clip in 3D before division, with correct texture coordinates
    PsxPresentationScene *affine=scene(1,2,vertices,PSX_PRESENTATION_AFFINE_UV,0);
    t=build(d,affine,&viewport,2);found=0;
    for(unsigned i=0;i<2;++i) for(unsigned j=0;j<3;++j)
        if(fabs(t[i].vertices[j].x-2.0/3)<1e-9 && fabs(t[i].vertices[j].y)<1e-9) {
            near(t[i].vertices[j].uv[0],2.0/3);found=1;
        }
    assert(found); // PS1 affine mapping must not become perspective mapping at clips
    const PsxPresentationViewport narrow={.2,.2,.6,.6};
    assert(psx_presentation_draw_build(d,NULL,affine,1,&narrow));
    t=psx_presentation_draw_triangles(d,&count);assert(t && count>=2);
    all_inside(t,count,&narrow);
    /* Independently known affine field over the original projected triangle. */
    for(unsigned i=0;i<count;++i) for(unsigned j=0;j<3;++j) {
        near(t[i].vertices[j].uv[0],t[i].vertices[j].x);
        near(t[i].vertices[j].uv[1],t[i].vertices[j].y);
        near(t[i].vertices[j].color[0],t[i].vertices[j].x);
        near(t[i].vertices[j].color[1],t[i].vertices[j].y);
    }
    /* Refuse the entire build if a clipped triangle exceeds output capacity. */
    PsxPresentationDraw *small=psx_presentation_draw_create(3,1);
    assert(!psx_presentation_draw_build(small,NULL,perspective,1,&viewport));
    assert(!psx_presentation_draw_triangles(small,&count) && count==0);
    for(unsigned i=0;i<3;++i)vertices[i].position[2]=2;
    PsxPresentationScene *a=scene(1,4,vertices,0,0),*b=scene(1,6,vertices,0,4);
    assert(psx_presentation_draw_build(d,a,b,0,&viewport));
    t=psx_presentation_draw_triangles(d,&count);assert(count==1);near(t[0].vertices[0].x,0);
    assert(psx_presentation_draw_build(d,a,b,.5,&viewport));
    t=psx_presentation_draw_triangles(d,&count);near(t[0].vertices[0].x,1);
    assert(psx_presentation_draw_build(d,a,b,1,&viewport));
    t=psx_presentation_draw_triangles(d,&count);near(t[0].vertices[0].x,2);
    PsxPresentationScene *cut=scene(2,8,vertices,0,4);
    assert(psx_presentation_draw_build(d,a,cut,.5,&viewport));
    t=psx_presentation_draw_triangles(d,&count);near(t[0].vertices[0].x,2);
    size_t bytes=0;
    const unsigned char *texture=psx_presentation_scene_resource_data(b,t[0].resource,&bytes);
    assert(texture && bytes==4 && texture[0]==255 && texture[3]==255);
    /* A material interpolation-mode change is a discontinuity. */
    PsxPresentationScene *mode=scene(1,8,vertices,PSX_PRESENTATION_AFFINE_UV,4);
    assert(psx_presentation_draw_build(d,a,mode,.5,&viewport));
    t=psx_presentation_draw_triangles(d,&count);near(t[0].vertices[0].x,2);
    /* Coplanar near vertices are retained once. Entirely hidden triangles are
     * a valid empty frame, rather than an invalid frame or a NaN projection. */
    for(unsigned i=0;i<3;++i)vertices[i].position[2]=1;
    PsxPresentationScene *plane=scene(1,10,vertices,0,0);
    build(d,plane,&viewport,1);
    /* Multiple objects keep authored draw order, even when identities descend. */
    PsxPresentationScene *ordered=psx_presentation_scene_create(2,6,1,4);
    PsxPresentationCamera c=camera();
    assert(ordered && psx_presentation_scene_begin(ordered,1,11,&c));
    uint32_t resource;const uint32_t bytes4=0x12345678;
    assert(psx_presentation_scene_resource(ordered,&bytes4,4,&resource));
    PsxPresentationObject object={0};object.pose=c.pose;object.resource=resource;
    object.vertices=vertices;object.vertex_count=3;object.lifetime=1;
    object.identity=99;assert(psx_presentation_scene_append(ordered,&object));
    object.identity=1;assert(psx_presentation_scene_append(ordered,&object));
    assert(psx_presentation_scene_seal(ordered));
    t=build(d,ordered,&viewport,2);assert(t[0].identity==99 && t[1].identity==1);
    for(unsigned i=0;i<3;++i)vertices[i].position[2]=0;
    PsxPresentationScene *hidden=scene(1,12,vertices,0,0);
    build(d,hidden,&viewport,0);
    PsxPresentationViewport bad=viewport;bad.right=NAN;
    assert(!psx_presentation_draw_build(d,NULL,a,1,&bad));
    assert(!psx_presentation_draw_triangles(d,&count) && count==0);
    assert(!psx_presentation_draw_build(d,NULL,a,NAN,&viewport));
    /* Even finite scene inputs may overflow projection: refuse instead of
     * handing a raster backend infinite coordinates or a partial draw stream. */
    vertices[0].position[0]=DBL_MAX;vertices[0].position[2]=1;
    PsxPresentationScene *huge=scene(1,14,vertices,0,DBL_MAX);
    assert(!psx_presentation_draw_build(d,NULL,huge,1,&viewport));
    psx_presentation_scene_destroy(perspective);psx_presentation_scene_destroy(affine);
    psx_presentation_scene_destroy(a);psx_presentation_scene_destroy(b);
    psx_presentation_scene_destroy(cut);psx_presentation_scene_destroy(mode);
    psx_presentation_scene_destroy(plane);psx_presentation_scene_destroy(hidden);
    psx_presentation_scene_destroy(ordered);
    psx_presentation_scene_destroy(huge);
    psx_presentation_draw_destroy(d);psx_presentation_draw_destroy(small);
    puts("presentation_draw: geometry phases, near/viewport clips, affine UVs, resources, bounds PASS");
    return 0;
}

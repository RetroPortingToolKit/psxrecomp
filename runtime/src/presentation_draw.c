#include "presentation_draw.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct ClipVertex {
    double x, y, z; /* homogeneous projection X/Y and W (view Z) */
    double uv[2], color[4];
} ClipVertex;
struct PsxPresentationDraw {
    PsxPresentationSample *samples;
    PsxPresentationTriangle *triangles;
    uint32_t source_cap, triangle_cap, count;
    int valid;
};

PsxPresentationDraw *psx_presentation_draw_create(uint32_t vertices,
    uint32_t triangles) {
    if (!vertices || vertices > (1u << 24) || !triangles ||
        triangles > (1u << 24)) return NULL;
    PsxPresentationDraw *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->samples = calloc(vertices, sizeof *d->samples);
    d->triangles = calloc(triangles, sizeof *d->triangles);
    if (!d->samples || !d->triangles) {
        psx_presentation_draw_destroy(d); return NULL;
    }
    d->source_cap = vertices; d->triangle_cap = triangles;
    return d;
}
void psx_presentation_draw_destroy(PsxPresentationDraw *d) {
    if (!d) return;
    free(d->samples); free(d->triangles); free(d);
}

static double distance_to(const ClipVertex *v, unsigned plane,
    const PsxPresentationViewport *p, double near_z) {
    switch (plane) {
    case 0: return v->z - near_z;
    case 1: return v->x - p->left * v->z;
    case 2: return p->right * v->z - v->x;
    case 3: return v->y - p->top * v->z;
    default: return p->bottom * v->z - v->y;
    }
}
static int intersection(const ClipVertex *a, const ClipVertex *b,
    double da, double db, int affine_uv, ClipVertex *out) {
    /* Opposite signs. Normalize first so da-db cannot overflow. */
    const double scale = fmax(fabs(da), fabs(db));
    if (!(scale > 0) || !isfinite(scale)) return 0;
    const double t = (da / scale) / (da / scale - db / scale);
    out->x = a->x * (1-t) + b->x * t;
    out->y = a->y * (1-t) + b->y * t;
    out->z = a->z * (1-t) + b->z * t;
    /* The near plane is clipped FIRST, so every later generated W is positive.
     * Affine attributes use the edge's screen-space fraction. Ordinary linear
     * UV interpolation here is only correct for perspective-correct textures. */
    if (!(out->z > 0) || !isfinite(out->x) || !isfinite(out->y) ||
        !isfinite(out->z)) return 0;
    const double screen_t = t * b->z / out->z;
    for (unsigned i=0; i<2; ++i)
        out->uv[i] = a->uv[i]*(1-(affine_uv ? screen_t : t)) +
                     b->uv[i]*(affine_uv ? screen_t : t);
    for (unsigned i=0; i<4; ++i)
        out->color[i] = a->color[i]*(1-screen_t) + b->color[i]*screen_t;
    return 1;
}
static int emit_vertex(const ClipVertex *v, PsxPresentationDrawVertex *out) {
    out->reciprocal_z = 1 / v->z;
    out->x = v->x / v->z; out->y = v->y / v->z;
    if (!isfinite(out->reciprocal_z) || !isfinite(out->x) ||
        !isfinite(out->y)) return 0;
    for (unsigned i=0; i<2; ++i) {
        out->uv[i] = (float)v->uv[i];
        if (!isfinite(out->uv[i])) return 0;
    }
    for (unsigned i=0; i<4; ++i) {
        out->color[i] = (float)v->color[i];
        if (!isfinite(out->color[i])) return 0;
    }
    return 1;
}
int psx_presentation_draw_build(PsxPresentationDraw *d,
    const PsxPresentationScene *old_scene, const PsxPresentationScene *new_scene,
    double alpha, const PsxPresentationViewport *p) {
    if (!d) return 0;
    d->valid = 0; d->count = 0;
    if (!p || !isfinite(p->left) || !isfinite(p->top) ||
        !isfinite(p->right) || !isfinite(p->bottom) ||
        p->left >= p->right || p->top >= p->bottom) return 0;
    PsxPresentationCamera camera;
    if (!psx_presentation_scene_sample(old_scene, new_scene, alpha, &camera,
        d->samples, d->source_cap)) return 0;
    const uint32_t n = psx_presentation_scene_vertices(new_scene);
    return psx_presentation_draw_samples(d,d->samples,n,&camera,p);
}
int psx_presentation_draw_samples(PsxPresentationDraw *d,
    const PsxPresentationSample *samples,uint32_t n,const PsxPresentationCamera *projection,
    const PsxPresentationViewport *p) {
    if(!d)return 0;
    d->valid=0;d->count=0;
    if(n>d->source_cap || n%3 || (n && !samples) || !projection || !p ||
        !isfinite(p->left) || !isfinite(p->top) || !isfinite(p->right) || !isfinite(p->bottom) ||
        p->left>=p->right || p->top>=p->bottom ||
        !isfinite(projection->focal_x) || !isfinite(projection->focal_y) ||
        !isfinite(projection->center_x) || !isfinite(projection->center_y) ||
        !isfinite(projection->near_z) || projection->near_z<=0 ||
        projection->focal_x<=0 || projection->focal_y<=0)return 0;
    const PsxPresentationCamera camera=*projection;
    if(n && samples!=d->samples)memcpy(d->samples,samples,(size_t)n*sizeof *samples);
    for (uint32_t first=0; first<n; first+=3) {
        ClipVertex poly[2][10]; unsigned count=3, side=0;
        const PsxPresentationSample *sample = d->samples + first;
        for (unsigned i=0; i<3; ++i) {
            if (!sample[i].identity || !isfinite(sample[i].view[0]) || !isfinite(sample[i].view[1]) ||
                !isfinite(sample[i].view[2]) || !isfinite(sample[i].uv[0]) || !isfinite(sample[i].uv[1]) ||
                (sample[i].flags&~(PSX_PRESENTATION_CUT|PSX_PRESENTATION_NATIVE|PSX_PRESENTATION_AFFINE_UV)) ||
                sample[i].identity != sample->identity ||
                sample[i].resource != sample->resource ||
                sample[i].flags != sample->flags) return 0;
            ClipVertex *v = &poly[0][i];
            v->z = sample[i].view[2];
            v->x = camera.focal_x*sample[i].view[0] + camera.center_x*v->z;
            v->y = camera.focal_y*sample[i].view[1] + camera.center_y*v->z;
            if (!isfinite(v->x) || !isfinite(v->y)) return 0;
            for (unsigned k=0; k<2; ++k) v->uv[k] = sample[i].uv[k];
            for (unsigned k=0; k<4; ++k)
                v->color[k] = ((sample[i].rgba >> (k*8)) & 255) / 255.0;
        }
        for (unsigned plane=0; plane<5 && count; ++plane) {
            unsigned next=0;
            for (unsigned i=0; i<count; ++i) {
                const ClipVertex *a=&poly[side][i], *b=&poly[side][(i+1)%count];
                const double da=distance_to(a,plane,p,camera.near_z);
                const double db=distance_to(b,plane,p,camera.near_z);
                if (!isfinite(da) || !isfinite(db)) return 0;
                if (da>=0) {
                    if (next==10) return 0;
                    poly[side^1][next++]=*a;
                }
                /* A vertex exactly on the plane is emitted only once. */
                if ((da>0 && db<0) || (da<0 && db>0)) {
                    if (next==10 || !intersection(a,b,da,db,
                        sample->flags & PSX_PRESENTATION_AFFINE_UV,
                        &poly[side^1][next])) return 0;
                    ++next;
                }
            }
            count=next; side^=1;
        }
        for (unsigned i=1; i+1<count; ++i) {
            if (d->count == d->triangle_cap) return 0;
            PsxPresentationTriangle *t=&d->triangles[d->count++];
            t->identity=sample->identity; t->resource=sample->resource;
            t->flags=sample->flags;
            if (!emit_vertex(&poly[side][0], &t->vertices[0]) ||
                !emit_vertex(&poly[side][i], &t->vertices[1]) ||
                !emit_vertex(&poly[side][i+1], &t->vertices[2])) return 0;
        }
    }
    d->valid = 1; return 1;
}
const PsxPresentationTriangle *psx_presentation_draw_triangles(
    const PsxPresentationDraw *d, uint32_t *count) {
    if (count) *count = d && d->valid ? d->count : 0;
    return d && d->valid ? d->triangles : NULL;
}

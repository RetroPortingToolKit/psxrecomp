#ifndef PSX_PRESENTATION_SCENE_H
#define PSX_PRESENTATION_SCENE_H

/* Host-only scene snapshots. No guest execution, memory pointers, graphics API,
 * wall-clock waits, image blending, correspondence inference or extrapolation.
 * The producer supplies verified identities and lifetime/topology generations.
 * One producer owns mutable scenes; sealed scenes may be read concurrently.
 * Destroy only after all readers have released their snapshot. */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct PsxPresentationScene PsxPresentationScene;
typedef struct PsxPresentationPose {
    double position[3];
    double rotation[4]; /* quaternion w,x,y,z, normalized on ingestion */
    double scale[3];
} PsxPresentationPose;
typedef struct PsxPresentationVertex {
    double position[3]; /* stable corresponding local-space animated vertex */
    float uv[2];
    uint32_t rgba; /* R in low byte, then G, B, A */
} PsxPresentationVertex;
typedef struct PsxPresentationObject {
    uint64_t identity, lifetime, topology;
    PsxPresentationPose pose;
    const PsxPresentationVertex *vertices; /* copied at append */
    uint32_t vertex_count; /* triangle list, multiple of three */
    uint32_t resource; /* index in this snapshot, not a guest address */
    uint32_t flags;
} PsxPresentationObject;
enum {
    PSX_PRESENTATION_CUT = 1, PSX_PRESENTATION_NATIVE = 2,
    PSX_PRESENTATION_AFFINE_UV = 4
};
typedef struct PsxPresentationCamera {
    PsxPresentationPose pose; /* camera-to-world; scale must be one */
    double focal_x, focal_y, center_x, center_y, near_z;
} PsxPresentationCamera;
typedef struct PsxPresentationSample {
    uint64_t identity;
    uint32_t vertex_index, resource;
    double view[3]; /* project only after near-plane clipping */
    float uv[2];
    uint32_t rgba;
    uint32_t flags;
    int interpolated;
} PsxPresentationSample;

/* Bounded allocation. Oversized/invalid appends poison the frame: seal fails,
 * so the caller shows the ordinary native frame rather than a partial scene. */
PsxPresentationScene *psx_presentation_scene_create(uint32_t objects,
    uint32_t vertices, uint32_t resources, size_t resource_bytes);
void psx_presentation_scene_destroy(PsxPresentationScene *scene);
int psx_presentation_scene_begin(PsxPresentationScene *scene, uint64_t epoch,
    uint64_t source_tick, const PsxPresentationCamera *camera);
int psx_presentation_scene_resource(PsxPresentationScene *scene,
    const void *bytes, size_t size, uint32_t *index);
int psx_presentation_scene_append(PsxPresentationScene *scene,
    const PsxPresentationObject *object);
int psx_presentation_scene_seal(PsxPresentationScene *scene);
const void *psx_presentation_scene_resource_data(const PsxPresentationScene *scene,
    uint32_t index, size_t *size);
uint32_t psx_presentation_scene_vertices(const PsxPresentationScene *scene);
/* old may be NULL. alpha is finite and in [0,1]; no extrapolation. Objects are
 * emitted in newer-frame draw order. Identity/lifetime/topology discontinuities
 * select the newer pose; they never pair by draw order or nearest position.
 * Epoch/camera cuts select the entire newer scene, including camera. */
int psx_presentation_scene_sample(const PsxPresentationScene *old_scene,
    const PsxPresentationScene *new_scene, double alpha,
    PsxPresentationCamera *camera, PsxPresentationSample *out, uint32_t capacity);

/* Select a phase on the ORIGINAL native display timeline. Both scenes must
 * already be complete; ready_ns includes GPU resource readiness. Never wait
 * for the newer scene or shift native display deadlines to manufacture a pair.
 * A zero result means present the native scene currently due. In particular,
 * missed readiness is a coverage failure, not permission to add source-frame
 * latency. Caller obtains display deadlines from a qualified title producer;
 * VBlank count alone does not establish a distinct scene update. */
int psx_presentation_phase(uint64_t earlier_display_ns,
    uint64_t newer_display_ns, uint64_t newer_ready_ns, uint64_t now_ns,
    double *alpha);

#ifdef __cplusplus
}
#endif
#endif

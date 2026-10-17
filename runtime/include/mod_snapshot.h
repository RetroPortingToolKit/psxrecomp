#ifndef PSX_MOD_SNAPSHOT_H
#define PSX_MOD_SNAPSHOT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Trusted, executable-owned host state for a mod package. Register once,
 * before gameplay, using the manifest package ID (not a feature/plugin ID).
 * Only packages in the committed simulation plan participate. A forced
 * executable-owned netplay profile uses its plugin ID. Local-view netplay
 * packages do not. The callbacks are copied; their code must remain
 * resident for the lifetime of the process.
 *
 * All calls run on the emulation thread. bytes/write/validate must not mutate
 * gameplay or call the guest. Encode explicit little-endian fields, never
 * host pointers, padding or file handles. bytes may vary between captures;
 * validate checks the saved length independently of the current live size.
 * write/validate return 1 on success. A successful validate promises that
 * read is infallible, does not allocate, and can consume exactly those bytes.
 * All providers validate before any guest or host state is restored. read
 * runs after the guest/device sections; rebuild derived resources in the
 * existing savestate plugin callback, before the restored PC resumes.
 *
 * ready is optional (NULL means ready). Return 0 while a host callback or
 * projected guest context has a live C stack that cannot be resumed from a
 * snapshot. User save/load and rewind requests wait for ready to return 1.
 * A provider must not disable readiness merely because the UI is paused.
 * Version is a nonzero wire schema number, not a build or resource counter.
 */
typedef struct PSXModSnapshotProvider {
    uint32_t version;
    uint32_t (*bytes)(void);
    int (*write)(uint8_t *out, uint32_t len);
    int (*validate)(const uint8_t *data, uint32_t len);
    void (*read)(const uint8_t *data, uint32_t len);
    int (*ready)(void);
} PSXModSnapshotProvider;

/* Returns 0 for invalid callbacks/ID, duplicate registration, or capacity.
 * IDs use manifest syntax: 1..96 lowercase ASCII letters, digits, '.', '-'
 * or '_', without a leading/trailing '.'. Registration cannot be replaced. */
int psx_mod_register_snapshot_provider(const char *package_id,
                                      const PSXModSnapshotProvider *provider);
/* A native-RAM-only mod can guard a live host stack without inventing host
 * state. New snapshots omit these guards, so enabling/disabling them does not
 * change the layout. An optional legacy schema/validator accepts and discards
 * the mod's former constant payload, even when it is currently disabled.
 * This is only for stateless mods: never register mutable state this way.
 * At most eight guards can declare legacy schemas (bounded cookie migration).
 * legacy_version and legacy_validate must either both be present or both zero.
 */
int psx_mod_register_snapshot_guard(const char *package_id, int (*ready)(void),
                                   uint32_t legacy_version,
                                   int (*legacy_validate)(const uint8_t *, uint32_t));
int psx_mod_snapshot_ready(void);

/* Runtime serializer helpers. No active provider => capture succeeds with
 * NULL/0 and cookie 0, preserving existing no-provider snapshot files.
 * capture owns measurement, allocation and encoding as one checked operation;
 * its malloc'd output belongs to the caller. On failure outputs are NULL/0.
 * The wire is bounded to 16 MiB and 64 providers, ordered by package ID.
 * read requires the same buffer/set to have passed validate at this boundary.
 */
int psx_mod_snapshot_capture(uint8_t **out_data, uint32_t *out_len);
uint32_t psx_mod_snapshot_layout_cookie(void);
/* Accept the current layout or a known legacy stateless-guard subset only.
 * Exact mutable-provider IDs, schemas and payloads are still prevalidated. */
int psx_mod_snapshot_layout_cookie_matches(uint32_t cookie);
int psx_mod_snapshot_validate(const uint8_t *data, uint32_t len);
void psx_mod_snapshot_read(const uint8_t *data, uint32_t len);

/* Diagnostic observers receive the exact complete loaded .pst image after a
 * successful restore (including rewind). The image is borrowed until return.
 * They may write diagnostic artifacts, but must not mutate simulation or call
 * the guest. One observer per package; the same callback registered for more
 * than one enabled package is invoked once. Failed loads never notify. */
typedef void (*PSXModSnapshotLoadedObserver)(const uint8_t *full_pst, uint32_t len);
int psx_mod_register_snapshot_loaded_observer(const char *package_id,
                                            PSXModSnapshotLoadedObserver observer);
void psx_mod_snapshot_notify_loaded(const uint8_t *full_pst, uint32_t len);

#ifdef __cplusplus
}
#endif
#endif

#include "mod_snapshot.h"
#include "mod_runtime.h"
#include "pst_wire.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define SNAPSHOT_MAX_PROVIDERS 64u
#define SNAPSHOT_MAX_ID 96u
#define SNAPSHOT_MAX_BYTES (16u * 1024u * 1024u)
#define SNAPSHOT_MAGIC 0x504e534du /* "MSNP" */
#define SNAPSHOT_VERSION 1u
#define SNAPSHOT_HEADER_BYTES 16u
#define SNAPSHOT_RECORD_BYTES 12u

typedef struct SnapshotRegistration {
    char id[SNAPSHOT_MAX_ID + 1u];
    PSXModSnapshotProvider provider;
} SnapshotRegistration;

typedef struct SnapshotRecord {
    const SnapshotRegistration *registration;
    const uint8_t *data;
    uint32_t len;
} SnapshotRecord;

static SnapshotRegistration s_registry[SNAPSHOT_MAX_PROVIDERS];
static uint32_t s_registry_count;

typedef struct SnapshotObserver {
    char id[SNAPSHOT_MAX_ID + 1u];
    PSXModSnapshotLoadedObserver callback;
} SnapshotObserver;
static SnapshotObserver s_observers[SNAPSHOT_MAX_PROVIDERS];
static uint32_t s_observer_count;

static int valid_id(const char *id, uint32_t *len) {
    uint32_t n = 0;
    if (!id) return 0;
    while (id[n]) {
        const unsigned char c = (unsigned char)id[n];
        if (n == SNAPSHOT_MAX_ID ||
            !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-' || c == '_')) return 0;
        ++n;
    }
    if (!n || id[0] == '.' || id[n - 1u] == '.') return 0;
    if (len) *len = n;
    return 1;
}

int psx_mod_register_snapshot_provider(const char *id,
                                      const PSXModSnapshotProvider *provider) {
    uint32_t len, at = 0;
    if (!valid_id(id, &len) || !provider || !provider->version ||
        !provider->bytes || !provider->write || !provider->validate ||
        !provider->read || s_registry_count == SNAPSHOT_MAX_PROVIDERS) return 0;
    /* Keep the registry canonical independently of static constructor order. */
    while (at < s_registry_count && strcmp(s_registry[at].id, id) < 0) ++at;
    if (at < s_registry_count && !strcmp(s_registry[at].id, id)) return 0;
    memmove(&s_registry[at + 1u], &s_registry[at],
            (s_registry_count - at) * sizeof(s_registry[0]));
    memcpy(s_registry[at].id, id, len + 1u);
    s_registry[at].provider = *provider;
    ++s_registry_count;
    return 1;
}

int psx_mod_register_snapshot_loaded_observer(const char *id,
                                            PSXModSnapshotLoadedObserver observer) {
    uint32_t len;
    if (!valid_id(id, &len) || !observer || s_observer_count == SNAPSHOT_MAX_PROVIDERS)
        return 0;
    for (uint32_t i = 0; i < s_observer_count; ++i)
        if (!strcmp(s_observers[i].id, id)) return 0;
    memcpy(s_observers[s_observer_count].id, id, len + 1u);
    s_observers[s_observer_count++].callback = observer;
    return 1;
}

void psx_mod_snapshot_notify_loaded(const uint8_t *full_pst, uint32_t len) {
    PSXModSnapshotLoadedObserver notified[SNAPSHOT_MAX_PROVIDERS];
    uint32_t count = 0;
    if (!full_pst || !len) return;
    for (uint32_t i = 0; i < s_observer_count; ++i) {
        uint32_t j;
        if (!mod_runtime_snapshot_package_active(s_observers[i].id)) continue;
        for (j = 0; j < count; ++j)
            if (notified[j] == s_observers[i].callback) break;
        if (j != count) continue;
        notified[count++] = s_observers[i].callback;
        s_observers[i].callback(full_pst, len);
    }
}

static uint32_t active_registrations(const SnapshotRegistration **active) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < s_registry_count; ++i) {
        if (mod_runtime_snapshot_package_active(s_registry[i].id)) {
            if (active) active[count] = &s_registry[i];
            ++count;
        }
    }
    return count;
}

int psx_mod_snapshot_ready(void) {
    const SnapshotRegistration *active[SNAPSHOT_MAX_PROVIDERS];
    const uint32_t count = active_registrations(active);
    for (uint32_t i = 0; i < count; ++i)
        if (active[i]->provider.ready && !active[i]->provider.ready()) return 0;
    return 1;
}

uint32_t psx_mod_snapshot_layout_cookie(void) {
    const SnapshotRegistration *active[SNAPSHOT_MAX_PROVIDERS];
    const uint32_t count = active_registrations(active);
    uint32_t hash = 2166136261u;
    if (!count) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        const char *id = active[i]->id;
        do { hash = (hash ^ (uint8_t)*id) * 16777619u; } while (*id++);
        for (uint32_t b = 0; b < 4u; ++b)
            hash = (hash ^ (uint8_t)(active[i]->provider.version >> (b * 8u))) * 16777619u;
    }
    /* Zero is reserved for no providers. Exact IDs/schemas are checked too. */
    return hash ? hash : 1u;
}

/* Framing and exact enabled-set/schema checks precede every payload validator.
 * No caller can restore provider A before discovering malformed provider B. */
static int parse_records(const uint8_t *data, uint32_t len,
                         SnapshotRecord *records, uint32_t *out_count) {
    const SnapshotRegistration *active[SNAPSHOT_MAX_PROVIDERS];
    const uint32_t active_count = active_registrations(active);
    uint32_t magic, version, count, reserved;
    PstR r;
    if (!data || len < SNAPSHOT_HEADER_BYTES || len > SNAPSHOT_MAX_BYTES) return 0;
    pst_r_init(&r, data, len);
    if (!pst_r_u32(&r, &magic) || !pst_r_u32(&r, &version) ||
        !pst_r_u32(&r, &count) || !pst_r_u32(&r, &reserved) ||
        magic != SNAPSHOT_MAGIC || version != SNAPSHOT_VERSION || reserved ||
        !count || count != active_count) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t schema, id_len, payload_len;
        const uint32_t expected_id_len = (uint32_t)strlen(active[i]->id);
        if (!pst_r_u32(&r, &schema) || !pst_r_u32(&r, &id_len) ||
            !pst_r_u32(&r, &payload_len) || schema != active[i]->provider.version ||
            id_len != expected_id_len || (size_t)(r.end - r.p) < id_len ||
            memcmp(r.p, active[i]->id, id_len)) return 0;
        r.p += id_len;
        if ((size_t)(r.end - r.p) < payload_len) return 0;
        records[i].registration = active[i];
        records[i].data = r.p;
        records[i].len = payload_len;
        r.p += payload_len;
    }
    if (r.p != r.end) return 0;
    *out_count = count;
    return 1;
}

int psx_mod_snapshot_validate(const uint8_t *data, uint32_t len) {
    SnapshotRecord records[SNAPSHOT_MAX_PROVIDERS];
    uint32_t count = 0;
    if (!parse_records(data, len, records, &count)) return 0;
    for (uint32_t i = 0; i < count; ++i)
        if (!records[i].registration->provider.validate(records[i].data, records[i].len))
            return 0;
    return 1;
}

void psx_mod_snapshot_read(const uint8_t *data, uint32_t len) {
    SnapshotRecord records[SNAPSHOT_MAX_PROVIDERS];
    uint32_t count = 0;
    const int parsed = parse_records(data, len, records, &count);
    assert(parsed); /* Caller prevalidated this unchanged buffer and plan. */
    if (!parsed) return;
    for (uint32_t i = 0; i < count; ++i)
        records[i].registration->provider.read(records[i].data, records[i].len);
}

int psx_mod_snapshot_capture(uint8_t **out_data, uint32_t *out_len) {
    const SnapshotRegistration *active[SNAPSHOT_MAX_PROVIDERS];
    const SnapshotRegistration *after[SNAPSHOT_MAX_PROVIDERS];
    uint32_t sizes[SNAPSHOT_MAX_PROVIDERS], count, total = SNAPSHOT_HEADER_BYTES;
    uint8_t *data;
    PstW w;
    if (!out_data || !out_len) return 0;
    *out_data = NULL;
    *out_len = 0;
    if (!psx_mod_snapshot_ready()) return 0;
    count = active_registrations(active);
    if (!count) return 1;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t overhead = SNAPSHOT_RECORD_BYTES + (uint32_t)strlen(active[i]->id);
        sizes[i] = active[i]->provider.bytes();
        if (overhead > SNAPSHOT_MAX_BYTES - total ||
            sizes[i] > SNAPSHOT_MAX_BYTES - total - overhead) return 0;
        total += overhead + sizes[i];
    }
    data = (uint8_t *)malloc(total);
    if (!data) return 0;
    pst_w_init(&w, data, total);
    if (!pst_w_u32(&w, SNAPSHOT_MAGIC) || !pst_w_u32(&w, SNAPSHOT_VERSION) ||
        !pst_w_u32(&w, count) || !pst_w_u32(&w, 0)) goto fail;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t id_len = (uint32_t)strlen(active[i]->id);
        if (active[i]->provider.bytes() != sizes[i] ||
            !pst_w_u32(&w, active[i]->provider.version) || !pst_w_u32(&w, id_len) ||
            !pst_w_u32(&w, sizes[i]) || !pst_w_bytes(&w, active[i]->id, id_len) ||
            !active[i]->provider.write(w.p, sizes[i]) ||
            !pst_w_bytes(&w, NULL, sizes[i])) goto fail;
    }
    if (w.written != total || active_registrations(after) != count ||
        !psx_mod_snapshot_ready()) goto fail;
    for (uint32_t i = 0; i < count; ++i)
        if (active[i] != after[i] || active[i]->provider.bytes() != sizes[i]) goto fail;
    if (!psx_mod_snapshot_validate(data, total)) goto fail;
    *out_data = data;
    *out_len = total;
    return 1;
fail:
    free(data);
    return 0;
}

/* Player replay file reader and writer. See replay_file.h. */
#include "replay_file.h"
#include "psx_sha256.h"
#include <stdlib.h>
#include <string.h>

#define RAM_BYTES (2u << 20)
#define PAGE_BYTES 4096u

uint32_t replay_file_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const unsigned char *p)
{
    return (uint64_t)replay_file_le32(p) | ((uint64_t)replay_file_le32(p + 4) << 32);
}

static void put32(unsigned char *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i));
}

int replay_file_name_ok(const char *s, size_t n)
{
    if (!s || !n || n > REPLAY_FILE_NAME_MAX) return 0;
    for (size_t i = 0; i < n; ++i)
        if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] == 0x7f) return 0;
    return 1;
}

/* Printable ASCII and '\n' only. */
static int lines_ok(const char *s, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (s[i] != '\n' && ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7e)) return 0;
    return 1;
}

void replay_file_checkpoint(uint32_t frame, const uint8_t *ram, uint64_t cycle, ReplayCheckpoint *out)
{
    out->frame = frame;
    out->cycle = cycle;
    psx_sha256_compute(ram, RAM_BYTES, out->ram_sha256);
    for (uint32_t p = 0; p < REPLAY_FILE_RAM_PAGES; ++p) {
        uint64_t hash = UINT64_C(14695981039346656037);
        const uint8_t *bytes = ram + p * PAGE_BYTES;
        for (uint32_t i = 0; i < PAGE_BYTES; ++i)
            hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
        out->pages[p] = hash;
    }
}

/* One identity text: printable ASCII, not empty, read once. */
static const char *read_text(FILE *f, uint32_t length, char *out)
{
    if (out[0]) return "duplicate identity tag";
    if (!length || length >= REPLAY_FILE_TEXT) return "identity text length";
    if (fread(out, 1, length, f) != length) return "short identity text";
    for (uint32_t i = 0; i < length; ++i)
        if ((unsigned char)out[i] < 0x20 || (unsigned char)out[i] > 0x7e) { out[0] = 0; return "identity text byte"; }
    out[length] = 0;
    return NULL;
}

/* Reads `length` bytes of text that may hold '\n' into out[cap]. */
static const char *read_lines(FILE *f, uint32_t length, char *out, uint32_t cap, const char *what)
{
    if (out[0]) return what;
    if (!length || length > cap || fread(out, 1, length, f) != length) { out[0] = 0; return what; }
    out[length] = 0;
    if (!lines_ok(out, length)) { out[0] = 0; return what; }
    return NULL;
}

static const char *skip(FILE *f, uint32_t length, const char *what)
{
    return fseek(f, (long)length, SEEK_CUR) ? what : NULL;
}

const char *replay_file_read(FILE *f, ReplayFileInfo *info, ReplayStep *steps, uint32_t step_cap)
{
    unsigned char h[REPLAY_FILE_HEADER_BYTES], e[8], small[36];
    unsigned char *cp = NULL;
    ReplayFileInfo *s;
    long start, size;
    uint32_t used = 0, identity = 0, ext, record_size, markers = 0, last_marker = 0, last_kind = 0;
    uint32_t checkpoints = 0, last_checkpoint = 0;
    int have_settings = 0, have_digests = 0, have_cards = 0;
    const char *error = NULL;
    if (!f || !info || !steps || (start = ftell(f)) < 0) return "unseekable replay";
    if (!(s = (ReplayFileInfo *)calloc(1, sizeof *s))) return "out of memory";
    if (fread(h, 1, sizeof h, f) != sizeof h) { free(s); return "short header"; }
    if (memcmp(h, "PSXRTI3\0", 8) || replay_file_le32(h + 8) != 3) { free(s); return "not a replay file"; }
    record_size = replay_file_le32(h + 12);
    s->frames = replay_file_le32(h + 16);
    ext = replay_file_le32(h + 24);
    if (record_size != REPLAY_FILE_RECORD_BYTES) error = "record size";
    else if (!s->frames || s->frames > REPLAY_FILE_MAX_FRAMES) error = "frame count";
    else if (replay_file_le32(h + 20)) error = "flags";
    else if (ext % 4 || ext > REPLAY_FILE_MAX_EXT) error = "extension size";
    else if (fseek(f, 0, SEEK_END) || (size = ftell(f)) < 0 || fseek(f, start + (long)sizeof h, SEEK_SET))
        error = "unseekable replay";
    else if ((uint64_t)(size - start) != (uint64_t)sizeof h + ext + (uint64_t)s->frames * record_size)
        error = "file size";
    while (!error && used < ext) {
        uint32_t tag, length, padded;
        if (ext - used < sizeof e || fread(e, 1, sizeof e, f) != sizeof e) { error = "short extension entry"; break; }
        used += sizeof e;
        tag = replay_file_le32(e);
        length = replay_file_le32(e + 4);
        padded = (uint32_t)(((uint64_t)length + 3u) & ~(uint64_t)3u);
        if (length > ext - used || padded > ext - used) { error = "extension length"; break; }
        switch (tag) {
        case REPLAY_TAG_BUILD:
            error = read_text(f, length, s->id.build);
            if (!error && length != 40) error = "build length";
            for (uint32_t i = 0; !error && i < 40; ++i)
                if (!((s->id.build[i] >= '0' && s->id.build[i] <= '9') ||
                      (s->id.build[i] >= 'a' && s->id.build[i] <= 'f'))) error = "build hex";
            identity |= 1u;
            break;
        case REPLAY_TAG_DISC_SERIAL:
            error = read_text(f, length, s->id.disc_serial); identity |= 2u; break;
        case REPLAY_TAG_BIOS_STEM:
            error = read_text(f, length, s->id.bios_stem); identity |= 4u; break;
        case REPLAY_TAG_BOOT_MODE:
            error = read_text(f, length, s->id.boot_mode); identity |= 8u;
            if (!error && strcmp(s->id.boot_mode, "hle") && strcmp(s->id.boot_mode, "lle") &&
                strcmp(s->id.boot_mode, "hle-calls") && strcmp(s->id.boot_mode, "hle-boot"))
                error = "boot mode";
            break;
        case REPLAY_TAG_DISC_DIGEST:
            if (s->id.disc_digest_kind) { error = "duplicate identity tag"; break; }
            if (length != 36 || fread(small, 1, 36, f) != 36) { error = "disc digest length"; break; }
            s->id.disc_digest_kind = small[0];
            if ((small[0] != REPLAY_DISC_DIGEST_CUE && small[0] != REPLAY_DISC_DIGEST_FILE) ||
                small[1] || small[2] || small[3]) error = "disc digest kind";
            memcpy(s->id.disc_digest, small + 4, 32);
            identity |= 16u;
            break;
        case REPLAY_TAG_MARKER: {
            uint32_t frame, kind;
            if (length != 8 || fread(small, 1, 8, f) != 8) { error = "marker length"; break; }
            frame = replay_file_le32(small);
            kind = small[4];
            if ((kind != REPLAY_MARKER_MENU && kind != REPLAY_MARKER_GAMEPLAY && kind != REPLAY_MARKER_END) ||
                small[5] || small[6] || small[7]) error = "marker kind";
            else if (frame > s->frames) error = "marker frame";
            else if (markers && frame <= last_marker) error = "marker order";
            else if (markers == REPLAY_FILE_MAX_MARKERS) error = "marker capacity";
            else { ++markers; last_marker = frame; last_kind = kind; }
            break;
        }
        case REPLAY_TAG_CHECKPOINT:
            if (length != REPLAY_FILE_CHECKPOINT_BYTES) { error = "checkpoint length"; break; }
            if (checkpoints == REPLAY_FILE_MAX_MARKERS) { error = "checkpoint capacity"; break; }
            if (!cp && !(cp = (unsigned char *)malloc(REPLAY_FILE_CHECKPOINT_BYTES))) { error = "out of memory"; break; }
            if (fread(cp, 1, length, f) != length) { error = "short checkpoint"; break; }
            if (replay_file_le32(cp + 4)) { error = "checkpoint reserved"; break; }
            s->end.frame = replay_file_le32(cp);
            if (s->end.frame > s->frames) { error = "checkpoint frame"; break; }
            if (checkpoints && s->end.frame <= last_checkpoint) { error = "checkpoint order"; break; }
            /* Every checkpoint sits on a marker read before it. */
            if (!markers || last_marker != s->end.frame) { error = "checkpoint without marker"; break; }
            s->end.cycle = le64(cp + 8);
            memcpy(s->end.ram_sha256, cp + 16, 32);
            for (uint32_t i = 0; i < REPLAY_FILE_RAM_PAGES; ++i) s->end.pages[i] = le64(cp + 48 + 8 * i);
            last_checkpoint = s->end.frame;
            ++checkpoints;
            break;
        case REPLAY_TAG_ANCHOR:
            if (s->has_anchor) { error = "duplicate replay anchor"; break; }
            if (!length) { error = "replay anchor length"; break; }
            s->has_anchor = 1;
            s->anchor_offset = ftell(f) - start;
            s->anchor_length = length;
            error = skip(f, length, "short replay anchor");
            break;
        case REPLAY_TAG_SETTINGS:
            if (have_settings) { error = "duplicate replay settings"; break; }
            have_settings = 1;
            error = read_lines(f, length, s->settings, REPLAY_FILE_SETTINGS_MAX, "replay settings");
            break;
        case REPLAY_TAG_DIGESTS:
            if (have_digests) { error = "duplicate replay digests"; break; }
            have_digests = 1;
            if (length < 4 || fread(small, 1, 4, f) != 4) { error = "replay digests length"; break; }
            s->digest_count = replay_file_le32(small);
            if (s->digest_count > s->frames + 1u ||
                length != 4u + s->digest_count * REPLAY_FILE_DIGEST_BYTES) { error = "replay digests length"; break; }
            s->digest_offset = ftell(f) - start;
            error = skip(f, length - 4u, "short replay digests");
            break;
        case REPLAY_TAG_POWER_ON:
            if (s->power_on) { error = "duplicate replay power-on"; break; }
            if (length != 4 || fread(small, 1, 4, f) != 4) { error = "replay power-on length"; break; }
            if (replay_file_le32(small)) { error = "replay power-on vblank"; break; }
            s->power_on = 1;
            break;
        case REPLAY_TAG_CARDS:
            if (have_cards) { error = "duplicate replay cards"; break; }
            have_cards = 1;
            if (length < 4 || fread(small, 1, 4, f) != 4) { error = "replay cards length"; break; }
            s->cards_mask = replay_file_le32(small);
            if (s->cards_mask > 3u || length != 4u + replay_file_card_count(s->cards_mask) * REPLAY_FILE_CARD_BYTES)
                { error = "replay cards length"; break; }
            s->cards_offset = ftell(f) - start;
            error = skip(f, length - 4u, "short replay cards");
            break;
        case REPLAY_TAG_THUMB:
            if (s->thumb_w) { error = "duplicate replay thumbnail"; break; }
            if (length < 4 || length > REPLAY_FILE_THUMB_MAX || fread(small, 1, 4, f) != 4)
                { error = "replay thumbnail length"; break; }
            s->thumb_w = (uint32_t)small[0] | ((uint32_t)small[1] << 8);
            s->thumb_h = (uint32_t)small[2] | ((uint32_t)small[3] << 8);
            if (!s->thumb_w || !s->thumb_h || length != 4u + 4u * s->thumb_w * s->thumb_h)
                { s->thumb_w = s->thumb_h = 0; error = "replay thumbnail length"; break; }
            s->thumb_offset = ftell(f) - start;
            error = skip(f, length - 4u, "short replay thumbnail");
            break;
        case REPLAY_TAG_NAME:
            if (s->name[0]) { error = "duplicate replay name"; break; }
            if (!length || length > REPLAY_FILE_NAME_MAX || fread(s->name, 1, length, f) != length)
                { s->name[0] = 0; error = "replay name length"; break; }
            s->name[length] = 0;
            if (!replay_file_name_ok(s->name, length)) { s->name[0] = 0; error = "replay name byte"; }
            break;
        case REPLAY_TAG_PRODUCT:
            error = read_lines(f, length, s->product, REPLAY_FILE_PRODUCT_MAX, "replay product");
            break;
        default:
            if (!(tag & REPLAY_TAG_SKIPPABLE)) { error = "unsupported mandatory extension tag"; break; }
            if ((tag & 0x7fffff00u) == 0x100u) { error = "unsupported identity or marker tag"; break; }
            error = skip(f, length, "short extension payload");
            break;
        }
        for (uint32_t i = length; !error && i < padded; ++i)
            if (fgetc(f) != 0) error = "extension padding";
        used += padded;
    }
    free(cp);
    if (!error && identity != 31u) error = identity ? "partial identity" : "replay identity missing";
    if (!error && s->has_anchor == s->power_on)
        error = s->has_anchor ? "replay has an anchor and a power-on start" : "replay anchor missing";
    if (!error && s->power_on && !have_cards) error = "replay cards missing";
    if (!error && !s->power_on && have_cards) error = "replay cards without a power-on start";
    if (!error && (!markers || last_kind != REPLAY_MARKER_END || last_marker != s->frames))
        error = "replay END marker missing";
    if (!error && (!checkpoints || last_checkpoint != s->frames)) error = "replay END checkpoint missing";
    /* The records: runs of identical words. */
    for (uint32_t i = 0; !error && i < s->frames; ++i) {
        unsigned char r[REPLAY_FILE_RECORD_BYTES];
        ReplayStep *prev = s->steps ? &steps[s->steps - 1] : NULL;
        uint16_t buttons;
        if (fread(r, 1, sizeof r, f) != sizeof r) { error = "short record"; break; }
        if (replay_file_le32(r) != i + 1 || r[10] > 1 || r[11]) { error = "record sequence/button/reserved"; break; }
        buttons = (uint16_t)(r[4] | (r[5] << 8));
        if (prev && prev->buttons == buttons && !memcmp(prev->axes_ly_lx_ry_rx, r + 6, 4) &&
            prev->analog_button == r[10]) {
            ++prev->frames;
            continue;
        }
        if (s->steps == step_cap) { error = "step capacity"; break; }
        steps[s->steps].frames = 1;
        steps[s->steps].buttons = buttons;
        memcpy(steps[s->steps].axes_ly_lx_ry_rx, r + 6, 4);
        steps[s->steps++].analog_button = r[10];
    }
    if (!error && (fgetc(f) != EOF || ferror(f))) error = "trailing bytes or read error";
    if (!error) *info = *s;
    free(s);
    return error;
}

static int put_entry(FILE *f, uint32_t tag, const void *payload, uint32_t length)
{
    static const unsigned char zero[4] = {0, 0, 0, 0};
    const uint32_t pad = (4u - length % 4u) % 4u;
    unsigned char e[8];
    put32(e, tag);
    put32(e + 4, length);
    return fwrite(e, 1, 8, f) == 8 && (!length || fwrite(payload, 1, length, f) == length) &&
           fwrite(zero, 1, pad, f) == pad;
}

/* An entry whose payload is a u32 then `n` bytes. */
static int put_entry32(FILE *f, uint32_t tag, uint32_t first, const void *rest, uint32_t n)
{
    static const unsigned char zero[4] = {0, 0, 0, 0};
    const uint32_t pad = (4u - n % 4u) % 4u;
    unsigned char e[12];
    put32(e, tag);
    put32(e + 4, 4u + n);
    put32(e + 8, first);
    return fwrite(e, 1, 12, f) == 12 && (!n || fwrite(rest, 1, n, f) == n) && fwrite(zero, 1, pad, f) == pad;
}

static uint64_t entry_bytes(uint64_t length) { return 8u + ((length + 3u) & ~(uint64_t)3u); }

const char *replay_file_write(FILE *f, const ReplayFileOut *o)
{
    const ReplayIdentity *id = o ? o->id : NULL;
    const char *text[4];
    static const uint32_t text_tag[4] = {REPLAY_TAG_BUILD, REPLAY_TAG_DISC_SERIAL, REPLAY_TAG_BIOS_STEM,
                                         REPLAY_TAG_BOOT_MODE};
    const size_t settings_len = o && o->settings ? strlen(o->settings) : 0;
    const size_t name_len = o && o->name ? strlen(o->name) : 0;
    const size_t product_len = o && o->product ? strlen(o->product) : 0;
    const uint32_t card_bytes = o ? replay_file_card_count(o->cards_mask) * REPLAY_FILE_CARD_BYTES : 0;
    const uint32_t thumb_px = o && o->thumb ? (uint32_t)o->thumb_w * o->thumb_h : 0;
    unsigned char h[REPLAY_FILE_HEADER_BYTES], small[36], *cp;
    uint64_t ext = 0;
    if (!f || !o || !id || !o->words || !o->end) return "missing replay parts";
    text[0] = id->build; text[1] = id->disc_serial; text[2] = id->bios_stem; text[3] = id->boot_mode;
    if (!o->frames || o->frames > REPLAY_FILE_MAX_FRAMES) return "frame count";
    if (o->end->frame != o->frames) return "END checkpoint frame";
    if (!o->anchor == !o->power_on) return "replay needs exactly one of an anchor and a power-on start";
    if (o->anchor && !o->anchor_length) return "replay anchor length";
    if (o->power_on && (o->cards_mask > 3u || (card_bytes && !o->cards))) return "replay cards";
    if (settings_len > REPLAY_FILE_SETTINGS_MAX || !lines_ok(o->settings ? o->settings : "", settings_len))
        return "replay settings";
    if (product_len > REPLAY_FILE_PRODUCT_MAX || !lines_ok(o->product ? o->product : "", product_len))
        return "replay product";
    if (name_len && !replay_file_name_ok(o->name, name_len)) return "replay name";
    if (thumb_px && (4u + 4u * (uint64_t)thumb_px > REPLAY_FILE_THUMB_MAX)) return "replay thumbnail size";
    if (o->digest_count > o->frames + 1u) return "replay digests";
    for (uint32_t i = 0; i < o->digest_count; ++i)
        if (o->digests[i][0] > o->frames || (i && o->digests[i][0] <= o->digests[i - 1][0]))
            return "replay digest order";
    for (unsigned i = 0; i < 4; ++i) {
        const size_t n = strlen(text[i]);
        if (!n || n >= REPLAY_FILE_TEXT) return "identity text";
        ext += entry_bytes(n);
    }
    if (strlen(id->build) != 40) return "build length";
    ext += entry_bytes(36);
    if (o->anchor) ext += entry_bytes(o->anchor_length);
    if (o->power_on) ext += entry_bytes(4) + entry_bytes(4u + (uint64_t)card_bytes);
    if (product_len) ext += entry_bytes(product_len);
    if (settings_len) ext += entry_bytes(settings_len);
    if (o->digest_count) ext += entry_bytes(4u + (uint64_t)o->digest_count * REPLAY_FILE_DIGEST_BYTES);
    if (thumb_px) ext += entry_bytes(4u + 4u * (uint64_t)thumb_px);
    if (name_len) ext += entry_bytes(name_len);
    ext += entry_bytes(8) + entry_bytes(REPLAY_FILE_CHECKPOINT_BYTES);
    if (ext > REPLAY_FILE_MAX_EXT) return "extension size";

    memcpy(h, "PSXRTI3\0", 8);
    put32(h + 8, 3);
    put32(h + 12, REPLAY_FILE_RECORD_BYTES);
    put32(h + 16, o->frames);
    put32(h + 20, 0);
    put32(h + 24, (uint32_t)ext);
    if (fwrite(h, 1, sizeof h, f) != sizeof h) return "write header";
    for (unsigned i = 0; i < 4; ++i)
        if (!put_entry(f, text_tag[i], text[i], (uint32_t)strlen(text[i]))) return "write identity";
    memset(small, 0, sizeof small);
    small[0] = (unsigned char)id->disc_digest_kind;
    memcpy(small + 4, id->disc_digest, 32);
    if (!put_entry(f, REPLAY_TAG_DISC_DIGEST, small, 36)) return "write identity";
    if (o->anchor && !put_entry(f, REPLAY_TAG_ANCHOR, o->anchor, o->anchor_length)) return "write replay anchor";
    if (o->power_on) {
        if (!put_entry32(f, REPLAY_TAG_POWER_ON, 0, NULL, 0)) return "write replay power-on";
        if (!put_entry32(f, REPLAY_TAG_CARDS, o->cards_mask, o->cards, card_bytes)) return "write replay cards";
    }
    if (product_len && !put_entry(f, REPLAY_TAG_PRODUCT, o->product, (uint32_t)product_len))
        return "write replay product";
    if (settings_len && !put_entry(f, REPLAY_TAG_SETTINGS, o->settings, (uint32_t)settings_len))
        return "write replay settings";
    if (o->digest_count) {
        unsigned char e[12];
        put32(e, REPLAY_TAG_DIGESTS);
        put32(e + 4, 4u + o->digest_count * REPLAY_FILE_DIGEST_BYTES);
        put32(e + 8, o->digest_count);
        if (fwrite(e, 1, 12, f) != 12) return "write replay digests";
        for (uint32_t i = 0; i < o->digest_count; ++i) {
            unsigned char d[REPLAY_FILE_DIGEST_BYTES];
            for (unsigned k = 0; k < 5; ++k) put32(d + 4 * k, o->digests[i][k]);
            if (fwrite(d, 1, sizeof d, f) != sizeof d) return "write replay digests";
        }
    }
    if (thumb_px) {
        unsigned char e[12];
        put32(e, REPLAY_TAG_THUMB);
        put32(e + 4, 4u + 4u * thumb_px);
        e[8] = (unsigned char)o->thumb_w; e[9] = (unsigned char)(o->thumb_w >> 8);
        e[10] = (unsigned char)o->thumb_h; e[11] = (unsigned char)(o->thumb_h >> 8);
        if (fwrite(e, 1, 12, f) != 12) return "write replay thumbnail";
        for (uint32_t i = 0; i < thumb_px; ++i) {
            unsigned char px[4];
            put32(px, o->thumb[i]);
            if (fwrite(px, 1, 4, f) != 4) return "write replay thumbnail";
        }
    }
    if (name_len && !put_entry(f, REPLAY_TAG_NAME, o->name, (uint32_t)name_len)) return "write replay name";
    memset(small, 0, 8);
    put32(small, o->frames);
    small[4] = REPLAY_MARKER_END;
    if (!put_entry(f, REPLAY_TAG_MARKER, small, 8)) return "write marker";
    if (!(cp = (unsigned char *)calloc(1, REPLAY_FILE_CHECKPOINT_BYTES))) return "out of memory";
    put32(cp, o->end->frame);
    put32(cp + 8, (uint32_t)o->end->cycle);
    put32(cp + 12, (uint32_t)(o->end->cycle >> 32));
    memcpy(cp + 16, o->end->ram_sha256, 32);
    for (uint32_t p = 0; p < REPLAY_FILE_RAM_PAGES; ++p) {
        put32(cp + 48 + 8 * p, (uint32_t)o->end->pages[p]);
        put32(cp + 52 + 8 * p, (uint32_t)(o->end->pages[p] >> 32));
    }
    const int ok = put_entry(f, REPLAY_TAG_CHECKPOINT, cp, REPLAY_FILE_CHECKPOINT_BYTES);
    free(cp);
    if (!ok) return "write checkpoint";
    for (uint32_t i = 0; i < o->frames; ++i) {
        unsigned char r[REPLAY_FILE_RECORD_BYTES];
        put32(r, i + 1);
        r[4] = (unsigned char)o->words[i].buttons;
        r[5] = (unsigned char)(o->words[i].buttons >> 8);
        memcpy(r + 6, o->words[i].axes_ly_lx_ry_rx, 4);
        r[10] = r[11] = 0;
        if (fwrite(r, 1, sizeof r, f) != sizeof r) return "write record";
    }
    return ferror(f) ? "write error" : NULL;
}

/* Resident disc resources for seamless-loading adapters (mod_resident.h).
 *
 * Pack file (little endian):
 *   "PSXRES01", u32 version, u32 files, u32 derived, u32 blobs,
 *   u8 key[32], u64 payload_bytes,
 *   files[]   {u32 lba, size, padded, stock, blob}
 *   derived[] {u32 file, tag, meta[4], blob}
 *   blobs[]   {u64 offset, u32 size, u32 zero, u8 sha256[32]}
 *   u8 table_sha256[32]  (everything above), payload.
 * Every blob is hashed again on load. Identical bytes share one blob. */
#include "mod_resident.h"
#include "mod_plugins.h"
#include "mod_runtime.h"
#include "psx_sha256.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

using Bytes = std::vector<uint8_t>;
using Digest = std::array<uint8_t, 32>;

constexpr char kMagic[8] = {'P', 'S', 'X', 'R', 'E', 'S', '0', '1'};
constexpr uint32_t kVersion = 1;
constexpr uint32_t kBlobAlign = 16;
constexpr uint32_t kFullCd = 360000u * 2048u;
constexpr uint64_t kMaxPackBytes = 1ull << 31;

struct FileRec { uint32_t lba, size, padded, stock, blob; };
struct DerivedRec { uint32_t file, tag, meta[4], blob; };
struct Blob { uint64_t offset; uint32_t size; Digest sha; };

}  // namespace

struct PSXResidentPack {
    Bytes payload;
    std::vector<FileRec> files;
    std::vector<DerivedRec> derived;
    std::vector<Blob> blobs;
    std::vector<uint32_t> by_lba;  /* file indices sorted by lba */
    uint32_t modified = 0;
};

struct PSXResidentSink {
    PSXResidentPack* pack;
    std::map<Digest, uint32_t> index;
    uint32_t file_count;
    std::string error;
};

namespace {

struct Record {
    std::string title, format, state, error, path;
    std::unique_ptr<PSXResidentPack> pack;
    double ms = 0;
};
std::vector<Record>& records() {
    static std::vector<Record> r;
    return r;
}

Digest sha(const uint8_t* p, size_t n) {
    Digest d;
    psx_sha256_compute(p, n, d.data());
    return d;
}
std::string hex(const uint8_t* d, size_t n = 32) {
    std::string out(n * 2, '0');
    static const char* digits = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[d[i] >> 4];
        out[2 * i + 1] = digits[d[i] & 15];
    }
    return out;
}

void put32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; i++) b.push_back(uint8_t(v >> (8 * i))); }
void put64(Bytes& b, uint64_t v) { for (int i = 0; i < 8; i++) b.push_back(uint8_t(v >> (8 * i))); }
uint32_t get32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint64_t get64(const uint8_t* p) { return uint64_t(get32(p)) | uint64_t(get32(p + 4)) << 32; }

bool valid_spec(const PSXResidentSpec* s) {
    if (!s || s->struct_size < sizeof(PSXResidentSpec) || !s->title || !*s->title ||
        !s->format || !*s->format || !s->files || !s->file_count ||
        s->policy > PSX_RESIDENT_REQUIRE_STOCK)
        return false;
    for (const char* p = s->format; *p; ++p)
        if (!(std::isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == '.')) return false;
    for (const char* p = s->title; *p; ++p)
        if (*p == '/' || *p == '\\' || *p == ':' || *p == '.') return false;
    for (uint32_t i = 0; i < s->file_count; i++) {
        const auto& f = s->files[i];
        if (!f.path || !*f.path) return false;
        if (f.stock_sha256 && std::strlen(f.stock_sha256) != 64) return false;
    }
    return true;
}

Digest cache_key(const PSXResidentSpec* s, const std::string& fingerprint) {
    psx_sha256_ctx ctx;
    psx_sha256_init(&ctx);
    auto text = [&](const std::string& t) {
        psx_sha256_update(&ctx, reinterpret_cast<const uint8_t*>(t.c_str()), t.size() + 1);
    };
    text("PSXRES01");
    text(fingerprint);
    text(s->title);
    text(s->format);
    text(std::to_string(s->policy) + "/" + std::to_string(s->max_file_bytes));
    for (uint32_t i = 0; i < s->file_count; i++) {
        const auto& f = s->files[i];
        text(f.path);
        text(std::to_string(f.stock_size));
        text(f.stock_sha256 ? f.stock_sha256 : "");
    }
    Digest d;
    psx_sha256_final(&ctx, d.data());
    return d;
}

std::filesystem::path cache_dir(const char* title) {
    if (const char* p = std::getenv("PSX_RESIDENT_CACHE"))
        if (*p) return std::filesystem::path(p) / title;
    std::filesystem::path root;
#ifdef _WIN32
    if (const char* p = std::getenv("LOCALAPPDATA")) root = p;
#else
    if (const char* p = std::getenv("XDG_CACHE_HOME")) root = p;
    else if (const char* p = std::getenv("HOME")) root = std::filesystem::path(p) / ".cache";
#endif
    if (root.empty()) throw std::runtime_error("no cache directory");
    return root / title / "seamless";
}

uint32_t add_blob(PSXResidentSink& sink, const uint8_t* p, uint32_t n) {
    const Digest d = sha(p, n);
    auto it = sink.index.find(d);
    if (it != sink.index.end()) return it->second;
    auto& pack = *sink.pack;
    uint64_t offset = (pack.payload.size() + kBlobAlign - 1) & ~uint64_t(kBlobAlign - 1);
    if (offset + n > kMaxPackBytes) throw std::runtime_error("pack size limit");
    pack.payload.resize(size_t(offset));
    pack.payload.insert(pack.payload.end(), p, p + n);
    pack.blobs.push_back({offset, n, d});
    const uint32_t id = uint32_t(pack.blobs.size() - 1);
    sink.index.emplace(d, id);
    return id;
}

void index_lba(PSXResidentPack& pack) {
    pack.by_lba.resize(pack.files.size());
    for (uint32_t i = 0; i < pack.by_lba.size(); i++) pack.by_lba[i] = i;
    std::sort(pack.by_lba.begin(), pack.by_lba.end(),
              [&](uint32_t a, uint32_t b) { return pack.files[a].lba < pack.files[b].lba; });
    pack.modified = 0;
    for (const auto& f : pack.files) pack.modified += f.stock ? 0 : 1;
}

std::unique_ptr<PSXResidentPack> build(const PSXResidentSpec* s, std::string& error) {
    auto pack = std::make_unique<PSXResidentPack>();
    PSXResidentSink sink{pack.get(), {}, s->file_count, {}};
    const uint32_t max = s->max_file_bytes ? s->max_file_bytes : kFullCd;
    Bytes data;
    for (uint32_t i = 0; i < s->file_count; i++) {
        const auto& f = s->files[i];
        uint32_t lba = 0, size = 0;
        if (!PSXRecompV4::mod_runtime_read_disc_file_sectors(f.path, max, data, lba, size, &error))
            return nullptr;
        int stock = 0;
        if (f.stock_sha256)
            stock = (!f.stock_size || f.stock_size == size) &&
                    hex(sha(data.data(), data.size()).data()) == f.stock_sha256;
        if (!stock && s->policy == PSX_RESIDENT_REQUIRE_STOCK && f.stock_sha256) {
            error = std::string(f.path) + ": differs from the original disc";
            return nullptr;
        }
        const uint32_t blob = add_blob(sink, data.data(), uint32_t(data.size()));
        pack->files.push_back({lba, size, uint32_t(data.size()), uint32_t(stock), blob});
        if (s->derive) {
            const size_t before = pack->derived.size();
            if (!s->derive(&sink, i, data.data(), uint32_t(data.size()), stock, s->derive_user)) {
                error = sink.error.empty() ? std::string(f.path) + ": derive failed" : sink.error;
                return nullptr;
            }
            for (size_t d = before; d < pack->derived.size(); d++)
                if (pack->derived[d].file != i) {
                    error = std::string(f.path) + ": derived blob names another file";
                    return nullptr;
                }
        }
    }
    index_lba(*pack);
    return pack;
}

Bytes table(const PSXResidentPack& p, const Digest& key) {
    Bytes t(kMagic, kMagic + 8);
    put32(t, kVersion);
    put32(t, uint32_t(p.files.size()));
    put32(t, uint32_t(p.derived.size()));
    put32(t, uint32_t(p.blobs.size()));
    t.insert(t.end(), key.begin(), key.end());
    put64(t, p.payload.size());
    for (const auto& f : p.files) {
        put32(t, f.lba); put32(t, f.size); put32(t, f.padded); put32(t, f.stock); put32(t, f.blob);
    }
    for (const auto& d : p.derived) {
        put32(t, d.file); put32(t, d.tag);
        for (uint32_t m : d.meta) put32(t, m);
        put32(t, d.blob);
    }
    for (const auto& b : p.blobs) {
        put64(t, b.offset); put32(t, b.size); put32(t, 0);
        t.insert(t.end(), b.sha.begin(), b.sha.end());
    }
    const Digest d = sha(t.data(), t.size());
    t.insert(t.end(), d.begin(), d.end());
    return t;
}

void publish(const std::filesystem::path& path, const PSXResidentPack& p, const Digest& key) {
    std::filesystem::create_directories(path.parent_path());
    auto tmp = path;
#ifdef _WIN32
    tmp += "." + std::to_string(GetCurrentProcessId()) + ".tmp";
#else
    tmp += "." + std::to_string(getpid()) + ".tmp";
#endif
    const Bytes t = table(p, key);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(t.data()), std::streamsize(t.size()));
        out.write(reinterpret_cast<const char*>(p.payload.data()), std::streamsize(p.payload.size()));
        out.close();
        if (!out) {
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            throw std::runtime_error("cannot write " + tmp.string());
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        throw std::runtime_error("cannot publish " + path.string());
    }
#else
    std::filesystem::rename(tmp, path);
#endif
}

/* Structure, key and every blob hash. Returns null on any mismatch. */
std::unique_ptr<PSXResidentPack> load(const std::filesystem::path& path, const Digest& key,
                                      uint32_t file_count) {
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(path, ec);
    if (ec || bytes < 64 || bytes > kMaxPackBytes) return nullptr;
    std::ifstream in(path, std::ios::binary);
    uint8_t head[64];
    if (!in.read(reinterpret_cast<char*>(head), sizeof head)) return nullptr;
    if (std::memcmp(head, kMagic, 8) || get32(head + 8) != kVersion || get32(head + 12) != file_count ||
        std::memcmp(head + 24, key.data(), 32))
        return nullptr;
    const uint64_t nd = get32(head + 16), nb = get32(head + 20), payload = get64(head + 56);
    const uint64_t tsize = 64 + uint64_t(file_count) * 20 + nd * 28 + nb * 48;
    if (tsize + 32 > bytes || tsize + 32 + payload != bytes) return nullptr;
    Bytes raw(static_cast<size_t>(tsize + 32));
    std::memcpy(raw.data(), head, sizeof head);
    if (!in.read(reinterpret_cast<char*>(raw.data() + 64), std::streamsize(raw.size() - 64))) return nullptr;
    const uint8_t* p = raw.data();
    if (sha(p, size_t(tsize)) != *reinterpret_cast<const Digest*>(p + tsize)) return nullptr;
    auto pack = std::make_unique<PSXResidentPack>();
    const uint8_t* q = p + 64;
    for (uint32_t i = 0; i < file_count; i++, q += 20)
        pack->files.push_back({get32(q), get32(q + 4), get32(q + 8), get32(q + 12), get32(q + 16)});
    for (uint64_t i = 0; i < nd; i++, q += 28)
        pack->derived.push_back({get32(q), get32(q + 4), {get32(q + 8), get32(q + 12), get32(q + 16),
                                 get32(q + 20)}, get32(q + 24)});
    for (uint64_t i = 0; i < nb; i++, q += 48) {
        Blob b{get64(q), get32(q + 8), {}};
        std::memcpy(b.sha.data(), q + 16, 32);
        if (b.offset > payload || b.size > payload - b.offset) return nullptr;
        pack->blobs.push_back(b);
    }
    for (const auto& f : pack->files)
        if (f.blob >= nb || pack->blobs[f.blob].size != f.padded || f.padded % 2048 ||
            f.size > f.padded || f.padded - f.size >= 2048)
            return nullptr;
    for (const auto& d : pack->derived)
        if (d.blob >= nb || d.file >= file_count) return nullptr;
    pack->payload.resize(static_cast<size_t>(payload));
    if (!in.read(reinterpret_cast<char*>(pack->payload.data()), std::streamsize(payload))) return nullptr;
    for (const auto& b : pack->blobs)
        if (sha(pack->payload.data() + b.offset, b.size) != b.sha) return nullptr;
    index_lba(*pack);
    return pack;
}

/* A cache from another disc with the same plan fingerprint cannot match the
 * extents of every listed file on this one. */
bool extents_match(const PSXResidentPack& pack, const PSXResidentSpec* s) {
    for (uint32_t i = 0; i < s->file_count; i++) {
        uint32_t lba = 0, size = 0;
        if (!psx_mod_disc_file_extent(s->files[i].path, &lba, &size) ||
            lba != pack.files[i].lba || size != pack.files[i].size)
            return false;
    }
    return true;
}

void prune(const std::filesystem::path& keep, const std::string& format, uint32_t count) {
    std::error_code ec;
    std::vector<std::filesystem::directory_entry> packs;
    const std::string prefix = format + "-";
    for (const auto& e : std::filesystem::directory_iterator(keep.parent_path(), ec)) {
        const auto name = e.path().filename().string();
        if (e.path() != keep && name.rfind(prefix, 0) == 0 && e.path().extension() == ".pack")
            packs.push_back(e);
    }
    if (packs.size() <= count) return;
    std::sort(packs.begin(), packs.end(), [](const auto& a, const auto& b) {
        std::error_code e1, e2;
        return a.last_write_time(e1) > b.last_write_time(e2);
    });
    for (size_t i = count; i < packs.size(); i++) std::filesystem::remove(packs[i].path(), ec);
}

Record& record_for(const PSXResidentSpec* s) {
    auto& r = records();
    for (auto& rec : r)
        if (rec.title == s->title && rec.format == s->format) return rec;
    r.push_back({});
    r.back().title = s->title;
    r.back().format = s->format;
    return r.back();
}

void json_string(std::string& out, const std::string& s) {
    out += '"';
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out += c;
    }
    out += '"';
}

}  // namespace

extern "C" int psx_resident_emit(PSXResidentSink* sink, uint32_t file, uint32_t tag,
                                 const uint32_t meta[4], const void* bytes, uint32_t size) {
    if (!sink || file >= sink->file_count || (!bytes && size)) return 0;
    try {
        const uint32_t blob = add_blob(*sink, static_cast<const uint8_t*>(bytes), size);
        DerivedRec d{file, tag, {0, 0, 0, 0}, blob};
        if (meta) std::memcpy(d.meta, meta, sizeof d.meta);
        sink->pack->derived.push_back(d);
        return 1;
    } catch (const std::exception& e) {
        sink->error = e.what();
        return 0;
    }
}

extern "C" const PSXResidentPack* psx_resident_prepare(const PSXResidentSpec* spec) {
    if (!valid_spec(spec)) {
        psx_mod_counter_add("resident.invalid_spec", 1);
        return nullptr;
    }
    Record& rec = record_for(spec);
    rec.pack.reset();
    rec.error.clear();
    rec.path.clear();
    const auto start = std::chrono::steady_clock::now();
    try {
        const std::string& fingerprint = PSXRecompV4::mod_runtime_fingerprint();
        const Digest key = cache_key(spec, fingerprint);
        /* Without a committed plan there is no disc identity to key on: build
         * in memory and never reuse or publish. */
        bool cacheable = !fingerprint.empty() && !spec->memory_only;
        std::string memory_only;  /* why a planned cache is not used */
        std::filesystem::path path;
        std::unique_ptr<PSXResidentPack> pack;
        if (cacheable) {
            try {
                path = cache_dir(spec->title) / (std::string(spec->format) + "-" + hex(key.data(), 16) + ".pack");
                rec.path = path.string();
            } catch (const std::exception& e) {
                cacheable = false;
                memory_only = e.what();
            }
        }
        if (cacheable) {
            std::error_code ec;
            pack = load(path, key, spec->file_count);
            if (pack && !extents_match(*pack, spec)) pack.reset();
            if (!pack && std::filesystem::exists(path, ec)) psx_mod_counter_add("resident.cache_rejected", 1);
        }
        if (pack) {
            rec.state = "verified";
            std::error_code ec;
            std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), ec);
            prune(path, spec->format, spec->keep_packs ? spec->keep_packs : 3);
            psx_mod_counter_add("resident.verified", 1);
        } else {
            pack = build(spec, rec.error);
            if (!pack) throw std::runtime_error(rec.error);
            /* The cache only shortens the next launch. Whether a pack exists
             * depends on the effective disc and the plan alone, never on the
             * host file system, so netplay peers with one plan take one path. */
            if (cacheable) {
                try {
                    publish(path, *pack, key);
                    prune(path, spec->format, spec->keep_packs ? spec->keep_packs : 3);
                } catch (const std::exception& e) {
                    memory_only = e.what();
                }
            }
            if (!memory_only.empty()) {
                rec.state = "prepared (memory only: " + memory_only + ")";
                psx_mod_counter_add("resident.cache_write_failed", 1);
            } else {
                rec.state = cacheable ? "prepared"
                          : spec->memory_only ? "prepared (memory only)"
                                              : "prepared (uncached: no mod plan)";
            }
            psx_mod_counter_add("resident.prepared", 1);
        }
        rec.pack = std::move(pack);
    } catch (const std::exception& e) {
        rec.pack.reset();
        rec.state = "failed";
        if (rec.error.empty()) rec.error = e.what();
        psx_mod_counter_add("resident.prepare_failed", 1);
    }
    rec.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return rec.pack.get();
}

extern "C" uint32_t psx_resident_file_count(const PSXResidentPack* p) {
    return p ? uint32_t(p->files.size()) : 0;
}

extern "C" const uint8_t* psx_resident_file(const PSXResidentPack* p, uint32_t file,
                                            uint32_t* size, uint32_t* padded) {
    if (!p || file >= p->files.size()) return nullptr;
    const auto& f = p->files[file];
    if (size) *size = f.size;
    if (padded) *padded = f.padded;
    return p->payload.data() + p->blobs[f.blob].offset;
}

extern "C" uint32_t psx_resident_file_lba(const PSXResidentPack* p, uint32_t file) {
    return p && file < p->files.size() ? p->files[file].lba : 0;
}

extern "C" int psx_resident_file_stock(const PSXResidentPack* p, uint32_t file) {
    return p && file < p->files.size() ? int(p->files[file].stock) : 0;
}

extern "C" uint32_t psx_resident_modified_files(const PSXResidentPack* p) {
    return p ? p->modified : 0;
}

extern "C" const uint8_t* psx_resident_find_lba(const PSXResidentPack* p, uint32_t lba,
                                                uint32_t bytes, uint32_t* file) {
    if (!p || p->by_lba.empty()) return nullptr;
    /* Last file starting at or before lba. Files never overlap on a disc. */
    auto it = std::upper_bound(p->by_lba.begin(), p->by_lba.end(), lba,
                               [&](uint32_t v, uint32_t i) { return v < p->files[i].lba; });
    if (it == p->by_lba.begin()) return nullptr;
    const uint32_t i = *--it;
    const auto& f = p->files[i];
    const uint64_t offset = uint64_t(lba - f.lba) * 2048u;
    if (offset > f.padded || bytes > f.padded - offset) return nullptr;
    if (file) *file = i;
    return p->payload.data() + p->blobs[f.blob].offset + offset;
}

extern "C" uint32_t psx_resident_derived_count(const PSXResidentPack* p) {
    return p ? uint32_t(p->derived.size()) : 0;
}

extern "C" const uint8_t* psx_resident_derived(const PSXResidentPack* p, uint32_t index,
                                               uint32_t* file, uint32_t* tag,
                                               uint32_t meta[4], uint32_t* size) {
    if (!p || index >= p->derived.size()) return nullptr;
    const auto& d = p->derived[index];
    if (file) *file = d.file;
    if (tag) *tag = d.tag;
    if (meta) std::memcpy(meta, d.meta, sizeof d.meta);
    if (size) *size = p->blobs[d.blob].size;
    return p->payload.data() + p->blobs[d.blob].offset;
}

extern "C" int psx_resident_guest_ranges_match(const PSXResidentRange* ranges, uint32_t count,
                                               const char* sha256) {
    if (!ranges || !count || !sha256) return 0;
    psx_sha256_ctx ctx;
    psx_sha256_init(&ctx);
    uint8_t chunk[256];
    for (uint32_t r = 0; r < count; r++) {
        if (ranges[r].hi < ranges[r].lo) return 0;
        for (uint32_t a = ranges[r].lo; a < ranges[r].hi;) {
            const uint32_t n = std::min<uint32_t>(sizeof chunk, ranges[r].hi - a);
            for (uint32_t i = 0; i < n; i++) chunk[i] = psx_mod_read_byte(a + i);
            psx_sha256_update(&ctx, chunk, n);
            a += n;
        }
    }
    uint8_t d[32];
    psx_sha256_final(&ctx, d);
    return hex(d) == sha256;
}

extern "C" int psx_resident_status_json(char* out, uint32_t capacity) {
    std::string s = "[";
    for (const auto& r : records()) {
        if (s.size() > 1) s += ',';
        s += "{\"title\":"; json_string(s, r.title);
        s += ",\"format\":"; json_string(s, r.format);
        s += ",\"state\":"; json_string(s, r.state);
        s += ",\"error\":"; json_string(s, r.error);
        s += ",\"path\":"; json_string(s, r.path);
        char b[256];
        const PSXResidentPack* p = r.pack.get();
        std::snprintf(b, sizeof b,
                      ",\"files\":%u,\"modified\":%u,\"derived\":%u,\"blobs\":%u,\"bytes\":%llu,\"ms\":%.1f}",
                      p ? unsigned(p->files.size()) : 0u, p ? p->modified : 0u,
                      p ? unsigned(p->derived.size()) : 0u, p ? unsigned(p->blobs.size()) : 0u,
                      p ? (unsigned long long)p->payload.size() : 0ull, r.ms);
        s += b;
    }
    s += ']';
    if (!out || capacity <= s.size()) return 0;
    std::memcpy(out, s.c_str(), s.size() + 1);
    return 1;
}

/* Always-on service ring (mod_resident.h). Frames come from the debug
 * server's guest frame counter, the clock of the CD-ROM rings, so a load
 * window can be compared with cdrom_bursts / cdrom_sector_history. */
extern "C" uint64_t s_frame_count;
extern "C" uint64_t psx_get_cycle_count(void);

namespace {
struct Event {
    uint64_t seq, frame, cycle;
    const char* op;
    uint32_t file, lba, bytes, served;
};
constexpr uint32_t kEventCapacity = 8192;
Event g_events[kEventCapacity];
uint64_t g_event_total = 0;
}  // namespace

extern "C" void psx_resident_record(const char* op, uint32_t file, uint32_t lba,
                                    uint32_t bytes, int served) {
    Event& e = g_events[g_event_total % kEventCapacity];
    e.seq = g_event_total++;
    e.frame = s_frame_count;
    e.cycle = psx_get_cycle_count();
    e.op = op && *op ? op : "?";
    e.file = file;
    e.lba = lba;
    e.bytes = bytes;
    e.served = served ? 1u : 0u;
}

extern "C" int psx_resident_events_json(char* out, uint32_t capacity, uint64_t frame_lo,
                                        uint64_t frame_hi, uint32_t max) {
    const uint64_t held = std::min<uint64_t>(g_event_total, kEventCapacity);
    std::vector<const Event*> picked;
    for (uint64_t i = 0; i < held && picked.size() < max; i++) {
        const Event& e = g_events[(g_event_total - 1 - i) % kEventCapacity];
        if (e.frame >= frame_lo && e.frame <= frame_hi) picked.push_back(&e);
    }
    std::string s;
    char b[96];
    std::snprintf(b, sizeof b, "{\"total\":%llu,\"capacity\":%u,\"entries\":[",
                  (unsigned long long)g_event_total, kEventCapacity);
    s += b;
    for (size_t i = picked.size(); i-- > 0;) {
        const Event& e = *picked[i];
        s += "{\"seq\":" + std::to_string(e.seq) + ",\"frame\":" + std::to_string(e.frame) +
             ",\"cycle\":" + std::to_string(e.cycle) + ",\"op\":";
        json_string(s, e.op);
        std::snprintf(b, sizeof b, ",\"file\":%d,\"lba\":%u,\"bytes\":%u,\"served\":%u}%s",
                      e.file == UINT32_MAX ? -1 : int(e.file), e.lba, e.bytes, e.served,
                      i ? "," : "");
        s += b;
    }
    s += "]}";
    if (!out || capacity <= s.size()) return 0;
    std::memcpy(out, s.c_str(), s.size() + 1);
    return 1;
}

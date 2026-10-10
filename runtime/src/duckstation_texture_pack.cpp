#include "duckstation_texture_pack.h"
#include "texture_image_decode.h"
#define XXH_INLINE_ALL
#include "../third_party/xxhash.h"
#include "png_write.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif

namespace fs = std::filesystem;
namespace {
constexpr size_t kWords = 1024u * 512u;
constexpr size_t kMaxEntries = 262144;
constexpr size_t kMaxUploads = 8192;
constexpr size_t kMaxRectCache = 256;
constexpr size_t kMaxDumps = 8192;
constexpr size_t kMaxDumpQueue = 8;
constexpr size_t kMaxDecodeQueue = 32;
constexpr size_t kDefaultBudget = 64u * 1024u * 1024u;
constexpr size_t kMaxEncoded = 64u * 1024u * 1024u;
constexpr size_t kMaxSnapshots = 64u * 1024u * 1024u;
constexpr size_t kMaxPaletteRecords = 8192;
constexpr size_t kMaxDumpBytes = 32u * 1024u * 1024u;
constexpr size_t kMaxParts = 64;

void error_text(char* dest, size_t capacity, const std::string& text) {
    if (dest && capacity) std::snprintf(dest, capacity, "%s", text.c_str());
}
std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
}
std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
unsigned expansion(unsigned depth) { return depth == 0 ? 4 : depth == 1 ? 2 : 1; }
struct Rect { unsigned x, y, w, h; };
bool intersects(Rect a, Rect b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}
bool contains(Rect a, Rect b) {
    return b.x >= a.x && b.y >= a.y && b.x + b.w <= a.x + a.w && b.y + b.h <= a.y + a.h;
}
Rect intersection(Rect a, Rect b) {
    const unsigned x = std::max(a.x,b.x), y = std::max(a.y,b.y);
    const unsigned right = std::min(a.x+a.w,b.x+b.w), bottom = std::min(a.y+a.h,b.y+b.h);
    return {x,y,right > x ? right-x : 0,bottom > y ? bottom-y : 0};
}
Rect united(Rect a, Rect b) {
    if (!a.w || !a.h) return b;
    const unsigned x = std::min(a.x,b.x), y = std::min(a.y,b.y);
    return {x,y,std::max(a.x+a.w,b.x+b.w)-x,std::max(a.y+a.h,b.y+b.h)-y};
}
bool valid_rect(Rect r) { return r.w && r.h && r.x + r.w <= 1024 && r.y + r.h <= 512; }

template<class T> bool number(const std::string& text, T& value, int base = 10) {
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, base);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
bool dimensions(const std::string& s, uint16_t& w, uint16_t& h) {
    const size_t at = s.find('x');
    return at != std::string::npos && number(s.substr(0, at), w) &&
           number(s.substr(at + 1), h) && w && h;
}
bool valid_key(const DuckTextureKey& k) {
    if (k.kind > DUCK_TEXTURE_PAGE || k.depth > 2 || k.semitransparent > 1 ||
        !k.source_width_words || k.source_width_words > 1024 || !k.source_height ||
        k.source_height > 512 || !k.width || !k.height) return false;
    const unsigned ppw = expansion(k.depth);
    if (k.offset_x % ppw || k.width % ppw ||
        unsigned(k.offset_x) + k.width > unsigned(k.source_width_words) * ppw ||
        unsigned(k.offset_y) + k.height > k.source_height) return false;
    if (k.kind == DUCK_TEXTURE_PAGE && (k.source_width_words != 256 / ppw ||
        k.source_height != 256 || unsigned(k.offset_x) + k.width > 256 ||
        unsigned(k.offset_y) + k.height > 256)) return false;
    if (k.depth < 2 && (k.palette_min > k.palette_max ||
        k.palette_max >= (k.depth == 0 ? 16 : 256))) return false;
    return k.depth < 2 || (!k.palette_hash && !k.palette_min && !k.palette_max);
}
std::string stem(const DuckTextureKey& k) {
    char result[192];
    if (!duck_texture_format_name(&k, result, sizeof(result))) return {};
    return result;
}
bool query_rect(const HdTextureDrawQuery* q, Rect& r) {
    if (!q || !q->vram || q->vram_word_count < kWords || q->depth > 2 ||
        q->u_first > q->u_last || q->v_first > q->v_last ||
        q->page_x >= 1024 || q->page_x % 64 || q->page_y > 256 || q->page_y % 256)
        return false;
    const unsigned ppw = expansion(q->depth);
    /* A page that wraps in X is outside the initial safe subset. */
    if (q->page_x + 256 / ppw > 1024) return false;
    r = {q->page_x + q->u_first / ppw, unsigned(q->page_y) + q->v_first,
         unsigned(q->u_last / ppw - q->u_first / ppw + 1),
         unsigned(q->v_last - q->v_first + 1)};
    return valid_rect(r);
}

uint64_t palette_hash(const HdTextureDrawQuery& q, unsigned first, unsigned last, bool& valid) {
    valid = false;
    if (q.depth == 2) { valid = true; return 0; }
    const unsigned full = q.depth == 0 ? 16 : 256;
    if (first > last || last >= full || q.clut_x >= 1024 || q.clut_y >= 512 || q.clut_x % 16)
        return 0;
    unsigned count = last - first + 1;
    if (first == 0 && last == full - 1) count = std::min(full, 1024u - q.clut_x);
    else if (q.clut_x + last >= 1024) return 0;
    /* Compatibility with the pinned upstream behavior: range length is used,
     * but min does not offset the hashed palette pointer. */
    valid = true;
    return duck_texture_hash_words_le(q.vram + q.clut_y * 1024 + q.clut_x, count);
}

struct Entry { DuckTextureKey key{}; std::string path; bool ambiguous = false; };
struct Observation {
    Rect used{}; /* Original source-space native words. */
    std::array<uint16_t,256> palette{};
    uint64_t palette_hash = 0;
    uint16_t palette_size = 0, clut_x = 0, clut_y = 0;
    uint16_t page_x = 0, page_y = 0;
    uint8_t depth = 0;
    bool semi = false;
};
struct Source {
    Rect rect{};
    uint64_t hash = 0;
    std::vector<uint16_t> words;
    std::vector<Observation> records;
    DuckTexturePack* dump_owner = nullptr;
    uint8_t kind = DUCK_TEXTURE_UPLOAD;
    bool used = false;
    bool dump_dirty = false;
};
struct Upload { std::shared_ptr<Source> source; Rect active{}; unsigned splits = 0; };
struct PageSource { std::shared_ptr<Source> source; uint8_t depth = 0; uint16_t clut_x = 0, clut_y = 0; };
struct CopyUpdate { Rect original, active; unsigned splits; };
struct RectHash { Rect rect; uint64_t hash; const uint16_t* vram; };
struct PageGroup {
    Rect texels;
    std::unordered_map<uint64_t, std::vector<size_t>> hashes;
};
struct MatchCache {
    HdTextureDrawQuery query;
    int st, status;
    std::vector<DuckTextureMatch> results;
};
struct Image { std::vector<uint8_t> rgba; uint32_t w = 0, h = 0; };
struct Lease { std::shared_ptr<Image> image; };
enum class DecodeStatus { Queued, Loading, Ready, Failed };
struct Decode {
    DecodeStatus status = DecodeStatus::Queued;
    std::string path;
    std::shared_ptr<Image> image;
    uint64_t use = 0;
};
struct Dump { fs::path path; std::shared_ptr<Image> image; };
struct DumpHistory { std::unordered_set<std::string> names; std::deque<std::string> order; };
/* png_write.h's C-compatible CRC table is mutable during initialization.
 * Different pack workers therefore serialize this TU's calls to the writer. */
std::mutex png_writer_mutex;
std::atomic<uint64_t> dump_sequence{static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};

FILE* create_exclusive(const fs::path& path) {
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (handle == INVALID_HANDLE_VALUE) return nullptr;
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle),_O_BINARY | _O_WRONLY);
    if (fd < 0) { CloseHandle(handle); return nullptr; }
    FILE* file = _fdopen(fd,"wb");
    if (!file) _close(fd);
#else
    const int fd = ::open(path.c_str(),O_WRONLY | O_CREAT | O_EXCL,0600);
    if (fd < 0) return nullptr;
    FILE* file = ::fdopen(fd,"wb");
    if (!file) ::close(fd);
#endif
    return file;
}

bool publish_exclusive(const fs::path& temp, const fs::path& target) {
#ifdef _WIN32
    if (MoveFileExW(temp.c_str(),target.c_str(),0)) return true;
    const DWORD code = GetLastError();
    if (code != ERROR_ALREADY_EXISTS && code != ERROR_FILE_EXISTS) throw std::runtime_error("cannot publish texture dump PNG");
#else
    if (::link(temp.c_str(),target.c_str()) == 0) return true;
    if (errno != EEXIST) throw std::runtime_error("cannot publish texture dump PNG");
#endif
    return false;
}

struct Worker {
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::unordered_map<uint64_t, Decode> decoded;
    std::deque<uint64_t> decode_queue;
    std::deque<Dump> dump_queue;
    std::thread thread;
    size_t budget = kDefaultBudget, used = 0;
    size_t dump_bytes = 0;
    uint64_t tick = 0;
    uint64_t decoded_images = 0, decode_evictions = 0;
    bool stop = false;
    bool dump_busy = false;
    std::string dump_error;
    ~Worker() {
        { std::lock_guard<std::mutex> lock(mutex); stop = true; }
        wake.notify_all();
        if (thread.joinable()) thread.join();
    }
    void evict(size_t incoming) {
        while (used + incoming > budget) {
            auto candidate = decoded.end();
            for (auto it = decoded.begin(); it != decoded.end(); ++it)
                if (it->second.status == DecodeStatus::Ready &&
                    (candidate == decoded.end() || it->second.use < candidate->second.use)) candidate = it;
            if (candidate == decoded.end()) break;
            used -= candidate->second.image->rgba.size();
            ++decode_evictions;
            decoded.erase(candidate);
        }
    }
};

void worker_main(Worker* w) {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    for (;;) {
        uint64_t id = 0; std::string path; Dump dump; size_t budget = 0;
        {
            std::unique_lock<std::mutex> lock(w->mutex);
            w->wake.wait(lock, [&] { return w->stop || !w->decode_queue.empty() || !w->dump_queue.empty(); });
            /* Finish queued dumps on shutdown so the final frame is durable. */
            if (w->stop && w->dump_queue.empty()) return;
            if (!w->stop && w->dump_queue.empty() && !w->decode_queue.empty()) {
                id = w->decode_queue.front(); w->decode_queue.pop_front();
                auto it = w->decoded.find(id);
                if (it == w->decoded.end()) continue;
                it->second.status = DecodeStatus::Loading;
                path = it->second.path; budget = w->budget;
            } else {
                dump = std::move(w->dump_queue.front()); w->dump_queue.pop_front(); w->dump_busy = true;
                w->wake.notify_all();
            }
        }
        if (id) {
            std::shared_ptr<Image> image;
            try {
                std::ifstream input(fs::u8path(path), std::ios::binary | std::ios::ate);
                const auto length = input ? input.tellg() : std::streampos(-1);
                if (length > 0 && static_cast<uint64_t>(length) <= kMaxEncoded) {
                    std::vector<uint8_t> encoded(static_cast<size_t>(length));
                    input.seekg(0);
                    DecodedTextureImage decoded; std::string error;
                    if (input.read(reinterpret_cast<char*>(encoded.data()), length) &&
                        texture_image_decode(encoded.data(),encoded.size(),budget,decoded,error)) {
                        image = std::make_shared<Image>(); image->w = decoded.width; image->h = decoded.height;
                        image->rgba = std::move(decoded.rgba);
                    }
                }
            } catch (...) { image.reset(); }
            std::lock_guard<std::mutex> lock(w->mutex);
            auto it = w->decoded.find(id);
            if (it == w->decoded.end()) continue;
            if (image) w->evict(image->rgba.size());
            if (!image || w->used + image->rgba.size() > w->budget) { it->second.status = DecodeStatus::Failed; continue; }
            w->used += image->rgba.size(); it->second.image = std::move(image);
            ++w->decoded_images;
            it->second.status = DecodeStatus::Ready; it->second.use = ++w->tick;
        } else {
            bool created = false;
            fs::path temporary = dump.path;
            temporary += ".partial-" + std::to_string(++dump_sequence);
            try {
                std::error_code ec;
                fs::create_directories(dump.path.parent_path(), ec);
                if (ec) throw std::runtime_error("cannot create texture dump directory: " + ec.message());
                if (!(fs::exists(dump.path,ec) && !ec)) {
                FILE* file = create_exclusive(temporary);
                if (!file) throw std::runtime_error("cannot create temporary texture dump PNG");
                created = true;
                bool success = false;
                try {
                    std::lock_guard<std::mutex> write_lock(png_writer_mutex);
                    success = png_write_rgba(file,dump.image->rgba.data(),dump.image->w,dump.image->h) != 0;
                } catch (...) { std::fclose(file); throw; }
                if (std::fclose(file) != 0 || !success) throw std::runtime_error("cannot write texture dump PNG");
                publish_exclusive(temporary,dump.path);
                fs::remove(temporary,ec);
                }
            } catch (const std::exception& e) {
                if (created) { std::error_code ignored; fs::remove(temporary,ignored); }
                std::lock_guard<std::mutex> lock(w->mutex); w->dump_error = e.what();
            } catch (...) {
                if (created) { std::error_code ignored; fs::remove(temporary,ignored); }
                std::lock_guard<std::mutex> lock(w->mutex); w->dump_error = "texture dump allocation failed";
            }
            {
                std::lock_guard<std::mutex> lock(w->mutex);
                w->dump_bytes -= dump.image->rgba.size(); w->dump_busy = false;
            }
            w->wake.notify_all();
        }
    }
}
} // namespace

struct DuckTexturePack {
    std::string root, diagnostic;
    mutable std::string reported_diagnostic;
    fs::path replacements, dumps;
    std::vector<Entry> entries;
    std::array<std::unordered_map<uint64_t, std::vector<size_t>>, 3> uploads_by_hash;
    std::array<std::unordered_map<uint64_t, PageGroup>, 3> page_groups;
    std::unordered_map<std::string, size_t> by_name;
    std::vector<Upload> uploads;
    std::vector<PageSource> pages;
    std::deque<std::shared_ptr<Source>> sources;
    std::vector<CopyUpdate> copy_updates;
    Rect copy_destination{};
    bool copy_pending = false;
    std::vector<RectHash> rect_cache;
    std::vector<MatchCache> match_cache;
    std::shared_ptr<DumpHistory> dump_history = std::make_shared<DumpHistory>();
    DuckTextureConfig config{0,0,0,16,16,128,128,0,0,0,1,0,1,0,0};
    size_t snapshot_bytes = 0, palette_records = 0;
    size_t queued_dumps = 0;
    size_t snapshot_budget = kMaxSnapshots, rect_cache_limit = kMaxRectCache;
    uint64_t revision = 1;
    std::string lifecycle_error;
    size_t ignored = 0, ambiguous = 0;
    bool geometry_limit_reported = false, dump_limit_reported = false;
    unsigned dump_draw_tick = 0;
    std::chrono::steady_clock::time_point next_dump_checkpoint =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    Worker worker;
};

namespace {
void note(DuckTexturePack& p, const std::string& s) {
    ++p.ignored;
    if (p.diagnostic.size() < 2048) p.diagnostic += (p.diagnostic.empty() ? "" : "; ") + s;
}
void add_entry(DuckTexturePack& p, const DuckTextureKey& key, const fs::path& path) {
    const std::string name = stem(key);
    auto previous = p.by_name.find(name);
    if (previous != p.by_name.end()) {
        auto& entry = p.entries[previous->second];
        if (!entry.ambiguous) { entry.ambiguous = true; ++p.ambiguous; }
        return;
    }
    if (p.entries.size() >= kMaxEntries) throw std::runtime_error("texture replacement entry limit (262144) exceeded");
    p.by_name.emplace(name, p.entries.size());
    if (key.kind == DUCK_TEXTURE_UPLOAD) p.uploads_by_hash[key.depth][key.source_hash].push_back(p.entries.size());
    else {
        const uint64_t geometry = (uint64_t(key.offset_x) << 48) | (uint64_t(key.offset_y) << 32) |
                                  (uint64_t(key.width) << 16) | key.height;
        PageGroup& group = p.page_groups[key.depth][geometry];
        group.texels = {key.offset_x, key.offset_y, key.width, key.height};
        group.hashes[key.source_hash].push_back(p.entries.size());
    }
    p.entries.push_back({key, path.u8string(), false});
}
uint64_t cached_hash(DuckTexturePack& p, const HdTextureDrawQuery& q, Rect r) {
    for (const auto& saved : p.rect_cache)
        if (saved.vram == q.vram && saved.rect.x == r.x && saved.rect.y == r.y && saved.rect.w == r.w && saved.rect.h == r.h) return saved.hash;
    const uint64_t value = duck_texture_hash_rect(q.vram, q.vram_word_count, r.x, r.y, r.w, r.h);
    if (p.rect_cache_limit) {
        if (p.rect_cache.size() >= p.rect_cache_limit) p.rect_cache.erase(p.rect_cache.begin());
        p.rect_cache.push_back({r, value, q.vram});
    }
    return value;
}
std::string unquote(std::string s) {
    s = trim(s);
    if (s.size() > 1 && ((s.front() == '\'' && s.back() == '\'') || (s.front() == '"' && s.back() == '"')))
        return s.substr(1, s.size() - 2);
    const auto comment = s.find(" #");
    return comment == std::string::npos ? s : trim(s.substr(0, comment));
}
bool image_extension(const fs::path& path) {
    const std::string ext = lower(path.extension().u8string());
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp";
}
bool option(DuckTexturePack& p, const std::string& key, const std::string& value) {
    const std::pair<const char*,uint16_t DuckTextureConfig::*> integers[] = {
        {"MaxVRAMWriteSplits",&DuckTextureConfig::max_vram_write_splits},
        {"MaxVRAMWriteCoalesceWidth",&DuckTextureConfig::max_vram_write_coalesce_width},
        {"MaxVRAMWriteCoalesceHeight",&DuckTextureConfig::max_vram_write_coalesce_height},
        {"DumpTextureWidthThreshold",&DuckTextureConfig::texture_dump_width_threshold},
        {"DumpTextureHeightThreshold",&DuckTextureConfig::texture_dump_height_threshold},
        {"DumpVRAMWriteWidthThreshold",&DuckTextureConfig::vram_write_dump_width_threshold},
        {"DumpVRAMWriteHeightThreshold",&DuckTextureConfig::vram_write_dump_height_threshold}
    };
    for (const auto& item : integers) if (key == item.first) {
        uint16_t parsed = 0;
        if (number(value,parsed)) p.config.*item.second = parsed;
        else note(p,"invalid config.yaml integer: " + key);
        return true;
    }
    const std::pair<const char*,uint8_t DuckTextureConfig::*> booleans[] = {
        {"DumpTexturePages",&DuckTextureConfig::dump_texture_pages},
        {"DumpFullTexturePages",&DuckTextureConfig::dump_full_texture_pages},
        {"DumpTextureForceAlphaChannel",&DuckTextureConfig::dump_texture_force_alpha_channel},
        {"DumpVRAMWriteForceAlphaChannel",&DuckTextureConfig::dump_vram_write_force_alpha_channel},
        {"DumpC16Textures",&DuckTextureConfig::dump_c16_textures},
        {"ReducePaletteRange",&DuckTextureConfig::reduce_palette_range},
        {"ConvertCopiesToWrites",&DuckTextureConfig::convert_copies_to_writes},
        {"ReplacementScaleLinearFilter",&DuckTextureConfig::replacement_scale_linear_filter}
    };
    for (const auto& item : booleans) if (key == item.first) {
        const std::string parsed = lower(value);
        if (parsed == "true" || parsed == "1" || parsed == "yes" || parsed == "on") p.config.*item.second = 1;
        else if (parsed == "false" || parsed == "0" || parsed == "no" || parsed == "off") p.config.*item.second = 0;
        else note(p,"invalid config.yaml boolean: " + key);
        return true;
    }
    if (key == "MaxHashCacheEntries" || key == "MaxHashCacheVRAMUsageMB" || key == "MaxReplacementCacheVRAMUsage") {
        uint32_t parsed = 0;
        if (!number(value,parsed)) note(p,"invalid config.yaml cache limit: " + key);
        else if (key == "MaxHashCacheEntries") p.rect_cache_limit = std::min<size_t>(parsed,4096);
        else if (key == "MaxHashCacheVRAMUsageMB") p.snapshot_budget = std::min<uint64_t>(uint64_t(parsed)*1024*1024,kMaxSnapshots);
        else p.worker.budget = std::min<uint64_t>(uint64_t(parsed)*1024*1024,kDefaultBudget);
        return true;
    }
    return false;
}
void load_config(DuckTexturePack& p, const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return;
    if (fs::file_size(path, ec) > 1024 * 1024 || ec) { note(p, "config.yaml exceeds 1 MiB"); return; }
    std::ifstream file(path); std::string line; bool aliases = false;
    while (std::getline(file, line)) {
        const std::string text = trim(line);
        if (text.empty() || text.front() == '#') continue;
        if (line.size() > 4096) { note(p, "oversized config.yaml line"); continue; }
        const auto colon = text.find(':');
        if (colon == std::string::npos) { note(p, "unsupported config.yaml syntax"); continue; }
        const std::string key = unquote(text.substr(0, colon));
        std::string value = unquote(text.substr(colon + 1));
        const bool indented = line.front() == ' ' || line.front() == '\t';
        if (!indented) {
            aliases = key == "Aliases";
            if (!aliases && !option(p,key,value)) note(p,"unsupported config.yaml option: " + key);
            continue;
        }
        if (!aliases) { note(p,"unsupported nested config.yaml option: " + key); continue; }
        DuckTextureKey parsed{};
        if (!duck_texture_parse_name(key.c_str(), &parsed)) { note(p, "invalid alias key"); continue; }
        if (value == "|" || value == "|-" || value == ">" || value == ">-") {
            if (!std::getline(file, line) || line.find_first_not_of(" \t") < 4) { note(p, "unsupported alias block"); continue; }
            value = trim(line);
        }
        fs::path relative = fs::u8path(value);
        bool unsafe = relative.empty() || relative.is_absolute() || relative.has_root_name();
        for (const auto& part : relative) if (part == "..") unsafe = true;
        if (unsafe || !image_extension(relative)) { note(p, "unsupported alias path"); continue; }
        const fs::path target = p.replacements / relative;
        if (!fs::is_regular_file(target, ec) || ec) { note(p, "alias target missing"); ec.clear(); continue; }
        const auto old = p.by_name.find(stem(parsed));
        /* A direct canonical filename takes precedence over an alias. */
        if (old == p.by_name.end()) add_entry(p, parsed, target);
    }
}

uint64_t snapshot_hash(const Source& source, Rect crop) {
    XXH3_state_t state; XXH3_64bits_reset(&state);
    std::array<uint8_t,2048> bytes{};
    for (unsigned y = crop.y; y < crop.y+crop.h; ++y) {
        const uint16_t* row = source.words.data()+size_t(y)*source.rect.w+crop.x;
        for (unsigned x = 0; x < crop.w; ++x) { bytes[2*x] = row[x]&255; bytes[2*x+1] = row[x]>>8; }
        XXH3_64bits_update(&state,bytes.data(),crop.w*2);
    }
    return XXH3_64bits_digest(&state);
}
bool enqueue_dump(DuckTexturePack& p, const std::string& name, std::shared_ptr<Image> image) {
    if (p.dump_history->names.count(name)) return true;
    Worker& w = p.worker;
    std::unique_lock<std::mutex> lock(w.mutex);
    if (!w.thread.joinable()) w.thread = std::thread(worker_main,&w);
    const size_t bytes = image->rgba.size();
    w.wake.wait(lock,[&] { return !w.dump_error.empty() ||
        (w.dump_queue.size() < kMaxDumpQueue && w.dump_bytes+bytes <= kMaxDumpBytes); });
    if (!w.dump_error.empty()) { p.lifecycle_error = w.dump_error; return false; }
    /* Bound metadata without permanently stopping authoring. Old names may
     * be requeued, but exclusive publication still preserves existing files. */
    if (p.dump_history->names.size() >= kMaxDumps) { p.dump_history->names.erase(p.dump_history->order.front()); p.dump_history->order.pop_front(); }
    p.dump_history->names.insert(name); p.dump_history->order.push_back(name);
    w.dump_bytes += bytes;
    w.dump_queue.push_back({p.dumps/(name+".png"),std::move(image)});
    ++p.queued_dumps;
    w.wake.notify_one();
    return true;
}
bool finalize_source(DuckTexturePack& p, Source& source, bool keep_observations = false) {
    if (source.dump_owner != &p) return true;
    bool success = true;
    for (const Observation& record : source.records) {
        const unsigned ppw = expansion(record.depth);
        Rect crop = record.used;
        if (source.kind == DUCK_TEXTURE_PAGE && p.config.dump_full_texture_pages)
            crop = {0,0,source.rect.w,source.rect.h};
        if (record.depth == 2 && !p.config.dump_c16_textures) continue;
        if (crop.w*ppw < p.config.texture_dump_width_threshold || crop.h < p.config.texture_dump_height_threshold) continue;
        DuckTextureKey key{};
        key.kind = source.kind; key.depth = record.depth;
        key.semitransparent = record.semi && !p.config.dump_texture_force_alpha_channel;
        key.source_hash = source.kind == DUCK_TEXTURE_UPLOAD ? source.hash : snapshot_hash(source,crop);
        key.source_width_words = static_cast<uint16_t>(source.rect.w);
        key.source_height = static_cast<uint16_t>(source.rect.h);
        key.offset_x = static_cast<uint16_t>(crop.x*ppw); key.offset_y = static_cast<uint16_t>(crop.y);
        key.width = static_cast<uint16_t>(crop.w*ppw); key.height = static_cast<uint16_t>(crop.h);
        key.palette_hash = record.palette_hash;
        if (record.depth < 2) {
            unsigned minimum = 0, maximum = record.depth == 0 ? 15 : 255;
            if (p.config.reduce_palette_range) {
                minimum = maximum; maximum = 0;
                const unsigned mask = record.depth == 0 ? 15 : 255;
                const unsigned bits = record.depth == 0 ? 4 : 8;
                for (unsigned y = crop.y; y < crop.y+crop.h; ++y)
                    for (unsigned x = crop.x; x < crop.x+crop.w; ++x) {
                        const uint16_t word = source.words[size_t(y)*source.rect.w+x];
                        for (unsigned n = 0; n < ppw; ++n) {
                            const unsigned index = (word>>(n*bits))&mask;
                            minimum = std::min(minimum,index); maximum = std::max(maximum,index);
                        }
                    }
                minimum = std::min<unsigned>(minimum,record.palette_size-1);
                maximum = std::min<unsigned>(maximum,record.palette_size-1);
                /* Pinned upstream hashes the FIRST range-size entries, not an
                 * offset into the palette. Keep that wire-format quirk. */
                key.palette_hash = duck_texture_hash_words_le(record.palette.data(),maximum-minimum+1);
            }
            key.palette_min = static_cast<uint8_t>(minimum); key.palette_max = static_cast<uint8_t>(maximum);
        }
        const std::string name = stem(key);
        if (name.empty() || p.dump_history->names.count(name)) continue;
        auto image = std::make_shared<Image>(); image->w = key.width; image->h = key.height;
        image->rgba.resize(size_t(image->w)*image->h*4);
        for (unsigned y = 0; y < image->h; ++y) for (unsigned x = 0; x < image->w; ++x) {
            uint16_t color = source.words[size_t(crop.y+y)*source.rect.w+crop.x+x/ppw];
            if (record.depth < 2) {
                const unsigned bits = record.depth == 0 ? 4 : 8;
                const unsigned index = (color>>((x%ppw)*bits))&(record.depth == 0 ? 15 : 255);
                color = record.palette[index];
            }
            uint8_t* pixel = image->rgba.data()+(size_t(y)*image->w+x)*4;
            for (unsigned c = 0; c < 3; ++c) pixel[c] = static_cast<uint8_t>((((color>>(5*c))&31)*255+15)/31);
            pixel[3] = p.config.dump_texture_force_alpha_channel ? 255 : color == 0 ? 0 :
                key.semitransparent && (color&0x8000) ? 143 : 255;
        }
        if (!enqueue_dump(p,name,std::move(image))) success = false;
    }
    if (success) {
        source.dump_dirty = false;
        if (!keep_observations) { p.palette_records -= source.records.size(); source.records.clear(); }
    }
    return success;
}
bool has_residency(const DuckTexturePack& p, const Source* source) {
    for (const auto& upload : p.uploads) if (upload.source.get() == source) return true;
    for (const auto& page : p.pages) if (page.source.get() == source) return true;
    return false;
}
void retire_unused(DuckTexturePack& p) {
    for (auto it = p.sources.begin(); it != p.sources.end();) {
        if (has_residency(p,it->get())) { ++it; continue; }
        if (!finalize_source(p,**it)) {
            /* Keep failed pending records bounded and visible to the caller. */
            p.lifecycle_error = p.lifecycle_error.empty() ? "texture dump could not be finalized" : p.lifecycle_error;
            ++it; continue;
        }
        p.snapshot_bytes -= (*it)->words.size()*sizeof(uint16_t); it = p.sources.erase(it);
    }
}
std::shared_ptr<Source> capture_source(DuckTexturePack& p, Rect rect, const uint16_t* vram, uint8_t kind) {
    const size_t bytes = size_t(rect.w)*rect.h*sizeof(uint16_t);
    if (bytes > p.snapshot_budget) {
        p.lifecycle_error = "upload snapshot exceeds bounded authoring budget";
        return {};
    }
    while (p.snapshot_bytes+bytes > p.snapshot_budget || p.uploads.size() >= kMaxUploads || p.sources.size() >= kMaxUploads) {
        if (p.sources.empty()) return {};
        const auto victim = p.sources.front();
        if (!finalize_source(p,*victim)) return {};
        p.uploads.erase(std::remove_if(p.uploads.begin(),p.uploads.end(),[&](const Upload& u) { return u.source == victim; }),p.uploads.end());
        p.pages.erase(std::remove_if(p.pages.begin(),p.pages.end(),[&](const PageSource& u) { return u.source == victim; }),p.pages.end());
        p.snapshot_bytes -= victim->words.size()*sizeof(uint16_t); p.sources.pop_front();
        p.match_cache.clear();
    }
    auto source = std::make_shared<Source>(); source->rect = rect; source->kind = kind; source->dump_owner = &p;
    source->words.resize(size_t(rect.w)*rect.h);
    for (unsigned y = 0; y < rect.h; ++y)
        std::copy_n(vram+(rect.y+y)*1024+rect.x,rect.w,source->words.data()+size_t(y)*rect.w);
    source->hash = duck_texture_hash_words_le(source->words.data(),source->words.size());
    p.snapshot_bytes += bytes; p.sources.push_back(source);
    return source;
}
bool record_usage(DuckTexturePack& p, Source& source, Rect global, const HdTextureDrawQuery& q, bool semi) {
    source.used = true;
    Observation current{}; current.depth = q.depth; current.semi = semi;
    current.page_x = q.page_x; current.page_y = q.page_y;
    current.clut_x = q.clut_x; current.clut_y = q.clut_y;
    current.used = {global.x-source.rect.x,global.y-source.rect.y,global.w,global.h};
    if (q.depth < 2) {
        if (q.clut_x >= 1024 || q.clut_x%16 || q.clut_y >= 512) return false;
        current.palette_size = static_cast<uint16_t>(std::min(q.depth == 0 ? 16u : 256u,1024u-q.clut_x));
        std::copy_n(q.vram+q.clut_y*1024+q.clut_x,current.palette_size,current.palette.data());
        current.palette_hash = duck_texture_hash_words_le(current.palette.data(),current.palette_size);
    }
    for (Observation& previous : source.records) {
        if (previous.depth != current.depth || previous.palette_hash != current.palette_hash ||
            previous.palette != current.palette || (q.depth < 2 &&
            (previous.clut_x != q.clut_x || previous.clut_y != q.clut_y))) continue;
        const Rect combined = united(previous.used,current.used);
        const bool changed = combined.x != previous.used.x || combined.y != previous.used.y ||
            combined.w != previous.used.w || combined.h != previous.used.h || (!previous.semi && semi);
        previous.used = combined; previous.semi |= semi;
        source.dump_dirty |= changed;
        return changed;
    }
    if (p.palette_records >= kMaxPaletteRecords) {
        for (const auto& saved : p.sources) if (saved->dump_owner == &p && !saved->records.empty()) {
            if (!finalize_source(p,*saved)) return false;
            if (p.palette_records < kMaxPaletteRecords) break;
        }
    }
    if (p.palette_records >= kMaxPaletteRecords) { p.lifecycle_error = "palette record budget exhausted"; return false; }
    source.records.push_back(std::move(current)); ++p.palette_records;
    source.dump_dirty = true;
    return true;
}
} // namespace

extern "C" {
int duck_texture_parse_name(const char* filename, DuckTextureKey* out) {
    if (!filename || !out) return 0;
    try {
        std::string text(filename);
        if (text.size() > 192 || text.find_first_of("/\\") != std::string::npos) return 0;
        const auto dot = text.find('.');
        if (dot != std::string::npos) {
            if (!image_extension(fs::u8path(text))) return 0;
            text.resize(dot);
        }
        std::vector<std::string> parts;
        size_t begin = 0;
        for (size_t at = 0; at <= text.size(); ++at) if (at == text.size() || text[at] == '-') {
            parts.push_back(text.substr(begin, at - begin)); begin = at + 1;
        }
        if (parts.size() != 7 && parts.size() != 10) return 0;
        DuckTextureKey k{};
        if (parts[0] == "texupload") k.kind = DUCK_TEXTURE_UPLOAD;
        else if (parts[0] == "texpage") k.kind = DUCK_TEXTURE_PAGE;
        else return 0;
        std::string mode = parts[1];
        if (mode.size() > 2 && mode.substr(0, 2) == "ST") { k.semitransparent = 1; mode.erase(0, 2); }
        if (mode == "P4") k.depth = 0; else if (mode == "P8") k.depth = 1; else if (mode == "C16") k.depth = 2; else return 0;
        if (parts[2].size() != 16 || !number(parts[2], k.source_hash, 16)) return 0;
        size_t field = 3;
        if (k.depth < 2) {
            if (parts.size() != 10 || parts[3].size() != 16 || !number(parts[3], k.palette_hash, 16)) return 0;
            ++field;
        } else if (parts.size() != 7) return 0;
        if (!dimensions(parts[field++], k.source_width_words, k.source_height) ||
            !number(parts[field++], k.offset_x) || !number(parts[field++], k.offset_y) ||
            !dimensions(parts[field++], k.width, k.height)) return 0;
        if (k.depth < 2) {
            unsigned a = 0, b = 0;
            if (parts[field].empty() || parts[field][0] != 'P' || !number(parts[field].substr(1), a) ||
                !number(parts[field + 1], b) || a > 255 || b > 255) return 0;
            k.palette_min = static_cast<uint8_t>(a); k.palette_max = static_cast<uint8_t>(b);
        }
        if (!valid_key(k)) return 0;
        *out = k; return 1;
    } catch (...) { return 0; }
}

int duck_texture_format_name(const DuckTextureKey* k, char* out, size_t capacity) {
    if (!k || !out || !capacity || !valid_key(*k)) return 0;
    const char* kind = k->kind == DUCK_TEXTURE_UPLOAD ? "texupload" : "texpage";
    const char* mode = k->depth == 0 ? "P4" : k->depth == 1 ? "P8" : "C16";
    const char* st = k->semitransparent ? "ST" : "";
    int count;
    if (k->depth < 2) count = std::snprintf(out, capacity,
        "%s-%s%s-%016llX-%016llX-%ux%u-%u-%u-%ux%u-P%u-%u", kind, st, mode,
        static_cast<unsigned long long>(k->source_hash), static_cast<unsigned long long>(k->palette_hash),
        k->source_width_words, k->source_height, k->offset_x, k->offset_y, k->width, k->height, k->palette_min, k->palette_max);
    else count = std::snprintf(out, capacity, "%s-%s%s-%016llX-%ux%u-%u-%u-%ux%u", kind, st, mode,
        static_cast<unsigned long long>(k->source_hash), k->source_width_words, k->source_height,
        k->offset_x, k->offset_y, k->width, k->height);
    return count >= 0 && static_cast<size_t>(count) < capacity;
}

uint64_t duck_texture_hash_words_le(const uint16_t* words, size_t count) {
    if ((!words && count) || count > kWords) return 0;
    XXH3_state_t state;
    XXH3_64bits_reset(&state);
    std::array<uint8_t, 2048> bytes{};
    while (count) {
        const size_t chunk = std::min(count, bytes.size() / 2);
        for (size_t i = 0; i < chunk; ++i) { bytes[2 * i] = words[i] & 255; bytes[2 * i + 1] = words[i] >> 8; }
        XXH3_64bits_update(&state, bytes.data(), chunk * 2);
        words += chunk; count -= chunk;
    }
    return XXH3_64bits_digest(&state);
}
uint64_t duck_texture_hash_rect(const uint16_t* vram, size_t count, uint16_t x, uint16_t y,
                               uint16_t width, uint16_t height) {
    if (!vram || count < kWords || !valid_rect({x, y, width, height})) return 0;
    XXH3_state_t state; XXH3_64bits_reset(&state);
    std::array<uint8_t, 2048> bytes{};
    for (unsigned row = 0; row < height; ++row) {
        const uint16_t* src = vram + (y + row) * 1024 + x;
        for (unsigned i = 0; i < width; ++i) { bytes[2 * i] = src[i] & 255; bytes[2 * i + 1] = src[i] >> 8; }
        XXH3_64bits_update(&state, bytes.data(), size_t(width) * 2);
    }
    return XXH3_64bits_digest(&state);
}

int duck_texture_pack_create(const char* root, DuckTexturePack** out, char* error, size_t capacity) {
    if (out) *out = nullptr;
    if (!out || !root || !root[0]) { error_text(error, capacity, "texture pack root is unset"); return 0; }
    try {
        auto p = std::make_unique<DuckTexturePack>();
        std::error_code ec; fs::path input = fs::absolute(fs::u8path(root), ec).lexically_normal();
        if (ec || (fs::exists(input, ec) && !fs::is_directory(input, ec))) {
            error_text(error, capacity, "texture pack root is not a directory"); return 0;
        }
        if (lower(input.filename().u8string()) == "replacements") { p->replacements = input; input = input.parent_path(); }
        else p->replacements = input / "replacements";
        p->root = input.u8string(); p->dumps = input / "dumps";
        std::vector<fs::path> paths;
        if (fs::is_directory(p->replacements, ec)) {
            size_t scanned = 0;
            for (fs::recursive_directory_iterator it(p->replacements, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec)) {
                if (++scanned > kMaxEntries * 4) throw std::runtime_error("texture replacement directory scan limit exceeded");
                if (it.depth() >= 16) { if (it->is_directory(ec)) it.disable_recursion_pending(); note(*p, "replacement nesting exceeds 16 levels"); }
                if (it->is_regular_file(ec)) {
                    const std::string name = it->path().filename().u8string();
                    if (name.rfind("texupload-", 0) != 0 && name.rfind("texpage-", 0) != 0 && name.rfind("vram-write-", 0) != 0) continue;
                    if (!image_extension(it->path())) { note(*p, "unsupported replacement image format"); continue; }
                    if (paths.size() >= kMaxEntries) throw std::runtime_error("texture replacement file limit (262144) exceeded");
                    paths.push_back(it->path());
                }
            }
            if (ec) { error_text(error, capacity, "cannot scan replacement directory: " + ec.message()); return 0; }
        } else if (ec && ec != std::errc::no_such_file_or_directory) {
            error_text(error, capacity, "cannot inspect replacement directory: " + ec.message()); return 0;
        }
        std::sort(paths.begin(), paths.end());
        for (const auto& path : paths) {
            DuckTextureKey k{};
            if (duck_texture_parse_name(path.filename().u8string().c_str(), &k)) add_entry(*p, k, path);
            else note(*p, "invalid or unsupported replacement filename");
        }
        load_config(*p, input / "config.yaml");
        *out = p.release(); return 1;
    } catch (const std::exception& e) { error_text(error, capacity, e.what()); return 0; }
      catch (...) { error_text(error, capacity, "texture pack allocation failed"); return 0; }
}
void duck_texture_pack_destroy(DuckTexturePack* p) {
    if (p) { duck_texture_pack_flush_dumps(p,nullptr,0); delete p; }
}
void duck_texture_pack_get_info(const DuckTexturePack* p, DuckTexturePackInfo* out) {
    if (!out) return;
    *out = {};
    if (p) {
        p->reported_diagnostic = p->diagnostic;
        if (!p->lifecycle_error.empty()) p->reported_diagnostic += "; " + p->lifecycle_error;
        { std::lock_guard<std::mutex> lock(p->worker.mutex);
          out->decoded_images = p->worker.decoded_images;
          out->decode_evictions = p->worker.decode_evictions;
          out->decoded_bytes = p->worker.used;
          if (!p->worker.dump_error.empty()) p->reported_diagnostic += "; " + p->worker.dump_error; }
        out->root = p->root.c_str(); out->diagnostic = p->reported_diagnostic.c_str(); out->replacement_count = p->entries.size();
        out->ambiguous_count = p->ambiguous; out->ignored_count = p->ignored; out->queued_dump_count = p->queued_dumps;
        out->snapshot_bytes = p->snapshot_bytes;
        for (const auto& source : p->sources) if (source->dump_owner == p && source->dump_dirty && !source->records.empty()) {
            ++out->pending_dump_sources; out->pending_palette_records += source->records.size();
        }
    }
}
void duck_texture_pack_get_config(const DuckTexturePack* p, DuckTextureConfig* out) { if (out) *out = p ? p->config : DuckTextureConfig{}; }
int duck_texture_pack_linear_filter(const DuckTexturePack* p) { return p && p->config.replacement_scale_linear_filter; }
uint64_t duck_texture_pack_revision(const DuckTexturePack* p) { return p ? p->revision : 0; }
void duck_texture_pack_invalidate(DuckTexturePack* p, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (!p || !w || !h) return;
    if (w > 1024 || h > 512) { duck_texture_pack_reset_tracking(p); return; }
    try {
    const unsigned px = x % 1024, py = y % 512;
    const unsigned w0 = std::min(unsigned(w), 1024 - px), h0 = std::min(unsigned(h), 512 - py);
    const std::array<Rect, 4> regions{{{px,py,w0,h0},{0,py,unsigned(w)-w0,h0},
                                    {px,0,w0,unsigned(h)-h0},{0,0,unsigned(w)-w0,unsigned(h)-h0}}};
    ++p->revision;
    for (const auto& r : regions) if (r.w && r.h) {
        std::vector<Upload> survivors; survivors.reserve(p->uploads.size());
        for (const auto& upload : p->uploads) {
            const Rect cut = intersection(upload.active,r);
            if (!cut.w || !cut.h) { if (survivors.size() < kMaxUploads) survivors.push_back(upload); continue; }
            if (upload.splits >= p->config.max_vram_write_splits || contains(r,upload.active)) continue;
            const Rect a = upload.active;
            const Rect fragments[] = {
                {a.x,a.y,a.w,cut.y-a.y}, {a.x,cut.y+cut.h,a.w,a.y+a.h-cut.y-cut.h},
                {a.x,cut.y,cut.x-a.x,cut.h}, {cut.x+cut.w,cut.y,a.x+a.w-cut.x-cut.w,cut.h}
            };
            if (survivors.size()+4 > kMaxUploads) continue;
            for (const Rect& fragment : fragments) if (fragment.w && fragment.h)
                survivors.push_back({upload.source,fragment,upload.splits+1});
        }
        p->uploads.swap(survivors);
        p->pages.erase(std::remove_if(p->pages.begin(),p->pages.end(),[&](const PageSource& page) {
            if (intersects(page.source->rect,r)) return true;
            for (const auto& record : page.source->records)
                if (record.depth < 2 && intersects({record.clut_x,record.clut_y,record.palette_size,1},r)) return true;
            return false;
        }),p->pages.end());
        p->rect_cache.erase(std::remove_if(p->rect_cache.begin(), p->rect_cache.end(), [&](const RectHash& v){ return intersects(v.rect,r); }), p->rect_cache.end());
        p->match_cache.erase(std::remove_if(p->match_cache.begin(), p->match_cache.end(), [&](const MatchCache& c) {
            const auto& q = c.query;
            const Rect page{q.page_x,q.page_y,256 / expansion(q.depth),256};
            const Rect clut{q.clut_x,q.clut_y,std::min(q.depth == 0 ? 16u : 256u,1024u-q.clut_x),1};
            return intersects(page,r) || (q.depth < 2 && intersects(clut,r));
        }), p->match_cache.end());
    }
    retire_unused(*p);
    } catch (...) {
        /* Allocation/finalization failure must never preserve stale residency
         * after a native write. Pending immutable observations remain owned. */
        p->uploads.clear(); p->pages.clear(); p->rect_cache.clear(); p->match_cache.clear();
    }
}
void duck_texture_pack_reset_tracking(DuckTexturePack* p) {
    if (!p) return;
    const bool flushed = duck_texture_pack_flush_dumps(p,nullptr,0) >= 0;
    p->uploads.clear(); p->pages.clear(); p->copy_updates.clear(); p->copy_pending = false;
    if (flushed) { p->sources.clear(); p->snapshot_bytes = 0; p->palette_records = 0; }
    p->rect_cache.clear(); p->match_cache.clear(); ++p->revision;
}
int duck_texture_pack_copy_tracking(DuckTexturePack* destination, const DuckTexturePack* source) {
    if (!destination || !source) return 0;
    if (destination == source) return 1;
    try {
        auto uploads = source->uploads;
        auto pages = source->pages;
        std::deque<std::shared_ptr<Source>> sources;
        std::unordered_map<Source*,std::shared_ptr<Source>> cloned;
        size_t bytes = 0, records = 0;
        const auto clone = [&](const std::shared_ptr<Source>& item) {
            const auto found = cloned.find(item.get());
            if (found != cloned.end()) return found->second;
            auto copy = std::make_shared<Source>(*item); copy->dump_owner = destination;
            if (destination->root != source->root && !copy->records.empty()) copy->dump_dirty = true;
            cloned.emplace(item.get(),copy); sources.push_back(copy);
            bytes += copy->words.size()*sizeof(uint16_t); records += copy->records.size();
            return copy;
        };
        for (auto& upload : uploads) upload.source = clone(upload.source);
        for (auto& page : pages) page.source = clone(page.source);
        if (bytes > destination->snapshot_budget || duck_texture_pack_flush_dumps(destination,nullptr,0) < 0) return 0;
        destination->uploads.swap(uploads);
        destination->pages.swap(pages); destination->sources.swap(sources);
        destination->snapshot_bytes = bytes; destination->palette_records = records;
        if (destination->root == source->root) destination->dump_history = source->dump_history;
        destination->copy_updates.clear(); destination->copy_pending = false;
        destination->rect_cache.clear();
        destination->match_cache.clear();
        ++destination->revision;
        return 1;
    } catch (...) { return 0; }
}
int duck_texture_pack_track_upload(DuckTexturePack* p, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                  const uint16_t* vram, size_t count) {
    if (!p) return 0;
    duck_texture_pack_invalidate(p, x, y, w, h);
    if (!vram || count < kWords || !valid_rect({x,y,w,h})) return 0;
    try {
        const Rect written{x,y,w,h};
        if (!p->uploads.empty()) {
            const Upload last = p->uploads.back(); const Rect before = last.source->rect;
            const bool vertical = h <= p->config.max_vram_write_coalesce_height && before.x == x && before.w == w && before.y+before.h == y;
            const bool horizontal = w <= p->config.max_vram_write_coalesce_width && before.y == y && before.h == h && before.x+before.w == x;
            if (!last.splits && !last.source->used && (vertical || horizontal)) {
                const Rect combined = united(before,written);
                p->uploads.pop_back(); retire_unused(*p);
                auto merged = capture_source(*p,combined,vram,DUCK_TEXTURE_UPLOAD);
                if (!merged) return 0;
                p->uploads.push_back({merged,combined,0}); p->match_cache.clear(); return 1;
            }
        }
        auto captured = capture_source(*p,written,vram,DUCK_TEXTURE_UPLOAD);
        if (!captured) return 0;
        p->uploads.push_back({captured,written,0});
        p->match_cache.clear(); return 1;
    } catch (...) { return 0; }
}
int duck_texture_pack_begin_copy(DuckTexturePack* p, uint16_t sx, uint16_t sy, uint16_t dx, uint16_t dy,
                                  uint16_t w, uint16_t h, const uint16_t* vram, size_t count) {
    if (!p || !vram || count < kWords) return 0;
    p->copy_updates.clear(); p->copy_pending = false;
    const Rect destination{dx,dy,w,h};
    if (!valid_rect({sx,sy,w,h}) || !valid_rect(destination)) { duck_texture_pack_invalidate(p,dx,dy,w,h); return 0; }
    try {
        if (p->config.convert_copies_to_writes) {
            for (const auto& upload : p->uploads) if (intersects(upload.active,destination)) {
                if (!finalize_source(*p,*upload.source)) return 0;
                if (contains(upload.active,destination))
                    p->copy_updates.push_back({upload.source->rect,upload.active,upload.splits});
            }
            p->uploads.erase(std::remove_if(p->uploads.begin(),p->uploads.end(),[&](const Upload& upload) {
                return contains(upload.active,destination);
            }),p->uploads.end());
        }
        duck_texture_pack_invalidate(p,dx,dy,w,h);
        p->copy_destination = destination; p->copy_pending = true;
        return 1;
    } catch (...) { p->copy_updates.clear(); return 0; }
}
int duck_texture_pack_end_copy(DuckTexturePack* p, const uint16_t* vram, size_t count) {
    if (!p || !p->copy_pending || !vram || count < kWords) return 0;
    p->copy_pending = false;
    try {
        for (const auto& update : p->copy_updates) {
            auto replacement = capture_source(*p,update.original,vram,DUCK_TEXTURE_UPLOAD);
            if (!replacement) { p->copy_updates.clear(); return 0; }
            p->uploads.push_back({replacement,update.active,update.splits});
        }
        p->copy_updates.clear(); p->match_cache.clear(); ++p->revision;
        return 1;
    } catch (...) { p->copy_updates.clear(); return 0; }
}

int duck_texture_pack_match(DuckTexturePack* p, const HdTextureDrawQuery* q, DuckTextureMatch* out) {
    return duck_texture_pack_match_draw(p,q,0,out);
}
int duck_texture_pack_match_draw(DuckTexturePack* p, const HdTextureDrawQuery* q, int st, DuckTextureMatch* out) {
    if (out) *out = {};
    if (!p || !q || !out) return HD_TEXTURE_LOOKUP_ERROR;
    std::array<DuckTextureMatch,kMaxParts> parts{}; size_t count = 0;
    const int status = duck_texture_pack_match_parts(p,q,st,parts.data(),parts.size(),&count);
    if (status != HD_TEXTURE_LOOKUP_FOUND) return status;
    size_t winner = size_t(-1);
    for (size_t i = 0; i < count; ++i) if (parts[i].clip_u <= q->u_first && parts[i].clip_v <= q->v_first &&
        parts[i].clip_u+parts[i].clip_width > q->u_last && parts[i].clip_v+parts[i].clip_height > q->v_last) {
        if (winner != size_t(-1)) return HD_TEXTURE_LOOKUP_AMBIGUOUS;
        winner = i;
    }
    if (winner == size_t(-1)) return HD_TEXTURE_LOOKUP_NONE;
    *out = parts[winner]; return HD_TEXTURE_LOOKUP_FOUND;
}
int duck_texture_pack_match_parts(DuckTexturePack* p, const HdTextureDrawQuery* q, int st,
                                   DuckTextureMatch* output, size_t capacity, size_t* out_count) {
    if (out_count) *out_count = 0;
    if (!p || !q || !out_count || (!output && capacity)) return HD_TEXTURE_LOOKUP_ERROR;
    Rect wanted{}; if (!query_rect(q, wanted)) return HD_TEXTURE_LOOKUP_NONE;
    try {
        for (const auto& c : p->match_cache) {
            const auto& a = c.query;
            if (c.st == (st != 0) && a.vram == q->vram && a.page_x == q->page_x && a.page_y == q->page_y &&
                a.depth == q->depth && a.clut_x == q->clut_x && a.clut_y == q->clut_y &&
                a.u_first == q->u_first && a.u_last == q->u_last && a.v_first == q->v_first && a.v_last == q->v_last) {
                if (c.results.size() > capacity) return HD_TEXTURE_LOOKUP_ERROR;
                std::copy(c.results.begin(),c.results.end(),output); *out_count = c.results.size(); return c.status;
            }
        }
        const unsigned ppw = expansion(q->depth);
        std::vector<DuckTextureMatch> candidates;
        bool over_budget = false; size_t checked = 0;
        const auto consider = [&](size_t index, int u, int v, Rect surviving) {
            if (candidates.size() >= kMaxParts*4) { over_budget = true; return; }
            const Entry& e = p->entries[index]; const auto& k = e.key;
            bool valid = false;
            if (palette_hash(*q,k.palette_min,k.palette_max,valid) != k.palette_hash || !valid) return;
            const int left = std::max({u,(int(surviving.x)-q->page_x)*int(ppw),int(q->u_first)});
            const int top = std::max({v,int(surviving.y)-q->page_y,int(q->v_first)});
            const int right = std::min({u+k.width,(int(surviving.x+surviving.w)-q->page_x)*int(ppw),int(q->u_last)+1});
            const int bottom = std::min({v+k.height,int(surviving.y+surviving.h)-q->page_y,int(q->v_last)+1});
            if (right <= left || bottom <= top) return;
            DuckTextureMatch part{}; part.key = k; part.entry_id = index+1; part.replacement_path = e.path.c_str();
            part.origin_u = u; part.origin_v = v; part.source_width = k.width; part.source_height = k.height;
            part.clip_u = left; part.clip_v = top; part.clip_width = static_cast<uint16_t>(right-left); part.clip_height = static_cast<uint16_t>(bottom-top);
            candidates.push_back(part);
        };
        for (const auto& upload : p->uploads) {
            if (!intersects(upload.active,wanted)) continue;
            upload.source->used = true;
            const Rect source = upload.source->rect;
            const auto found = p->uploads_by_hash[q->depth].find(upload.source->hash);
            if (found == p->uploads_by_hash[q->depth].end()) continue;
            for (const size_t index : found->second) {
                if (++checked > 4096) { over_budget = true; break; }
                const auto& k = p->entries[index].key;
                if (source.w != k.source_width_words || source.h != k.source_height) continue;
                Rect rect{source.x+k.offset_x/ppw,source.y+k.offset_y,unsigned(k.width)/ppw,k.height};
                if (intersects(rect,wanted)) consider(index,(int(source.x)-q->page_x)*int(ppw)+k.offset_x,
                                                     int(source.y)-q->page_y+k.offset_y,upload.active);
                if (over_budget) break;
            }
            if (over_budget) break;
        }
        size_t groups_checked = 0, hashed_bytes = 0;
        for (const auto& item : p->page_groups[q->depth]) {
            const auto& group = item.second; const auto& r = group.texels;
            Rect rect{q->page_x+r.x/ppw,q->page_y+r.y,r.w/ppw,r.h};
            if (!intersects(rect,wanted)) continue;
            const bool cached = std::any_of(p->rect_cache.begin(),p->rect_cache.end(),[&](const RectHash& c) {
                return c.vram == q->vram && c.rect.x == rect.x && c.rect.y == rect.y && c.rect.w == rect.w && c.rect.h == rect.h;
            });
            if (!cached) hashed_bytes += size_t(rect.w) * rect.h * 2;
            if (++groups_checked > 4096 || hashed_bytes > 4u*1024u*1024u) { over_budget = true; break; }
            const auto found = group.hashes.find(cached_hash(*p,*q,rect));
            if (found != group.hashes.end()) for (const size_t index : found->second) {
                if (++checked > 4096) { over_budget = true; break; }
                consider(index,r.x,r.y,rect);
            }
            if (over_budget) break;
        }
        if (over_budget && !p->geometry_limit_reported) {
            note(*p,"page candidate geometry/hash budget exceeded");
            p->geometry_limit_reported = true;
        }
        const auto same_coverage = [](const DuckTextureMatch& a, const DuckTextureMatch& b) {
            const auto& x = a.key; const auto& y = b.key;
            return x.kind == y.kind && x.depth == y.depth && x.source_hash == y.source_hash && x.palette_hash == y.palette_hash &&
                x.palette_min == y.palette_min && x.palette_max == y.palette_max && a.origin_u == b.origin_u && a.origin_v == b.origin_v &&
                a.source_width == b.source_width && a.source_height == b.source_height && a.clip_u == b.clip_u && a.clip_v == b.clip_v &&
                a.clip_width == b.clip_width && a.clip_height == b.clip_height;
        };
        std::vector<DuckTextureMatch> parts;
        for (const auto& candidate : candidates) {
            auto previous = std::find_if(parts.begin(),parts.end(),[&](const DuckTextureMatch& a) { return same_coverage(a,candidate); });
            if (previous == parts.end()) parts.push_back(candidate);
            else if (candidate.key.semitransparent == (st != 0) && previous->key.semitransparent != (st != 0)) *previous = candidate;
        }
        bool ambiguous = false;
        for (const auto& part : parts) ambiguous |= p->entries[part.entry_id-1].ambiguous;
        int status = over_budget || parts.size() > kMaxParts ? HD_TEXTURE_LOOKUP_ERROR : ambiguous ? HD_TEXTURE_LOOKUP_AMBIGUOUS :
            parts.empty() ? HD_TEXTURE_LOOKUP_NONE : HD_TEXTURE_LOOKUP_FOUND;
        if (status != HD_TEXTURE_LOOKUP_FOUND) parts.clear();
        std::sort(parts.begin(),parts.end(),[](const DuckTextureMatch& a,const DuckTextureMatch& b) {
            if (a.key.kind != b.key.kind) return a.key.kind == DUCK_TEXTURE_PAGE;
            const unsigned area_a = unsigned(a.source_width)*a.source_height, area_b = unsigned(b.source_width)*b.source_height;
            if (area_a != area_b) return area_a > area_b;
            if (a.entry_id != b.entry_id) return a.entry_id < b.entry_id;
            if (a.clip_v != b.clip_v) return a.clip_v < b.clip_v;
            return a.clip_u < b.clip_u;
        });
        if (p->match_cache.size() >= 256) p->match_cache.erase(p->match_cache.begin());
        p->match_cache.push_back({*q,st != 0,status,parts});
        if (parts.size() > capacity) return HD_TEXTURE_LOOKUP_ERROR;
        std::copy(parts.begin(),parts.end(),output); *out_count = parts.size();
        return status;
    } catch (...) { return HD_TEXTURE_LOOKUP_ERROR; }
}
void duck_texture_pack_set_decode_budget(DuckTexturePack* p, size_t bytes) {
    if (!p) return;
    std::lock_guard<std::mutex> lock(p->worker.mutex);
    p->worker.budget = bytes; p->worker.evict(0);
}
int duck_texture_pack_request_decode(DuckTexturePack* p, uint64_t id) {
    if (!p || !id || id > p->entries.size() || p->entries[id-1].ambiguous) return HD_TEXTURE_LOOKUP_ERROR;
    try {
        Worker& w = p->worker; std::lock_guard<std::mutex> lock(w.mutex);
        auto it = w.decoded.find(id);
        if (it != w.decoded.end()) {
            it->second.use = ++w.tick;
            return it->second.status == DecodeStatus::Ready ? HD_TEXTURE_LOOKUP_FOUND :
                   it->second.status == DecodeStatus::Failed ? HD_TEXTURE_LOOKUP_ERROR : HD_TEXTURE_LOOKUP_NONE;
        }
        if (!w.budget) return HD_TEXTURE_LOOKUP_ERROR;
        if (w.decode_queue.size() >= kMaxDecodeQueue) return HD_TEXTURE_LOOKUP_NONE;
        if (w.decoded.size() >= 512) {
            auto victim = w.decoded.end();
            for (auto item = w.decoded.begin(); item != w.decoded.end(); ++item)
                if ((item->second.status == DecodeStatus::Ready || item->second.status == DecodeStatus::Failed) &&
                    (victim == w.decoded.end() || item->second.use < victim->second.use)) victim = item;
            if (victim == w.decoded.end()) return HD_TEXTURE_LOOKUP_NONE;
            if (victim->second.image) w.used -= victim->second.image->rgba.size();
            w.decoded.erase(victim);
        }
        if (!w.thread.joinable()) w.thread = std::thread(worker_main,&w);
        w.decoded[id].path = p->entries[id-1].path; w.decode_queue.push_back(id);
        w.wake.notify_one(); return HD_TEXTURE_LOOKUP_NONE;
    } catch (...) { return HD_TEXTURE_LOOKUP_ERROR; }
}
int duck_texture_pack_acquire_decoded(DuckTexturePack* p, uint64_t id, DuckTexturePixels* out) {
    if (out) *out = {};
    if (!p || !out) return HD_TEXTURE_LOOKUP_ERROR;
    try {
        Worker& w = p->worker; std::lock_guard<std::mutex> lock(w.mutex); const auto it = w.decoded.find(id);
        if (it == w.decoded.end()) return HD_TEXTURE_LOOKUP_NONE;
        if (it->second.status != DecodeStatus::Ready) return it->second.status == DecodeStatus::Failed ? HD_TEXTURE_LOOKUP_ERROR : HD_TEXTURE_LOOKUP_NONE;
        auto* lease = new Lease{it->second.image}; it->second.use = ++w.tick;
        out->lease = lease; out->rgba = lease->image->rgba.data(); out->width = lease->image->w;
        out->height = lease->image->h; out->stride = out->width * 4; return HD_TEXTURE_LOOKUP_FOUND;
    } catch (...) { return HD_TEXTURE_LOOKUP_ERROR; }
}
void duck_texture_pixels_release(DuckTexturePixels* p) { if (p) { delete static_cast<Lease*>(p->lease); *p = {}; } }

int duck_texture_pack_dump_draw(DuckTexturePack* p, const HdTextureDrawQuery* q, int st, char* error, size_t capacity) {
    if (!p || !q) return HD_TEXTURE_LOOKUP_ERROR;
    Rect rect{}; if (!query_rect(q,rect)) return HD_TEXTURE_LOOKUP_NONE;
    try {
        { std::lock_guard<std::mutex> lock(p->worker.mutex);
          if (!p->worker.dump_error.empty()) { error_text(error,capacity,p->worker.dump_error); return HD_TEXTURE_LOOKUP_ERROR; } }
        bool changed = false;
        for (const auto& upload : p->uploads) if (intersects(upload.active,rect)) upload.source->used = true;
        if (q->depth == 2 && !p->config.dump_c16_textures) return HD_TEXTURE_LOOKUP_NONE;
        if (!p->config.dump_texture_pages) {
            for (const auto& upload : p->uploads) {
                const Rect observed = intersection(upload.active,rect);
                if (observed.w && observed.h) changed |= record_usage(*p,*upload.source,observed,*q,st != 0);
            }
        } else {
            const Rect page{q->page_x,q->page_y,256/expansion(q->depth),256};
            auto existing = std::find_if(p->pages.begin(),p->pages.end(),[&](const PageSource& value) {
                return value.depth == q->depth && value.source->rect.x == page.x && value.source->rect.y == page.y &&
                    (q->depth == 2 || (value.clut_x == q->clut_x && value.clut_y == q->clut_y));
            });
            std::shared_ptr<Source> source;
            if (existing != p->pages.end()) source = existing->source;
            else {
                source = capture_source(*p,page,q->vram,DUCK_TEXTURE_PAGE);
                if (source) p->pages.push_back({source,q->depth,q->clut_x,q->clut_y});
            }
            if (source) changed = record_usage(*p,*source,rect,*q,st != 0);
        }
        if (!p->lifecycle_error.empty()) { error_text(error,capacity,p->lifecycle_error); return HD_TEXTURE_LOOKUP_ERROR; }
        /* Background atlases can remain resident for an entire level. Publish
         * observed originals while playing, rather than waiting indefinitely
         * for a native overwrite or for capture to be turned off. Retain the
         * observations so later draws enlarge the same palette crop. */
        if ((++p->dump_draw_tick & 255u) == 0 &&
            std::chrono::steady_clock::now() >= p->next_dump_checkpoint) {
            if (duck_texture_pack_checkpoint_dumps(p,error,capacity) < 0) return HD_TEXTURE_LOOKUP_ERROR;
            p->next_dump_checkpoint = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }
        return changed ? HD_TEXTURE_LOOKUP_FOUND : HD_TEXTURE_LOOKUP_NONE;
    } catch (const std::exception& e) { error_text(error,capacity,e.what()); return HD_TEXTURE_LOOKUP_ERROR; }
      catch (...) { error_text(error,capacity,"texture dump allocation failed"); return HD_TEXTURE_LOOKUP_ERROR; }
}
int duck_texture_pack_checkpoint_dumps(DuckTexturePack* p, char* error, size_t capacity) {
    if (!p) return HD_TEXTURE_LOOKUP_ERROR;
    try {
        bool success = true;
        for (const auto& source : p->sources)
            if (source->dump_dirty) success &= finalize_source(*p,*source,true);
        if (!success) { error_text(error,capacity,p->lifecycle_error); return HD_TEXTURE_LOOKUP_ERROR; }
        return HD_TEXTURE_LOOKUP_FOUND;
    } catch (const std::exception& e) { error_text(error,capacity,e.what()); return HD_TEXTURE_LOOKUP_ERROR; }
      catch (...) { error_text(error,capacity,"texture dump checkpoint allocation failed"); return HD_TEXTURE_LOOKUP_ERROR; }
}
int duck_texture_pack_flush_dumps(DuckTexturePack* p, char* error, size_t capacity) {
    if (!p) return HD_TEXTURE_LOOKUP_ERROR;
    try {
        bool success = true;
        for (const auto& source : p->sources) success &= finalize_source(*p,*source);
        Worker& worker = p->worker;
        std::unique_lock<std::mutex> lock(worker.mutex);
        worker.wake.wait(lock,[&] { return worker.dump_queue.empty() && !worker.dump_busy; });
        if (!worker.dump_error.empty()) { error_text(error,capacity,worker.dump_error); return HD_TEXTURE_LOOKUP_ERROR; }
        if (!success) { error_text(error,capacity,p->lifecycle_error); return HD_TEXTURE_LOOKUP_ERROR; }
        return HD_TEXTURE_LOOKUP_FOUND;
    } catch (const std::exception& e) { error_text(error,capacity,e.what()); return HD_TEXTURE_LOOKUP_ERROR; }
      catch (...) { error_text(error,capacity,"texture dump finalization allocation failed"); return HD_TEXTURE_LOOKUP_ERROR; }
}
void duck_texture_pack_reset_dump(DuckTexturePack* p) {
    if (!p) return;
    duck_texture_pack_flush_dumps(p,nullptr,0);
    p->dump_history = std::make_shared<DumpHistory>();
    p->dump_limit_reported = false;
    p->lifecycle_error.clear();
    std::lock_guard<std::mutex> lock(p->worker.mutex);
    p->worker.dump_error.clear();
}
} // extern C

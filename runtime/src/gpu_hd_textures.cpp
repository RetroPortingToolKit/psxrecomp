#include "gpu_hd_textures.h"
#include "gpu_gl_renderer.h"
#include "hd_texture_pack.h"
#include "duckstation_texture_pack.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {
constexpr size_t kVramWords = 1024u * 512u;
constexpr size_t kCompositionBudget = 64u * 1024u * 1024u;
/* A tiled scene can use dozens of small compositions per frame. The byte
 * budget bounds memory; a sixteen-entry limit thrashed below that budget. */
constexpr size_t kCompositionCount = 256;
constexpr uint32_t kCompositionDimension = 8192;
constexpr size_t kMaxParts = 64;
struct Composition {
    std::vector<uint64_t> signature;
    std::vector<uint8_t> rgba;
    uint32_t width = 0, height = 0;
    uint64_t cache_key = 0, last_use = 0;
    uint64_t ready_mask = 0;
};
struct Session {
    std::string root;
    HdTexturePack* beetle = nullptr;
    DuckTexturePack* duck = nullptr;
    bool replacements = false;
    bool dump = false;
    bool dump_failed = false;
    bool copy_pending = false;
    std::vector<std::shared_ptr<Composition>> compositions;
    size_t composition_bytes = 0;
    uint64_t composition_clock = 0;
    std::string dump_error;
    std::deque<std::string> diagnostics;
    /* Rectangles of uploads whose CRC keys a Beetle replacement, kept so a
     * restage can re-verify them against the restored VRAM (bounded). */
    std::vector<std::array<uint16_t, 4>> learned;
    GpuHdTextureDiag diag{};
    ~Session() { hd_texture_pack_destroy(beetle); duck_texture_pack_destroy(duck); }
};
struct Lease {
    HdTexturePixels beetle{};
    DuckTexturePixels duck{};
    std::shared_ptr<Composition> composition;
    ~Lease() { hd_texture_pixels_release(&beetle); duck_texture_pixels_release(&duck); }
};
std::unique_ptr<Session> session;
const uint16_t* native_vram = nullptr;
uint64_t generation = 1;
uint64_t composition_serial = 0;

struct DecodedParts {
    std::array<DuckTexturePixels, kMaxParts> pixels{};
    ~DecodedParts() { for (auto& pixel : pixels) duck_texture_pixels_release(&pixel); }
};

void fail(char* error, size_t capacity, const char* message) {
    if (error && capacity) std::snprintf(error, capacity, "%s", message);
}
bool is_beetle_root(const std::filesystem::path& root) {
    std::error_code error;
    if (std::filesystem::exists(root / "Hashes.ini", error)) return true;
    if (std::filesystem::exists(root.parent_path() / "Hashes.ini", error)) return true;
    const auto name = root.filename().u8string();
    const std::string suffix = "-texture-replacements";
    return name.size() >= suffix.size() &&
           name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}
int window_coord(int value, int mask, int offset) {
    return (value & ~(mask * 8)) | ((offset & mask) * 8);
}
bool make_query(uint16_t texpage, uint16_t cx, uint16_t cy, const int limits[4],
                uint32_t window, HdTextureDrawQuery& query) {
    if (!native_vram || !limits || ((texpage >> 7) & 3u) == 3u) return false;
    if (limits[0] < 0 || limits[1] < 0 || limits[2] > 255 || limits[3] > 255 ||
        limits[0] > limits[2] || limits[1] > limits[3]) return false;
    int first[2] = {255,255}, last[2] = {0,0};
    for (int axis = 0; axis < 2; ++axis) {
        int mask = (window >> (axis * 5)) & 31;
        int offset = (window >> (10 + axis * 5)) & 31;
        for (int p = limits[axis]; p <= limits[axis + 2]; ++p) {
            const int mapped = window_coord(p, mask, offset);
            first[axis] = std::min(first[axis], mapped);
            last[axis] = std::max(last[axis], mapped);
        }
    }
    query = {};
    query.page_x = (texpage & 15u) * 64u;
    query.page_y = ((texpage >> 4) & 1u) * 256u;
    query.depth = (texpage >> 7) & 3u;
    query.u_first = static_cast<uint8_t>(first[0]);
    query.u_last = static_cast<uint8_t>(last[0]);
    query.v_first = static_cast<uint8_t>(first[1]);
    query.v_last = static_cast<uint8_t>(last[1]);
    query.clut_x = cx; query.clut_y = cy;
    query.vram = native_vram; query.vram_word_count = kVramWords;
    return true;
}
uint8_t replacement_class(const uint8_t* pixel, bool semi) {
    if (!semi) return pixel[3] >= 128 ? 255 : 0;
    if (!(pixel[0] | pixel[1] | pixel[2] | pixel[3])) return 0;
    if (pixel[3] <= 242) return 128;
    return (pixel[0] | pixel[1] | pixel[2]) ? 255 : 0;
}
void replacement_pixel(const DuckTexturePixels& image, bool semi, bool linear,
                       double x, double y, uint8_t* output) {
    const int nx = std::clamp(static_cast<int>(std::floor(x)), 0, static_cast<int>(image.width) - 1);
    const int ny = std::clamp(static_cast<int>(std::floor(y)), 0, static_cast<int>(image.height) - 1);
    const uint8_t* nearest = image.rgba + static_cast<size_t>(ny) * image.stride + nx * 4;
    const uint8_t alpha = replacement_class(nearest, semi);
    output[3] = alpha;
    if (!alpha) { output[0] = output[1] = output[2] = 0; return; }
    if (!linear) { std::memcpy(output, nearest, 3); return; }
    /* Scaling filters colors within the nearest PSX class. Alpha class is
     * discrete: mixing cutout/STP/opaque would alter the primitive's blend. */
    x -= 0.5; y -= 0.5;
    const int bx = static_cast<int>(std::floor(x)), by = static_cast<int>(std::floor(y));
    const double fx = x - bx, fy = y - by;
    double sum[3]{}, weight = 0;
    for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) {
        const int px = std::clamp(bx + dx, 0, static_cast<int>(image.width) - 1);
        const int py = std::clamp(by + dy, 0, static_cast<int>(image.height) - 1);
        const uint8_t* pixel = image.rgba + static_cast<size_t>(py) * image.stride + px * 4;
        if (replacement_class(pixel, semi) != alpha) continue;
        const double w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
        for (int channel = 0; channel < 3; ++channel) sum[channel] += pixel[channel] * w;
        weight += w;
    }
    for (int channel = 0; channel < 3; ++channel)
        output[channel] = weight > 0 ? static_cast<uint8_t>(std::clamp(sum[channel] / weight + 0.5, 0.0, 255.0)) : nearest[channel];
}
void native_pixel(const HdTextureDrawQuery& query, const std::array<uint16_t,256>& palette,
                  int u, int v, uint8_t* output) {
    const unsigned per_word = 4u >> query.depth;
    uint16_t word = query.vram[(query.page_y + v) * 1024u + query.page_x + u / per_word];
    if (query.depth == HD_TEXTURE_DEPTH_4BPP) word = palette[(word >> ((u & 3) * 4)) & 15];
    else if (query.depth == HD_TEXTURE_DEPTH_8BPP) word = palette[(word >> ((u & 1) * 8)) & 255];
    output[0] = static_cast<uint8_t>(((word & 31u) * 255u + 15u) / 31u);
    output[1] = static_cast<uint8_t>((((word >> 5) & 31u) * 255u + 15u) / 31u);
    output[2] = static_cast<uint8_t>((((word >> 10) & 31u) * 255u + 15u) / 31u);
    output[3] = !word ? 0 : (word & 0x8000u) ? 128 : 255;
}
std::vector<uint64_t> composition_signature(const HdTextureDrawQuery& query,
    const std::array<DuckTextureMatch,kMaxParts>& matches, size_t count,
    std::array<uint16_t,256>& palette) {
    const unsigned per_word = 4u >> query.depth;
    const unsigned first_word = query.u_first / per_word, last_word = query.u_last / per_word;
    const uint32_t native_height = query.v_last - query.v_first + 1;
    const size_t palette_size = query.depth == HD_TEXTURE_DEPTH_4BPP ? 16 : query.depth == HD_TEXTURE_DEPTH_8BPP ? 256 : 0;
    for (size_t i = 0; i < palette_size; ++i)
        palette[i] = query.vram[(query.clut_y & 511u) * 1024u + ((query.clut_x + i) & 1023u)];
    std::vector<uint64_t> signature{
        static_cast<uint64_t>(query.page_x) | (static_cast<uint64_t>(query.page_y) << 16) | (static_cast<uint64_t>(query.depth) << 32),
        static_cast<uint64_t>(query.u_first) | (static_cast<uint64_t>(query.v_first) << 8) |
            (static_cast<uint64_t>(query.u_last) << 16) | (static_cast<uint64_t>(query.v_last) << 24),
        duck_texture_hash_rect(query.vram, query.vram_word_count, query.page_x + first_word,
            query.page_y + query.v_first, last_word - first_word + 1, native_height),
        duck_texture_hash_words_le(palette.data(), palette_size)};
    signature.push_back(duck_texture_pack_linear_filter(session->duck) != 0);
    for (size_t i = 0; i < count; ++i) {
        const auto& match = matches[i];
        signature.insert(signature.end(), {match.entry_id, static_cast<uint32_t>(match.origin_u),
            static_cast<uint32_t>(match.origin_v), match.source_width, match.source_height,
            static_cast<uint32_t>(match.clip_u), static_cast<uint32_t>(match.clip_v),
            match.clip_width, match.clip_height, match.key.semitransparent});
    }
    return signature;
}
std::shared_ptr<Composition> find_composition(const std::vector<uint64_t>& signature, uint64_t ready_mask) {
    for (auto& cached : session->compositions)
        if (cached->ready_mask == ready_mask && cached->signature == signature) {
            ++session->diag.composition_hits;
            cached->last_use = ++session->composition_clock; return cached;
        }
    return {};
}
std::shared_ptr<Composition> compose(const HdTextureDrawQuery& query,
    const std::array<DuckTextureMatch,kMaxParts>& matches, const DecodedParts& decoded, size_t count,
    std::vector<uint64_t> signature, const std::array<uint16_t,256>& palette) {
    const unsigned per_word = 4u >> query.depth;
    if (query.page_x + query.u_last / per_word >= 1024 || query.page_y + query.v_last >= 512) return {};
    const uint32_t native_width = query.u_last - query.u_first + 1;
    const uint32_t native_height = query.v_last - query.v_first + 1;
    const bool linear = duck_texture_pack_linear_filter(session->duck) != 0;
    double scale_x = 1, scale_y = 1;
    uint64_t ready_mask = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto& pixel = decoded.pixels[i];
        if (!pixel.rgba) continue;
        ready_mask |= UINT64_C(1) << i;
        scale_x = std::max(scale_x, static_cast<double>(pixel.width) / matches[i].source_width);
        scale_y = std::max(scale_y, static_cast<double>(pixel.height) / matches[i].source_height);
    }
    if (auto cached = find_composition(signature, ready_mask)) return cached;
    const double wanted_width = std::ceil(native_width * scale_x);
    const double wanted_height = std::ceil(native_height * scale_y);
    if (wanted_width > kCompositionDimension || wanted_height > kCompositionDimension) return {};
    const uint32_t width = static_cast<uint32_t>(wanted_width), height = static_cast<uint32_t>(wanted_height);
    const size_t bytes = static_cast<size_t>(width) * height * 4u;
    if (bytes > kCompositionBudget) return {};
    while (!session->compositions.empty() && (session->compositions.size() >= kCompositionCount ||
           session->composition_bytes + bytes > kCompositionBudget)) {
        auto oldest = std::min_element(session->compositions.begin(), session->compositions.end(),
            [](const auto& a, const auto& b) { return a->last_use < b->last_use; });
        session->composition_bytes -= (*oldest)->rgba.size(); session->compositions.erase(oldest);
    }
    auto result = std::make_shared<Composition>();
    ++session->diag.composition_builds;
    session->diag.composition_pixels += static_cast<uint64_t>(width) * height;
    result->signature = std::move(signature); result->width = width; result->height = height;
    result->ready_mask = ready_mask;
    result->rgba.resize(bytes);
    for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x)
        native_pixel(query, palette, query.u_first + (static_cast<uint64_t>(x) * 2 + 1) * native_width / (width * 2u),
            query.v_first + (static_cast<uint64_t>(y) * 2 + 1) * native_height / (height * 2u),
            result->rgba.data() + (static_cast<size_t>(y) * width + x) * 4);
    for (size_t i = 0; i < count; ++i) {
        const auto& image = decoded.pixels[i]; const auto& match = matches[i];
        if (!image.rgba) continue;
        const int x0 = std::clamp(static_cast<int>(std::floor((match.clip_u - query.u_first) * static_cast<double>(width) / native_width)), 0, static_cast<int>(width));
        const int y0 = std::clamp(static_cast<int>(std::floor((match.clip_v - query.v_first) * static_cast<double>(height) / native_height)), 0, static_cast<int>(height));
        const int x1 = std::clamp(static_cast<int>(std::ceil((match.clip_u + match.clip_width - query.u_first) * static_cast<double>(width) / native_width)), 0, static_cast<int>(width));
        const int y1 = std::clamp(static_cast<int>(std::ceil((match.clip_v + match.clip_height - query.v_first) * static_cast<double>(height) / native_height)), 0, static_cast<int>(height));
        for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
            const double u = query.u_first + (x + 0.5) * native_width / width;
            const double v = query.v_first + (y + 0.5) * native_height / height;
            if (u < match.clip_u || u >= match.clip_u + match.clip_width ||
                v < match.clip_v || v >= match.clip_v + match.clip_height) continue;
            replacement_pixel(image, match.key.semitransparent != 0, linear,
                (u - match.origin_u) * image.width / match.source_width,
                (v - match.origin_v) * image.height / match.source_height,
                result->rgba.data() + (static_cast<size_t>(y) * width + x) * 4);
        }
    }
    result->cache_key = (UINT64_C(1) << 63) | ++composition_serial;
    result->last_use = ++session->composition_clock;
    session->compositions.push_back(result); session->composition_bytes += bytes;
    return result;
}
bool dumps_suppressed = false;
void dump_query(const HdTextureDrawQuery& query, int semi) {
    if (!session->dump || session->dump_failed || dumps_suppressed) return;
    char error[256]{};
    const int status = duck_texture_pack_dump_draw(session->duck, &query, semi, error, sizeof(error));
    if (status == HD_TEXTURE_LOOKUP_FOUND) ++session->diag.dumped_textures;
    if (status < 0) {
        /* Keep the configured raster authority until a safe session boundary.
         * Tearing it down inside a pre-draw query would lose command order. */
        session->dump_failed = true;
        session->dump_error = error;
    }
}
}

extern "C" int gpu_hd_textures_configure(const char* root, int replacements,
                                          int dump, char* error, size_t capacity) {
    if (error && capacity) error[0] = 0;
    if ((!root || !root[0]) && !replacements && !dump) {
        gpu_hd_textures_shutdown(); return 1;
    }
    if (!root || !root[0]) { fail(error, capacity, "Select an external texture-pack directory."); return 0; }
    try {
        auto next = std::make_unique<Session>();
        next->root = root; next->replacements = replacements != 0; next->dump = dump != 0;
        const bool beetle = is_beetle_root(std::filesystem::u8path(root));
        if (beetle) {
            if (!hd_texture_pack_create(root, &next->beetle, error, capacity)) return 0;
        } else {
            char ignored[256]{};
            hd_texture_pack_create(root, &next->beetle, ignored, sizeof(ignored));
        }
        if (!next->beetle) {
            auto directory = std::filesystem::u8path(root);
            if (directory.filename().u8string() == "replacements") directory = directory.parent_path();
            std::filesystem::create_directories(directory / "replacements");
            std::filesystem::create_directories(directory / "dumps");
        }
        if (!duck_texture_pack_create(root, &next->duck, error, capacity)) return 0;
        /* Quiesce renderer users (GL render thread: dump_draw appends to the
         * old session's sources) before flushing its dumps or reading its
         * residency; the cache is rebuilt from the new session. */
        if (session) gl_renderer_clear_hd_texture_cache();
        if (session) {
            char dump_error[256]{};
            if (duck_texture_pack_flush_dumps(session->duck, dump_error, sizeof(dump_error)) < 0) {
                session->dump_error = dump_error;
                fail(error, capacity, dump_error); return 0;
            }
        }
        if (session && (session->replacements || session->dump) && session->root == next->root) {
            if (!duck_texture_pack_copy_tracking(next->duck, session->duck)) {
                fail(error, capacity, "Could not preserve texture-upload tracking during reload."); return 0;
            }
            if (session->beetle && next->beetle) {
                uint8_t* saved = nullptr; size_t saved_size = 0;
                if (!hd_texture_pack_tracking_state_save(session->beetle, &saved, &saved_size)) {
                    fail(error, capacity, "Could not save Beetle texture-upload tracking during reload."); return 0;
                }
                const int restored = hd_texture_pack_tracking_state_load(next->beetle, saved, saved_size);
                std::free(saved);
                if (!restored) { fail(error, capacity, "Could not restore Beetle texture-upload tracking during reload."); return 0; }
                /* Restage after rewind/load rehashes these bounded upload
                 * rectangles against restored VRAM. A same-pack reload must
                 * preserve that knowledge as well as current residency. */
                next->learned = session->learned;
            }
        }
        /* This also flushes all queued users before old replacement textures
         * and pixel leases may be destroyed. */
        gl_renderer_set_hd_texture_mode(next->replacements || next->dump);
        gl_renderer_clear_hd_texture_cache();
        session = std::move(next); ++generation;
        DuckTexturePackInfo info{};
        duck_texture_pack_get_info(session->duck, &info);
        size_t replacements_found = info.replacement_count;
        size_t ambiguous = info.ambiguous_count;
        if (session->beetle) {
            HdTexturePackInfo beetle_info{};
            hd_texture_pack_get_info(session->beetle, &beetle_info);
            replacements_found = beetle_info.unique_key_count;
            ambiguous = beetle_info.ambiguous_key_count;
        }
        std::fprintf(stdout, "psxrecomp: texture pack '%s': %zu replacements, %zu ambiguous, %zu ignored\n",
                     session->root.c_str(), replacements_found, ambiguous, info.ignored_count);
        if (info.diagnostic && info.diagnostic[0])
            std::fprintf(stderr, "psxrecomp: texture pack: %s\n", info.diagnostic);
        return 1;
    } catch (const std::exception& exception) {
        fail(error, capacity, exception.what()); return 0;
    }
}
extern "C" void gpu_hd_textures_shutdown(void) {
    gl_renderer_set_hd_texture_mode(0);
    gl_renderer_clear_hd_texture_cache();
    session.reset(); ++generation;
}
extern "C" int gpu_hd_textures_reload(char* error, size_t capacity) {
    if (!session) { fail(error, capacity, "No texture-pack session is active."); return 0; }
    const std::string root = session->root;
    return gpu_hd_textures_configure(root.c_str(), session->replacements, session->dump, error, capacity);
}
extern "C" void gpu_hd_textures_set_dump_enabled(int enabled) {
    if (!session || session->dump == (enabled != 0)) return;
    /* The render thread may be dumping (dump_draw) or tracking residency. */
    gl_renderer_render_thread_sync("hd_set_dump");
    if (enabled && !session->replacements) gpu_hd_textures_reset_tracking();
    if (!enabled) {
        char error[256]{};
        if (duck_texture_pack_flush_dumps(session->duck, error, sizeof(error)) < 0) {
            session->dump_error = error; session->dump_failed = true; return;
        }
    }
    gl_renderer_set_hd_texture_mode(session->replacements || enabled);
    session->dump = enabled != 0;
    session->dump_failed = false;
}
extern "C" int gpu_hd_textures_replacements_enabled(void) { return session && session->replacements; }
extern "C" int gpu_hd_textures_dump_enabled(void) { return session && session->dump; }
extern "C" int gpu_hd_textures_active(void) { return session && (session->replacements || session->dump); }
extern "C" void gpu_hd_textures_get_diag(GpuHdTextureDiag* out) {
    if (!out) return;
    *out = {};
    if (!session) { out->root = ""; out->diagnostic = ""; return; }
    /* Counters and dump queues are written by the GL render thread. */
    gl_renderer_render_thread_sync("hd_diag");
    *out = session->diag;
    out->root = session->root.c_str(); out->active = gpu_hd_textures_active();
    out->replacements = session->replacements; out->dump = session->dump && !session->dump_failed;
    out->format = session->beetle ? 1 : 2;
    DuckTexturePackInfo info{}; duck_texture_pack_get_info(session->duck, &info);
    out->decoded_images = info.decoded_images;
    out->decode_evictions = info.decode_evictions;
    out->decoded_bytes = info.decoded_bytes;
    out->dumped_textures = info.queued_dump_count; out->pending_dump_sources = info.pending_dump_sources;
    if (session->beetle) { HdTexturePackInfo beetle_info{}; hd_texture_pack_get_info(session->beetle, &beetle_info); out->replacement_count = beetle_info.unique_key_count; }
    else out->replacement_count = info.replacement_count;
    std::string diagnostic = info.diagnostic ? info.diagnostic : "";
    if (!session->dump_error.empty()) {
        if (!diagnostic.empty()) diagnostic += "; ";
        diagnostic += session->dump_error;
    }
    if (session->diagnostics.empty() || session->diagnostics.back() != diagnostic)
        session->diagnostics.push_back(std::move(diagnostic));
    out->diagnostic = session->diagnostics.back().c_str();
}
extern "C" void gpu_hd_textures_note_applied(void) { if (session) ++session->diag.applied_draws; }
extern "C" void gpu_hd_textures_note_gl_cache(int eviction) {
    if (session) {
        if (eviction) ++session->diag.gl_evictions;
        else ++session->diag.gl_uploads;
    }
}
extern "C" void gpu_hd_textures_bind_vram(const uint16_t* vram) { native_vram = vram; }
extern "C" void gpu_hd_textures_suppress_dumps(int on) { dumps_suppressed = on != 0; }
extern "C" void gpu_hd_textures_set_vram(const uint16_t* vram) { native_vram = vram; gpu_hd_textures_reset_tracking(); }
extern "C" void gpu_hd_textures_reset_tracking(void) {
    if (!session) return;
    gl_renderer_clear_hd_texture_cache();
    session->compositions.clear(); session->composition_bytes = 0;
    session->copy_pending = false;
    hd_texture_pack_reset_tracking(session->beetle);
    duck_texture_pack_reset_tracking(session->duck);
}
extern "C" void gpu_hd_textures_begin_upload(int x, int y, int width, int height) {
    /* The GP0 header arrives before native payload writes. Complete uploads
     * establish their fresh provenance only in track_upload(), after writing. */
    gpu_hd_textures_invalidate(x, y, width, height);
}
extern "C" void gpu_hd_textures_begin_copy(int sx, int sy, int dx, int dy, int width, int height) {
    if (!session || !native_vram) return;
    session->copy_pending = false;
    if (width < 1 || width > 1024 || height < 1 || height > 512) { gpu_hd_textures_reset_tracking(); return; }
    hd_texture_pack_invalidate(session->beetle, dx & 1023, dy & 511, width, height);
    session->copy_pending = duck_texture_pack_begin_copy(session->duck, sx & 1023, sy & 511,
        dx & 1023, dy & 511, width, height, native_vram, kVramWords) > 0;
    if (!session->copy_pending)
        duck_texture_pack_invalidate(session->duck, dx & 1023, dy & 511, width, height);
}
extern "C" void gpu_hd_textures_end_copy(void) {
    if (!session || !session->copy_pending) return;
    session->copy_pending = false;
    if (duck_texture_pack_end_copy(session->duck, native_vram, kVramWords) < 0)
        gpu_hd_textures_reset_tracking();
}
namespace {
constexpr uint32_t kResidencyMagic = 0x52444850u; /* "PHDR" */
constexpr uint32_t kResidencyVersion = 2u;   /* 2: + the pack's identity */
constexpr size_t kResidencyHeader = 16u;
void put32(uint8_t* at, uint32_t v) { for (int i = 0; i < 4; ++i) at[i] = uint8_t(v >> (8 * i)); }
uint32_t get32(const uint8_t* at) { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t(at[i]) << (8 * i); return v; }
}
extern "C" int gpu_hd_textures_residency_save(uint8_t** data, size_t* size) {
    if (data) *data = nullptr;
    if (size) *size = 0;
    if (!data || !size || !session || !session->beetle || !native_vram) return 0;
    uint8_t* tracking = nullptr; size_t tracking_size = 0;
    if (!hd_texture_pack_tracking_state_save(session->beetle, &tracking, &tracking_size)) return 0;
    uint8_t* out = static_cast<uint8_t*>(std::malloc(kResidencyHeader + tracking_size));
    if (!out) { std::free(tracking); return 0; }
    put32(out, kResidencyMagic); put32(out + 4, kResidencyVersion);
    put32(out + 8, hd_texture_crc32_words_le(native_vram, kVramWords));
    put32(out + 12, hd_texture_pack_identity(session->beetle));
    if (tracking_size) std::memcpy(out + kResidencyHeader, tracking, tracking_size);
    std::free(tracking);
    *data = out; *size = kResidencyHeader + tracking_size;
    return 1;
}
extern "C" int gpu_hd_textures_residency_load(const uint8_t* data, size_t size) {
    if (!data || size < kResidencyHeader || !session || !session->beetle || !native_vram) return 0;
    if (get32(data) != kResidencyMagic || get32(data + 4) != kResidencyVersion) return 0;
    /* Saved with another pack: its residency names other uploads. */
    if (get32(data + 12) != hd_texture_pack_identity(session->beetle)) return 0;
    if (get32(data + 8) != hd_texture_crc32_words_le(native_vram, kVramWords)) return 0;
    if (!hd_texture_pack_tracking_state_check(data + kResidencyHeader, size - kResidencyHeader)) return 0;
    return hd_texture_pack_tracking_state_load(session->beetle, data + kResidencyHeader,
                                               size - kResidencyHeader);
}
extern "C" void gpu_hd_textures_invalidate(int x, int y, int width, int height) {
    if (!session || width < 1 || height < 1) return;
    if (width > 1024 || height > 512) { gpu_hd_textures_reset_tracking(); return; }
    hd_texture_pack_invalidate(session->beetle, x & 1023, y & 511, width, height);
    duck_texture_pack_invalidate(session->duck, x & 1023, y & 511, width, height);
}
extern "C" void gpu_hd_textures_track_upload(int x, int y, int width, int height,
                                             const uint16_t* words) {
    (void)words;
    if (!session || !native_vram || width < 1 || width > 1024 || height < 1 || height > 512) return;
    if (width == 1024 && height == 512) { gpu_hd_textures_reset_tracking(); return; }
    /* gpu.c's A0 staging already contains post-mask words. Read the canonical
     * mirror here also for host-initiated transfers which use raw input. */
    const size_t count = static_cast<size_t>(width) * height;
    std::unique_ptr<uint16_t[]> applied(new (std::nothrow) uint16_t[count]);
    if (!applied) { gpu_hd_textures_reset_tracking(); return; }
    for (int row = 0; row < height; ++row)
        for (int col = 0; col < width; ++col)
            applied[static_cast<size_t>(row) * width + col] =
                native_vram[((y + row) & 511) * 1024 + ((x + col) & 1023)];
    uint32_t hash = 0;
    hd_texture_pack_track_upload(session->beetle, x & 1023, y & 511, width, height,
                                 applied.get(), count, &hash);
    duck_texture_pack_track_upload(session->duck, x & 1023, y & 511, width, height,
                                   native_vram, kVramWords);
    if (session->beetle && hd_texture_pack_has_texture(session->beetle, hash)) {
        const std::array<uint16_t, 4> rect{uint16_t(x & 1023), uint16_t(y & 511),
                                           uint16_t(width), uint16_t(height)};
        if (std::find(session->learned.begin(), session->learned.end(), rect) == session->learned.end()) {
            /* Bounded by count and by area: a restage re-hashes at most one
             * VRAM's worth of words (~1 MiB of CRC), however large or many
             * the learned uploads (oldest dropped first). */
            session->learned.push_back(rect);
            size_t area = 0;
            for (const auto& r : session->learned) area += size_t(r[2]) * r[3];
            while (session->learned.size() > 1 &&
                   (session->learned.size() > 256 || area > kVramWords)) {
                const auto& r = session->learned.front();
                area -= size_t(r[2]) * r[3];
                session->learned.erase(session->learned.begin());
            }
        }
    }
}
extern "C" void gpu_hd_textures_restage(void) {
    gpu_hd_textures_reset_tracking();
    if (!session || !session->beetle || !native_vram) return;
    /* Only a rectangle whose restored words still hash to a replacement key
     * becomes resident again: the same words at the same place as an upload
     * this session saw, so no identity is invented. */
    std::vector<uint16_t> words;
    for (const auto& r : session->learned) {
        const size_t count = size_t(r[2]) * r[3];
        words.resize(count);
        for (unsigned row = 0; row < r[3]; ++row)
            for (unsigned col = 0; col < r[2]; ++col)
                words[size_t(row) * r[2] + col] = native_vram[((r[1] + row) & 511) * 1024 + ((r[0] + col) & 1023)];
        if (hd_texture_pack_has_texture(session->beetle, hd_texture_crc32_words_le(words.data(), count)))
            hd_texture_pack_track_upload(session->beetle, r[0], r[1], r[2], r[3], words.data(), count, nullptr);
    }
}
static int acquire_draw(uint16_t tp, uint16_t cx, uint16_t cy,
                                            const int limits[4], uint32_t window,
                                            int semi, GpuHdTextureImage* image, bool allow_gl_cache) {
    if (!image) return 0;
    *image = {};
    if (!session) return 0;
    ++session->diag.draw_queries;
    HdTextureDrawQuery query{};
    if (!make_query(tp, cx, cy, limits, window, query)) return 0;
    dump_query(query, semi);
    if (!session->replacements) return 0;
    auto lease = std::unique_ptr<Lease>(new (std::nothrow) Lease);
    if (!lease) return 0;
    if (session->beetle) {
        HdTextureMatch match{};
        if (hd_texture_pack_match(session->beetle, &query, &match) != HD_TEXTURE_LOOKUP_FOUND) return 0;
        ++session->diag.matched_draws;
        hd_texture_pack_request_decode(session->beetle, match.entry.texture_hash, match.entry.palette_hash);
        if (hd_texture_pack_acquire_decoded(session->beetle, match.entry.texture_hash,
                                          match.entry.palette_hash, &lease->beetle) != HD_TEXTURE_LOOKUP_FOUND) return 0;
        const unsigned pixels_per_word = 4u >> query.depth;
        image->rgba = lease->beetle.rgba; image->width = lease->beetle.width;
        image->height = lease->beetle.height; image->stride = lease->beetle.stride;
        image->source_width = match.upload_width_words * pixels_per_word;
        image->source_height = match.upload_height;
        image->origin_u = query.u_first - (match.source_word_x * pixels_per_word + (query.u_first % pixels_per_word));
        image->origin_v = query.v_first - match.source_y;
        image->cache_key = (static_cast<uint64_t>(match.entry.texture_hash) << 32) | match.entry.palette_hash;
        image->alpha_mode = 1;
    } else {
        try {
            std::array<DuckTextureMatch,kMaxParts> matches{}; size_t count = 0;
            if (duck_texture_pack_match_parts(session->duck, &query, semi, matches.data(), matches.size(), &count) != HD_TEXTURE_LOOKUP_FOUND || !count) return 0;
            ++session->diag.matched_draws;
            const auto& match = matches[0];
            if (count == 1 && match.clip_u <= query.u_first && match.clip_v <= query.v_first &&
                match.clip_u + match.clip_width > query.u_last && match.clip_v + match.clip_height > query.v_last) {
                /* The GPU copy outlives the decoder's smaller CPU LRU. A draw
                 * using that copy must not queue another PNG decode. */
                if (allow_gl_cache && gl_renderer_hd_texture_cached(match.entry_id, generation,
                        &image->width, &image->height)) {
                    image->stride = image->width * 4u;
                } else {
                    duck_texture_pack_request_decode(session->duck, match.entry_id);
                    if (duck_texture_pack_acquire_decoded(session->duck, match.entry_id, &lease->duck) != HD_TEXTURE_LOOKUP_FOUND) return 0;
                    image->rgba = lease->duck.rgba; image->width = lease->duck.width;
                    image->height = lease->duck.height; image->stride = lease->duck.stride;
                }
                image->source_width = match.source_width; image->source_height = match.source_height;
                image->origin_u = match.origin_u; image->origin_v = match.origin_v;
                image->cache_key = match.entry_id;
                image->alpha_mode = match.key.semitransparent ? 3 : 2;
            } else {
                std::array<uint16_t,256> palette{};
                auto signature = composition_signature(query, matches, count, palette);
                const uint64_t full_mask = count == 64 ? UINT64_MAX : (UINT64_C(1) << count) - 1;
                /* An immutable, complete composition already owns all source
                 * pixels. Reuse it even after those decoded PNGs are evicted.
                 * Partial compositions still check for newly ready parts. */
                lease->composition = find_composition(signature, full_mask);
                if (!lease->composition) {
                    DecodedParts decoded; size_t ready = 0;
                    for (size_t i = 0; i < count; ++i) {
                        duck_texture_pack_request_decode(session->duck, matches[i].entry_id);
                        if (duck_texture_pack_acquire_decoded(session->duck, matches[i].entry_id, &decoded.pixels[i]) == HD_TEXTURE_LOOKUP_FOUND) ++ready;
                    }
                    if (!ready) return 0;
                    lease->composition = compose(query, matches, decoded, count, std::move(signature), palette);
                }
                if (!lease->composition) return 0;
                const auto& composed = *lease->composition;
                image->rgba = composed.rgba.data(); image->width = composed.width;
                image->height = composed.height; image->stride = composed.width * 4u;
                image->source_width = query.u_last - query.u_first + 1;
                image->source_height = query.v_last - query.v_first + 1;
                image->origin_u = query.u_first; image->origin_v = query.v_first;
                image->cache_key = composed.cache_key; image->alpha_mode = 4;
            }
        } catch (const std::exception&) { return 0; }
    }
    image->generation = generation; image->lease = lease.release();
    ++session->diag.ready_draws;
    return 1;
}
extern "C" int gpu_hd_textures_acquire_draw(uint16_t tp, uint16_t cx, uint16_t cy,
    const int limits[4], uint32_t window, int semi, GpuHdTextureImage* image) {
    return acquire_draw(tp, cx, cy, limits, window, semi, image, false);
}
extern "C" int gpu_hd_textures_acquire_gl_draw(uint16_t tp, uint16_t cx, uint16_t cy,
    const int limits[4], uint32_t window, int semi, GpuHdTextureImage* image) {
    return acquire_draw(tp, cx, cy, limits, window, semi, image, true);
}
extern "C" void gpu_hd_textures_release_image(GpuHdTextureImage* image) {
    if (!image) return;
    delete static_cast<Lease*>(image->lease); *image = {};
}
extern "C" void gpu_hd_textures_observe_draw(uint16_t tp, uint16_t cx, uint16_t cy,
    const int limits[4], uint32_t window, int semi) {
    if (!session || !session->dump || session->dump_failed) return;
    HdTextureDrawQuery query{};
    if (make_query(tp, cx, cy, limits, window, query)) {
        ++session->diag.draw_queries; dump_query(query, semi);
    }
}

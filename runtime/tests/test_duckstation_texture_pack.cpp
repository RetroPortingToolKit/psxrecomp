#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"
#include "duckstation_texture_pack.h"
#include "../src/png_write.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr,"FAIL: %s\n",what); ++failures; }
}
using Pack = std::unique_ptr<DuckTexturePack,decltype(&duck_texture_pack_destroy)>;
Pack create(const fs::path& root) {
    DuckTexturePack* pack = nullptr; char error[512]{};
    check(duck_texture_pack_create(root.u8string().c_str(),&pack,error,sizeof(error)) == 1,error);
    return Pack(pack,&duck_texture_pack_destroy);
}
std::vector<uint16_t> words(size_t n) {
    std::vector<uint16_t> result(n);
    for (size_t i = 0; i < n; ++i) result[i] = static_cast<uint16_t>((i*7919)^0xa51c);
    return result;
}
void place(std::vector<uint16_t>& vram, unsigned x, unsigned y, unsigned w,
           const std::vector<uint16_t>& source) {
    for (size_t i = 0; i < source.size(); ++i) vram[(y+i/w)*1024+x+i%w] = source[i];
}
HdTextureDrawQuery query(std::vector<uint16_t>& vram, uint16_t page_x, uint16_t page_y,
                        uint8_t depth, uint8_t u0, uint8_t u1, uint8_t v0, uint8_t v1,
                        uint16_t clut_x = 512, uint16_t clut_y = 400) {
    return {page_x,page_y,depth,u0,u1,v0,v1,clut_x,clut_y,vram.data(),vram.size()};
}
void png(const fs::path& path, unsigned w = 4, unsigned h = 4) {
    fs::create_directories(path.parent_path());
    std::vector<uint8_t> pixels(size_t(w)*h*4);
    constexpr uint8_t alpha[] = {0,127,128,242,243,255};
    for (size_t i = 0; i < size_t(w)*h; ++i) {
        pixels[i*4] = 23; pixels[i*4+1] = 57; pixels[i*4+2] = 91; pixels[i*4+3] = alpha[i%6];
    }
#ifdef _WIN32
    FILE* file = _wfopen(path.c_str(),L"wb");
#else
    FILE* file = std::fopen(path.c_str(),"wb");
#endif
    check(file != nullptr,"open original test PNG");
    if (file) { check(png_write_rgba(file,pixels.data(),w,h) == 1,"encode original test PNG"); std::fclose(file); }
}
void config(const fs::path& root, const char* contents) {
    fs::create_directories(root);
    std::ofstream file(root/"config.yaml"); file << contents;
}
std::vector<fs::path> dumps(const fs::path& root) {
    std::vector<fs::path> result;
    if (fs::exists(root/"dumps")) for (const auto& file : fs::directory_iterator(root/"dumps"))
        if (file.path().extension() == ".png") result.push_back(file.path());
    std::sort(result.begin(),result.end()); return result;
}
std::vector<uint8_t> read_png(const fs::path& file, unsigned w, unsigned h) {
    std::ifstream input(file,std::ios::binary);
    std::vector<uint8_t> encoded((std::istreambuf_iterator<char>(input)),{}), result;
    int width = 0, height = 0, channels = 0;
    auto* pixels = encoded.empty() ? nullptr : stbi_load_from_memory(encoded.data(),static_cast<int>(encoded.size()),&width,&height,&channels,4);
    check(pixels && width == int(w) && height == int(h),"dump PNG has expected dimensions");
    if (pixels) { result.assign(pixels,pixels+size_t(width)*height*4); stbi_image_free(pixels); }
    return result;
}
void flush(DuckTexturePack* pack) {
    char error[512]{};
    check(duck_texture_pack_flush_dumps(pack,error,sizeof(error)) == HD_TEXTURE_LOOKUP_FOUND,error);
}
std::string filename(const DuckTextureKey& key) {
    char name[192]{}; check(duck_texture_format_name(&key,name,sizeof(name)),"format fixture name");
    return std::string(name)+".png";
}
DuckTexturePixels decoded(DuckTexturePack* pack, uint64_t id) {
    DuckTexturePixels pixels{};
    check(duck_texture_pack_request_decode(pack,id) >= 0,"queue PNG decode");
    const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(5);
    int status = HD_TEXTURE_LOOKUP_NONE;
    do {
        status = duck_texture_pack_acquire_decoded(pack,id,&pixels);
        if (status != HD_TEXTURE_LOOKUP_NONE) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < end);
    check(status == HD_TEXTURE_LOOKUP_FOUND,"background PNG decode becomes ready");
    return pixels;
}
bool decode_fails(DuckTexturePack* pack, uint64_t id) {
    int status = duck_texture_pack_request_decode(pack,id);
    const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (status == HD_TEXTURE_LOOKUP_NONE && std::chrono::steady_clock::now() < end) {
        DuckTexturePixels pixels{};
        status = duck_texture_pack_acquire_decoded(pack,id,&pixels);
        duck_texture_pixels_release(&pixels);
        if (status == HD_TEXTURE_LOOKUP_NONE) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return status == HD_TEXTURE_LOOKUP_ERROR;
}

void fixed_hash_vectors() {
    /* Independently produced by Python xxhash 3.6.0's native wheel. These
     * literal expectations cover XXH3's short/medium/long-input boundaries. */
    const std::pair<size_t,uint64_t> vectors[] = {
        {0,0x2d06800538d394c2ULL},{1,0xc5779b2f56fbe2e1ULL},{2,0x0edb68960f55aef6ULL},
        {3,0xfce9d54aec3f9d32ULL},{4,0x3631d2a04ec85311ULL},{8,0x000ca76cb9b329dcULL},
        {9,0xab6a9091cf532d8aULL},{16,0x9cbcdea9305d7528ULL},{17,0x2f133c3e193cbe48ULL},
        {32,0x3e4a087da5cd5990ULL},{64,0xc3e3ef9ad7f08dacULL},{65,0x740c4e7d1690d606ULL},
        {120,0x5942d561231419b2ULL},{121,0x152cb5857c4535f3ULL},{128,0x351cd56a8fe50e66ULL},
        {129,0x38a45de7ae84906eULL},{256,0x0005d0b83a005f4eULL},{8192,0xc68a00dcca652920ULL}
    };
    for (const auto& v : vectors) {
        const auto input = words(v.first);
        check(duck_texture_hash_words_le(input.data(),input.size()) == v.second,"fixed independent LE16 XXH3 vector");
    }
    std::vector<uint16_t> vram(1024*512,0xf00d);
    place(vram,101,39,2,words(8));
    check(duck_texture_hash_rect(vram.data(),vram.size(),101,39,2,4) == 0x000ca76cb9b329dcULL,
          "strided native rectangle excludes unrelated VRAM words");
    check(duck_texture_hash_rect(vram.data(),vram.size(),1023,0,2,1) == 0,"wrapped hash rectangle rejected");
}

void names() {
    DuckTextureKey key{};
    check(duck_texture_parse_name("texupload-P4-0123456789abcdef-FEDCBA9876543210-64x256-0-192-64x64-P0-14.png",&key) == 1,
          "literal upstream documented upload filename");
    check(key.source_hash == 0x0123456789abcdefULL && key.palette_hash == 0xfedcba9876543210ULL &&
          key.source_width_words == 64 && key.offset_y == 192 && key.width == 64 && key.palette_max == 14,
          "filename dimensions distinguish source words from expanded texels");
    check(duck_texture_parse_name("texpage-STC16-0123456789ABCDEF-256x256-8-16-32x64.PNG",&key) == 1 &&
          key.kind == DUCK_TEXTURE_PAGE && key.semitransparent && key.depth == 2 && !key.palette_hash,
          "direct ST filename omits palette fields");
    for (const char* extension : {"jpg","jpeg","webp"})
        check(duck_texture_parse_name((std::string("texupload-C16-0123456789ABCDEF-4x2-0-0-4x2.")+extension).c_str(),&key),
              "JPEG and WebP canonical payload extensions accepted");
    const char* invalid[] = {
        "texpage-P4-12345678-0123456789ABCDEF-64x256-0-0-4x1-P0-15.png",
        "texpage-P4-0123456789ABCDEF-0123456789ABCDEF-64x256-0-0-3x1-P0-15.png",
        "texpage-P4-0123456789ABCDEF-0123456789ABCDEF-64x256-0-0-4x1-P0-16.png",
        "texpage-P8-0123456789ABCDEF-0123456789ABCDEF-128x256-0-0-4x1-P15-0.png",
        "texupload-C16-0123456789ABCDEF-0x256-0-0-4x1.png",
        "texupload-C16-0123456789ABCDEF-65536x1-0-0-4x1.png",
        "texpage-C16-0123456789ABCDEF-256x256-255-0-2x1.png",
        "texpage-C16-0123456789ABCDEF-256x256-0-0-4x1.png.old",
        "../texpage-C16-0123456789ABCDEF-256x256-0-0-4x1.png"
    };
    for (const char* name : invalid) check(!duck_texture_parse_name(name,&key),"targeted malformed/unsupported name rejected");
}

void page_and_decode(const fs::path& root) {
    const char* name = "texpage-P4-3631D2A04EC85311-9CBCDEA9305D7528-64x256-4-2-8x2-P0-15.png";
    png(root/"replacements"/"nested"/name,16,4);
    std::vector<uint16_t> vram(1024*512);
    place(vram,65,258,2,words(4)); place(vram,512,400,16,words(16));
    auto pack = create(root); auto q = query(vram,64,256,0,5,10,2,3);
    DuckTextureMatch match{};
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_FOUND,"fixed subpage fixture matches");
    check(match.origin_u == 4 && match.origin_v == 2 && match.source_width == 8 && match.source_height == 2,
          "crop extent and origin are page-space texels");
    auto pixels = decoded(pack.get(),match.entry_id);
    check(pixels.width == 16 && pixels.height == 4,"arbitrary PNG scaling uses image dimensions");
    constexpr uint8_t alpha[] = {0,127,128,242,243,255};
    if (pixels.rgba) for (unsigned i = 0; i < 6; ++i)
        check(pixels.rgba[i*4+3] == alpha[i],"decode retains raw alpha boundary bytes");
    const uint16_t old = vram[258*1024+65];
    vram[258*1024+65] ^= 1; duck_texture_pack_invalidate(pack.get(),65,258,1,1);
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_NONE,"write invalidates positive page cache");
    vram[258*1024+65] = old; duck_texture_pack_invalidate(pack.get(),65,258,1,1);
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_FOUND,"write invalidates negative page cache");
    vram[400*1024+512] ^= 1; duck_texture_pack_invalidate(pack.get(),512,400,1,1);
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_NONE,"CLUT write invalidates cached match");
    pack.reset();
    check(pixels.rgba && pixels.rgba[0] == 23,"pixel lease outlives pack and decode worker");
    duck_texture_pixels_release(&pixels);
}

void upload(const fs::path& root) {
    png(root/"replacements"/"texupload-C16-000CA76CB9B329DC-4x2-2-0-2x2.png",4,8);
    std::vector<uint16_t> vram(1024*512); place(vram,66,257,4,words(8));
    auto pack = create(root); auto q = query(vram,64,256,2,4,5,1,2);
    DuckTextureMatch match{};
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_NONE,"upload names require original source identity");
    check(duck_texture_pack_track_upload(pack.get(),66,257,4,2,vram.data(),vram.size()),"track post-write native upload");
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_FOUND && match.origin_u == 4 && match.origin_v == 1,
          "upload crop has correct source origin");
    duck_texture_pack_invalidate(pack.get(),66,257,1,1); /* outside replacement crop */
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_NONE,"partial source overwrite drops entire upload identity");
    duck_texture_pack_track_upload(pack.get(),66,257,4,2,vram.data(),vram.size());
    duck_texture_pack_reset_tracking(pack.get());
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_NONE,"savestate/reset clears upload identity");
    check(!duck_texture_pack_track_upload(pack.get(),1023,0,2,1,vram.data(),vram.size()),"wrapped upload explicitly falls back");
}

void reload_tracking(const fs::path& root) {
    std::vector<uint16_t> vram(1024*512); place(vram,66,257,4,words(8));
    auto old = create(root);
    check(duck_texture_pack_track_upload(old.get(),66,257,4,2,vram.data(),vram.size()),"capture upload before replacement file exists");
    png(root/"replacements"/"texupload-C16-000CA76CB9B329DC-4x2-2-0-2x2.png");
    auto refreshed = create(root); auto q = query(vram,64,256,2,4,5,1,2); DuckTextureMatch match{};
    check(duck_texture_pack_match(refreshed.get(),&q,&match) == HD_TEXTURE_LOOKUP_NONE,"newly indexed replacement initially has no upload identity");
    check(duck_texture_pack_copy_tracking(refreshed.get(),old.get()),"same-session reload copies upload tracking");
    check(duck_texture_pack_match(refreshed.get(),&q,&match) == HD_TEXTURE_LOOKUP_FOUND,"copied tracking makes newly indexed replacement immediately match");
    duck_texture_pack_invalidate(refreshed.get(),66,257,1,1);
    check(duck_texture_pack_match(refreshed.get(),&q,&match) == HD_TEXTURE_LOOKUP_NONE,"destination tracking invalidates independently");
    auto second = create(root); duck_texture_pack_copy_tracking(second.get(),old.get());
    check(duck_texture_pack_match(second.get(),&q,&match) == HD_TEXTURE_LOOKUP_FOUND,"destination invalidation leaves original tracker intact");
}

void palettes(const fs::path& root) {
    std::vector<uint16_t> vram(1024*512); place(vram,128,0,1,words(2));
    place(vram,1008,400,16,words(16));
    const char* edge = "texpage-P8-0EDB68960F55AEF6-9CBCDEA9305D7528-128x256-0-0-2x2-P0-255.png";
    png(root/"edge"/"replacements"/edge);
    auto pack = create(root/"edge"); auto q = query(vram,128,0,1,0,1,0,1,1008,400); DuckTextureMatch match{};
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_FOUND,"full P8 edge CLUT hashes remaining 16 words");
    vram[400*1024] = 42; duck_texture_pack_invalidate(pack.get(),0,400,1,1);
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_FOUND,"full P8 CLUT hash does not wrap into row start");
    const char* partial = "texpage-P8-0EDB68960F55AEF6-3631D2A04EC85311-128x256-0-0-2x2-P8-11.png";
    png(root/"partial"/"replacements"/partial);
    place(vram,512,400,16,words(16)); q.clut_x = 512;
    pack = create(root/"partial");
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_FOUND,"reduced P8 min8-max11 hashes prefix4 per upstream pin");
    vram[400*1024+520] ^= 1; duck_texture_pack_invalidate(pack.get(),520,400,1,1);
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_FOUND,"pinned prefix quirk ignores palette8 despite declared minimum8");
    vram[400*1024+512] ^= 1; duck_texture_pack_invalidate(pack.get(),512,400,1,1);
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_NONE,"reduced P8 prefix change stops replacement");
    q.clut_x = 1023;
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_NONE,"misaligned/edge reduced CLUT rejected");
}

void st_alias_duplicates(const fs::path& root) {
    const char* ordinary = "texpage-C16-000CA76CB9B329DC-256x256-0-0-4x2.png";
    const char* st = "texpage-STC16-000CA76CB9B329DC-256x256-0-0-4x2.png";
    png(root/"paired"/"replacements"/ordinary); png(root/"paired"/"replacements"/st);
    std::vector<uint16_t> vram(1024*512); place(vram,64,0,4,words(8)); auto q = query(vram,64,0,2,0,3,0,1);
    auto pack = create(root/"paired"); DuckTextureMatch match{};
    check(duck_texture_pack_match_draw(pack.get(),&q,0,&match) == HD_TEXTURE_LOOKUP_FOUND && !match.key.semitransparent,
          "ordinary primitive prefers ordinary filename");
    check(duck_texture_pack_match_draw(pack.get(),&q,1,&match) == HD_TEXTURE_LOOKUP_FOUND && match.key.semitransparent,
          "semitransparent primitive prefers ST filename");
    png(root/"alias"/"replacements"/"art"/"sample.png");
    fs::create_directories(root/"alias");
    std::ofstream config(root/"alias"/"config.yaml");
    config << "MaxVRAMWriteCoalesceWidth: 1\nBogusOption: true\nAliases:\n  " << std::string(ordinary).substr(0,std::strlen(ordinary)-4)
           << ": |\n    art/sample.png\n"; config.close();
    pack = create(root/"alias");
    check(duck_texture_pack_match_draw(pack.get(),&q,1,&match) == HD_TEXTURE_LOOKUP_FOUND && !match.key.semitransparent,
          "bounded literal alias and opposite-convention fallback");
    DuckTexturePackInfo info{}; duck_texture_pack_get_info(pack.get(),&info);
    DuckTextureConfig settings{}; duck_texture_pack_get_config(pack.get(),&settings);
    check(settings.max_vram_write_coalesce_width == 1,"exact root-level coalesce option recognized");
    check(info.ignored_count && std::strstr(info.diagnostic,"BogusOption"),"unknown config option explicitly diagnosed");
    png(root/"duplicate"/"replacements"/"a"/ordinary); png(root/"duplicate"/"replacements"/"b"/ordinary);
    pack = create(root/"duplicate");
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_AMBIGUOUS,"recursive duplicate keys fall back deterministically");
    q.u_first = 254; q.u_last = 1;
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_NONE,"wrapped UV interval falls back");
}

void failed_payloads(const fs::path& root) {
    const char* name = "texpage-C16-000CA76CB9B329DC-256x256-0-0-4x2.png";
    png(root/"budget"/"replacements"/name);
    std::vector<uint16_t> vram(1024*512); place(vram,64,0,4,words(8)); auto q = query(vram,64,0,2,0,3,0,1);
    auto pack = create(root/"budget"); DuckTextureMatch match{};
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_FOUND,"over-budget payload identity remains valid metadata");
    duck_texture_pack_set_decode_budget(pack.get(),8);
    check(decode_fails(pack.get(),match.entry_id),"over-budget decoded image fails before pixel allocation");
    fs::create_directories(root/"malformed"/"replacements");
    std::ofstream invalid(root/"malformed"/"replacements"/name,std::ios::binary); invalid << "not PNG"; invalid.close();
    pack = create(root/"malformed");
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_FOUND,"malformed PNG identity can be indexed without render-thread decode");
    check(decode_fails(pack.get(),match.entry_id),"malformed PNG fails asynchronously for native fallback");
    const fs::path oversized = root/"dimension"/"replacements"/name;
    png(oversized);
    std::ifstream file(oversized,std::ios::binary); std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),{}); file.close();
    /* Forge a valid IHDR width above the decode dimension bound. Keeping the
     * tiny original IDAT makes the fixture inexpensive and verifies rejection
     * using image metadata before an enormous allocation could happen. */
    bytes[16] = 0; bytes[17] = 0; bytes[18] = 0x23; bytes[19] = 0x28; /* 9000 */
    const uint32_t crc = png_crc_update(0xffffffffu,bytes.data()+12,17)^0xffffffffu;
    for (unsigned i = 0; i < 4; ++i) bytes[29+i] = static_cast<uint8_t>(crc >> (24-8*i));
    std::ofstream changed(oversized,std::ios::binary|std::ios::trunc);
    changed.write(reinterpret_cast<const char*>(bytes.data()),bytes.size()); changed.close();
    pack = create(root/"dimension"); duck_texture_pack_match(pack.get(),&q,&match);
    check(decode_fails(pack.get(),match.entry_id),"oversized PNG IHDR fails within native fallback bounds");
}

void dumping(const fs::path& root) {
    config(root,"DumpTexturePages: true\nDumpTextureWidthThreshold: 1\nDumpTextureHeightThreshold: 1\nReducePaletteRange: false\n");
    auto pack = create(root); std::vector<uint16_t> vram(1024*512);
    vram[0] = 0x3210; vram[400*1024+1] = 0x001f; vram[400*1024+2] = 0x8000; vram[400*1024+3] = 0xffff;
    auto q = query(vram,0,0,0,0,3,0,0,0,400); char error[512]{};
    check(duck_texture_pack_dump_draw(pack.get(),&q,1,error,sizeof(error)) == HD_TEXTURE_LOOKUP_FOUND,"queue native ST page dump");
    check(duck_texture_pack_dump_draw(pack.get(),&q,1,error,sizeof(error)) == HD_TEXTURE_LOOKUP_NONE,"dump session deduplicates without filesystem access");
    pack.reset(); /* required to finish queued background writes */
    const fs::path file = root/"dumps"/"texpage-STP4-A92BF769517E0361-59786C612FB45363-64x256-0-0-4x1-P0-15.png";
    check(fs::is_regular_file(file),"fixed independent hash filename published at worker shutdown");
    int width = 0, height = 0, channels = 0;
    std::ifstream encoded_file(file,std::ios::binary);
    std::vector<uint8_t> encoded((std::istreambuf_iterator<char>(encoded_file)),{});
    auto* image = encoded.empty() ? nullptr : stbi_load_from_memory(encoded.data(),static_cast<int>(encoded.size()),&width,&height,&channels,4);
    constexpr uint8_t expected[] = {0,0,0,0,255,0,0,255,0,0,0,143,255,255,255,143};
    check(image && width == 4 && height == 1 && !std::memcmp(image,expected,sizeof(expected)),"dump decodes native indices/CLUT and ST alpha");
    if (image) stbi_image_free(image);
    std::ofstream owner(file,std::ios::binary|std::ios::trunc); owner << "owner edit"; owner.close();
    pack = create(root); duck_texture_pack_dump_draw(pack.get(),&q,1,error,sizeof(error)); pack.reset();
    std::ifstream edited(file,std::ios::binary); std::string payload((std::istreambuf_iterator<char>(edited)),{});
    check(payload == "owner edit","existing owner dump is never overwritten");
    config(root/"upload","DumpTextureWidthThreshold: 1\nDumpTextureHeightThreshold: 1\nReducePaletteRange: false\n");
    pack = create(root/"upload");
    duck_texture_pack_track_upload(pack.get(),0,0,1,1,vram.data(),vram.size());
    check(duck_texture_pack_dump_draw(pack.get(),&q,0,error,sizeof(error)) == HD_TEXTURE_LOOKUP_FOUND,"tracked source emits texupload dump");
    pack.reset();
    check(fs::is_regular_file(root/"upload"/"dumps"/"texupload-P4-A92BF769517E0361-59786C612FB45363-1x1-0-0-4x1-P0-15.png"),
          "upload dump retains original source dimensions");
    /* PNG fixtures remain independent of the VRAM pointer used by replacement
     * matching, so dumping never reads already-replaced pixels. */
}

void official_binary_oracle(const fs::path& root) {
    /* Literal oracle from the official DuckStation 697599c47 binary, driven
     * by a separately authored synthetic PS-X EXE. No source-derived expected
     * filename or hash is used here. Both normal/semi uses must aggregate. */
    std::vector<uint16_t> vram(1024*512), source(10*56);
    constexpr uint16_t pattern[]{0x3210,0x7654,0xba98,0xfedc};
    for (size_t i = 0; i < source.size(); ++i) source[i] = pattern[i%4];
    place(vram,640,0,10,source);
    for (unsigned i = 1; i < 16; ++i) {
        vram[256*1024+i] = static_cast<uint16_t>((i*1537)&0x7fff);
        vram[257*1024+i] = static_cast<uint16_t>(((i*293)&0x7fff)|0x8000);
    }
    auto pack = create(root); char error[512]{};
    check(duck_texture_pack_track_upload(pack.get(),640,0,10,56,vram.data(),vram.size()),"oracle immutable upload tracked");
    auto a = query(vram,640,0,0,0,31,0,31,0,256);
    check(duck_texture_pack_dump_draw(pack.get(),&a,0,error,sizeof(error)) == HD_TEXTURE_LOOKUP_FOUND,"oracle first ordinary use recorded");
    a.u_last = 35;
    check(duck_texture_pack_dump_draw(pack.get(),&a,1,error,sizeof(error)) == HD_TEXTURE_LOOKUP_FOUND,"oracle nested semi use expands same record");
    auto b = query(vram,640,0,0,0,39,32,55,0,257);
    check(duck_texture_pack_dump_draw(pack.get(),&b,0,error,sizeof(error)) == HD_TEXTURE_LOOKUP_FOUND,"oracle second palette crop recorded");
    DuckTexturePackInfo info{}; duck_texture_pack_get_info(pack.get(),&info);
    check(info.pending_dump_sources == 1 && info.pending_palette_records == 2 && info.queued_dump_count == 0,
          "authoring observes one source/two palettes and writes nothing before retirement");
    check(dumps(root).empty(),"oracle draws do not emit per-draw files");
    place(vram,640,0,10,std::vector<uint16_t>(560));
    check(duck_texture_pack_track_upload(pack.get(),640,0,10,56,vram.data(),vram.size()),"oracle full overwrite retires original snapshot");
    flush(pack.get()); duck_texture_pack_get_info(pack.get(),&info);
    check(info.queued_dump_count == 2 && info.pending_palette_records == 0,"oracle retirement queues exactly two identities");
    const char* names[]{
        "texupload-STP4-18DDF9B731440D20-0DAE5F73C88719B1-10x56-0-0-36x32-P0-15.png",
        "texupload-P4-18DDF9B731440D20-AF506869B80F5CE7-10x56-0-32-40x24-P0-15.png"};
    check(dumps(root).size() == 2,"official binary golden fixture produces only its two PNGs");
    for (unsigned variant = 0; variant < 2; ++variant) {
        const unsigned width = variant ? 40 : 36, height = variant ? 24 : 32, y0 = variant ? 32 : 0;
        const auto pixels = read_png(root/"dumps"/names[variant],width,height);
        bool correct = pixels.size() == size_t(width)*height*4;
        for (unsigned y = 0; correct && y < height; ++y) for (unsigned x = 0; x < width; ++x) {
            const unsigned index = (pattern[((y+y0)*10+x/4)%4]>>((x%4)*4))&15;
            const uint16_t color = index ? static_cast<uint16_t>(variant ? ((index*293)&0x7fff)|0x8000 : (index*1537)&0x7fff) : 0;
            const size_t at = (size_t(y)*width+x)*4;
            for (unsigned c = 0; c < 3; ++c) correct &= pixels[at+c] == (((color>>(5*c))&31)*255+15)/31;
            correct &= pixels[at+3] == (color ? 255 : 0);
        }
        check(correct,"official binary golden crop retains original pixels and captured palette");
    }
}

void defaults_and_options(const fs::path& root) {
    auto pack = create(root/"defaults"); DuckTextureConfig c{};
    duck_texture_pack_get_config(pack.get(),&c);
    check(!c.dump_texture_pages && !c.dump_full_texture_pages && !c.dump_c16_textures && c.reduce_palette_range &&
          c.texture_dump_width_threshold == 16 && c.texture_dump_height_threshold == 16 &&
          !c.max_vram_write_splits && !c.max_vram_write_coalesce_width && !c.max_vram_write_coalesce_height &&
          !c.convert_copies_to_writes && !c.replacement_scale_linear_filter && c.dump_vram_write_force_alpha_channel &&
          c.vram_write_dump_width_threshold == 128 && c.vram_write_dump_height_threshold == 128,
          "pinned native authoring defaults are exact");
    std::vector<uint16_t> vram(1024*512); char error[512]{};
    auto q = query(vram,0,0,0,0,63,0,31,0,400);
    check(duck_texture_pack_dump_draw(pack.get(),&q,0,error,sizeof(error)) == HD_TEXTURE_LOOKUP_NONE,"default no upload means no page fallback");
    duck_texture_pack_track_upload(pack.get(),0,0,4,15,vram.data(),vram.size()); q.u_last = 15; q.v_last = 14;
    duck_texture_pack_dump_draw(pack.get(),&q,0,error,sizeof(error)); flush(pack.get());
    check(dumps(root/"defaults").empty(),"height below minimum suppresses dump even when width meets threshold");
    duck_texture_pack_track_upload(pack.get(),0,0,16,16,vram.data(),vram.size()); q.depth = 2; q.v_last = 15;
    check(duck_texture_pack_dump_draw(pack.get(),&q,1,error,sizeof(error)) == HD_TEXTURE_LOOKUP_NONE,"C16 dumps disabled by default");
    flush(pack.get()); check(dumps(root/"defaults").empty(),"default C16 has no delayed output");

    config(root/"options","DumpTexturePages: true\nDumpFullTexturePages: true\nDumpTextureForceAlphaChannel: true\n"
        "DumpC16Textures: true\nReducePaletteRange: false\nDumpTextureWidthThreshold: 1\nDumpTextureHeightThreshold: 1\n"
        "MaxVRAMWriteSplits: 3\nMaxVRAMWriteCoalesceWidth: 9\nMaxVRAMWriteCoalesceHeight: 11\nConvertCopiesToWrites: true\n"
        "ReplacementScaleLinearFilter: true\nDumpVRAMWriteForceAlphaChannel: false\nDumpVRAMWriteWidthThreshold: 3\nDumpVRAMWriteHeightThreshold: 5\n"
        "Options:\n  DumpTexturePages: false\nUnknownOption: true\n");
    pack = create(root/"options"); duck_texture_pack_get_config(pack.get(),&c);
    check(c.dump_texture_pages && c.dump_full_texture_pages && c.dump_texture_force_alpha_channel && c.dump_c16_textures &&
          !c.reduce_palette_range && c.max_vram_write_splits == 3 && c.max_vram_write_coalesce_width == 9 &&
          c.max_vram_write_coalesce_height == 11 && c.convert_copies_to_writes && duck_texture_pack_linear_filter(pack.get()) &&
          !c.dump_vram_write_force_alpha_channel && c.vram_write_dump_width_threshold == 3 && c.vram_write_dump_height_threshold == 5,
          "exact root-level scalar options parsed without nested Options override");
    DuckTexturePackInfo info{}; duck_texture_pack_get_info(pack.get(),&info);
    check(std::strstr(info.diagnostic,"Options") && std::strstr(info.diagnostic,"UnknownOption"),"unsupported nested and unknown options diagnosed");
    q = query(vram,0,0,2,0,0,0,0);
    duck_texture_pack_dump_draw(pack.get(),&q,1,error,sizeof(error)); flush(pack.get());
    const auto files = dumps(root/"options"); check(files.size() == 1,"full page force-alpha emits one page");
    if (!files.empty()) {
        DuckTextureKey key{}; duck_texture_parse_name(files[0].filename().u8string().c_str(),&key);
        check(key.width == 256 && key.height == 256 && !key.semitransparent,"fullpage and force-alpha filename conventions");
        const auto pixels = read_png(files[0],256,256);
        check(pixels.size() == 256*256*4 && pixels[0] == 0 && pixels[3] == 255,"force-alpha preserves opaque native zero");
    }
    config(root/"zero","MaxHashCacheVRAMUsageMB: 0\n"); pack = create(root/"zero");
    check(!duck_texture_pack_track_upload(pack.get(),0,0,16,16,vram.data(),vram.size()),"zero snapshot budget falls back without blocking");
    duck_texture_pack_get_info(pack.get(),&info); check(std::strstr(info.diagnostic,"budget"),"snapshot budget fallback diagnosed");
    flush(pack.get());
}

void immutable_palettes_and_pages(const fs::path& root) {
    std::vector<uint16_t> vram(1024*512);
    place(vram,0,0,8,std::vector<uint16_t>(8*16,0x3210)); vram[400*1024+1] = 0x001f;
    auto pack = create(root/"upload"); auto q = query(vram,0,0,0,0,15,0,15,0,400); char error[512]{};
    duck_texture_pack_track_upload(pack.get(),0,0,8,16,vram.data(),vram.size());
    duck_texture_pack_dump_draw(pack.get(),&q,0,error,sizeof(error)); q.u_last = 31;
    duck_texture_pack_dump_draw(pack.get(),&q,1,error,sizeof(error));
    vram[400*1024+1] = 0x83e0; duck_texture_pack_invalidate(pack.get(),1,400,1,1);
    duck_texture_pack_dump_draw(pack.get(),&q,1,error,sizeof(error));
    /* Overwrite both source and CLUT before retirement; neither may be read
     * by the writer instead of the saved original words/palettes. */
    place(vram,0,0,8,std::vector<uint16_t>(8*16)); vram[400*1024+1] = 0x7c00;
    duck_texture_pack_invalidate(pack.get(),0,0,8,16); flush(pack.get());
    const auto files = dumps(root/"upload"); check(files.size() == 2,"palette versions keep separate lifetime records");
    bool old_red = false, saved_green = false;
    for (const auto& file : files) {
        DuckTextureKey key{}; duck_texture_parse_name(file.filename().u8string().c_str(),&key);
        check(key.width == 32 && key.height == 16 && key.semitransparent && key.palette_min == 0 && key.palette_max == 3,
              "nested crops union, ordinary+semi aggregate, palette bounds reduced");
        const auto pixels = read_png(file,32,16);
        if (pixels.size() >= 8) { old_red |= pixels[4] == 255 && pixels[5] == 0 && pixels[7] == 255;
            saved_green |= pixels[4] == 0 && pixels[5] == 255 && pixels[7] == 143; }
    }
    check(old_red && saved_green,"captured palette versions retain original colors and ST flags");

    config(root/"page","DumpTexturePages: true\nReducePaletteRange: false\n");
    vram.assign(vram.size(),0); place(vram,0,0,8,std::vector<uint16_t>(8*16,0x1111));
    vram[400*1024+1] = 0x001f; vram[401*1024+1] = 0x03e0;
    pack = create(root/"page"); q = query(vram,0,0,0,0,15,0,15,0,400);
    duck_texture_pack_dump_draw(pack.get(),&q,0,error,sizeof(error)); auto b = q; b.clut_y = 401;
    duck_texture_pack_dump_draw(pack.get(),&b,0,error,sizeof(error));
    vram[401*1024+1] = 0x7c00; duck_texture_pack_invalidate(pack.get(),1,401,1,1);
    q.u_last = 31; duck_texture_pack_dump_draw(pack.get(),&q,0,error,sizeof(error)); flush(pack.get());
    const auto pagefiles = dumps(root/"page"); check(pagefiles.size() == 2,"changing CLUT B does not fragment CLUT A lifetime");
    bool a_union = false, b_original = false;
    for (const auto& file : pagefiles) {
        DuckTextureKey key{}; duck_texture_parse_name(file.filename().u8string().c_str(),&key);
        const auto pixels = read_png(file,key.width,key.height);
        if (!pixels.empty()) { a_union |= key.width == 32 && pixels[0] == 255;
            b_original |= key.width == 16 && pixels[1] == 255 && pixels[2] == 0; }
    }
    check(a_union && b_original,"page authoring residency is isolated per CLUT and snapshots old palettes");
}

void split_coalesce_copy_parts(const fs::path& root) {
    std::vector<uint16_t> vram(1024*512); place(vram,0,0,4,std::vector<uint16_t>(4*16,0x3210));
    vram[400*1024+1] = 0x1f; vram[400*1024+2] = 0x3e0; vram[400*1024+3] = 0x7c00;
    DuckTextureKey key{}; key.depth = 0; key.source_width_words = 4; key.source_height = 16; key.width = 16; key.height = 16;
    key.source_hash = duck_texture_hash_rect(vram.data(),vram.size(),0,0,4,16);
    key.palette_hash = duck_texture_hash_words_le(vram.data()+400*1024,4); key.palette_max = 3;
    config(root/"split","MaxVRAMWriteSplits: 2\n"); png(root/"split"/"replacements"/filename(key),16,16);
    auto pack = create(root/"split"); duck_texture_pack_track_upload(pack.get(),0,0,4,16,vram.data(),vram.size());
    auto q = query(vram,0,0,0,0,15,0,15,0,400); char error[512]{};
    duck_texture_pack_dump_draw(pack.get(),&q,0,error,sizeof(error));
    vram[1024] = 0; duck_texture_pack_invalidate(pack.get(),0,1,1,1);
    std::array<DuckTextureMatch,64> parts{}; size_t count = 0;
    check(duck_texture_pack_match_parts(pack.get(),&q,0,parts.data(),parts.size(),&count) == HD_TEXTURE_LOOKUP_FOUND && count == 3,
          "partial overwrite splits original identity into three surviving clips");
    unsigned area = 0; for (size_t i = 0; i < count; ++i) area += unsigned(parts[i].clip_width)*parts[i].clip_height;
    check(area == 252,"split clips exclude exactly four overwritten native texels");
    check(duck_texture_pack_match_parts(pack.get(),&q,0,parts.data(),1,&count) == HD_TEXTURE_LOOKUP_ERROR && count == 0,
          "insufficient multi-part output capacity never truncates composition");
    auto cut = q; cut.u_last = 3; cut.v_first = cut.v_last = 1;
    DuckTextureMatch match{}; check(duck_texture_pack_match(pack.get(),&cut,&match) == HD_TEXTURE_LOOKUP_NONE,"overwritten region has native fallback");
    flush(pack.get()); const auto files = dumps(root/"split"); check(files.size() == 1,"split preserves pending original upload observation");
    if (!files.empty()) { const auto pixels = read_png(files[0],16,16); check(pixels.size() >= 68 && pixels[68] == 255,"split dump uses immutable preoverwrite words"); }

    config(root/"coalesce","MaxVRAMWriteCoalesceHeight: 8\n");
    vram.assign(vram.size(),0); place(vram,0,0,4,std::vector<uint16_t>(4*16,0x1111)); vram[400*1024+1] = 31;
    pack = create(root/"coalesce"); duck_texture_pack_track_upload(pack.get(),0,0,4,8,vram.data(),vram.size());
    duck_texture_pack_track_upload(pack.get(),0,8,4,8,vram.data(),vram.size());
    q = query(vram,0,0,0,0,15,0,15,0,400); duck_texture_pack_dump_draw(pack.get(),&q,0,error,sizeof(error)); flush(pack.get());
    const auto coalesced = dumps(root/"coalesce"); check(coalesced.size() == 1,"unused adjacent writes coalesce before authoring use");
    if (!coalesced.empty()) { DuckTextureKey k{}; duck_texture_parse_name(coalesced[0].filename().u8string().c_str(),&k);
        check(k.source_width_words == 4 && k.source_height == 16,"coalesced original extent retained in filename"); }

    config(root/"copy","ConvertCopiesToWrites: true\nDumpC16Textures: true\nDumpTextureWidthThreshold: 1\nDumpTextureHeightThreshold: 1\n");
    vram.assign(vram.size(),0); place(vram,64,0,4,words(64)); vram[0] = 0x8001;
    const uint64_t oldhash = duck_texture_hash_rect(vram.data(),vram.size(),64,0,4,16);
    auto changed = vram; changed[64+1024] = vram[0];
    DuckTextureKey updated{}; updated.depth = 2; updated.source_width_words = 4; updated.source_height = 16;
    updated.width = 4; updated.height = 16; updated.source_hash = duck_texture_hash_rect(changed.data(),changed.size(),64,0,4,16);
    png(root/"copy"/"replacements"/filename(updated));
    pack = create(root/"copy"); duck_texture_pack_track_upload(pack.get(),64,0,4,16,vram.data(),vram.size());
    q = query(vram,64,0,2,0,3,0,15); duck_texture_pack_dump_draw(pack.get(),&q,0,error,sizeof(error));
    check(duck_texture_pack_begin_copy(pack.get(),0,0,64,1,1,1,vram.data(),vram.size()),"copy begins before native write and retires old observation");
    vram[64+1024] = 0x8001;
    check(duck_texture_pack_end_copy(pack.get(),vram.data(),vram.size()),"copy commit records postmask native destination");
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_FOUND && match.key.source_hash == updated.source_hash && updated.source_hash != oldhash,
          "copy-to-write rehashes whole ORIGINAL destination extent");
    flush(pack.get()); const auto copyfiles = dumps(root/"copy"); check(copyfiles.size() == 1,"copy flush preserves old destination authored texture");

    pack = create(root/"copy_default"); duck_texture_pack_track_upload(pack.get(),64,0,4,16,vram.data(),vram.size());
    duck_texture_pack_begin_copy(pack.get(),0,0,64,1,1,1,vram.data(),vram.size());
    duck_texture_pack_end_copy(pack.get(),vram.data(),vram.size());
    DuckTexturePackInfo info{}; duck_texture_pack_get_info(pack.get(),&info);
    check(info.snapshot_bytes == 0,"default copy drops destination identity rather than cloning source provenance");

    config(root/"parts","MaxVRAMWriteSplits: 2\n");
    vram.assign(vram.size(),0); place(vram,0,0,8,std::vector<uint16_t>(8*16,0x3210));
    key.source_width_words = 8; key.source_hash = duck_texture_hash_rect(vram.data(),vram.size(),0,0,8,16);
    key.palette_hash = duck_texture_hash_words_le(vram.data()+400*1024,4); key.width = 16;
    png(root/"parts"/"replacements"/filename(key)); key.semitransparent = 1;
    png(root/"parts"/"replacements"/filename(key)); key.offset_x = 16;
    png(root/"parts"/"replacements"/filename(key));
    pack = create(root/"parts"); duck_texture_pack_track_upload(pack.get(),0,0,8,16,vram.data(),vram.size());
    q = query(vram,0,0,0,0,31,0,15,0,400);
    check(duck_texture_pack_match_parts(pack.get(),&q,0,parts.data(),parts.size(),&count) == HD_TEXTURE_LOOKUP_FOUND && count == 2,
          "adjacent replacement rectangles compose across one source");
    bool left_ordinary = false, right_st = false;
    for (size_t i = 0; i < count; ++i) { left_ordinary |= parts[i].clip_u == 0 && !parts[i].key.semitransparent;
        right_st |= parts[i].clip_u == 16 && parts[i].key.semitransparent; }
    check(left_ordinary && right_st,"same-coverage ST preference does not remove another rectangle's only ST asset");
    check(duck_texture_pack_match_draw(pack.get(),&q,0,&match) == HD_TEXTURE_LOOKUP_NONE,"single-cover API preserves native fallback for composed draw");
}

void copied_pack_ownership(const fs::path& root) {
    std::vector<uint16_t> vram(1024*512); place(vram,0,0,8,std::vector<uint16_t>(8*16,0x3210));
    vram[400*1024+1] = 31;
    auto original = create(root/"same"); duck_texture_pack_track_upload(original.get(),0,0,8,16,vram.data(),vram.size());
    auto q = query(vram,0,0,0,0,15,0,15,0,400); char error[512]{};
    duck_texture_pack_dump_draw(original.get(),&q,0,error,sizeof(error));
    {
        auto copied = create(root/"same"); check(duck_texture_pack_copy_tracking(copied.get(),original.get()),"same-root tracking clone with pending observations");
        DuckTexturePackInfo info{}; duck_texture_pack_get_info(copied.get(),&info);
        check(info.pending_dump_sources == 1 && info.pending_palette_records == 1,"cloned pack owns independent palette record count");
    } /* Copy destroyed FIRST: original still has valid owned observations. */
    q.u_last = 31;
    check(duck_texture_pack_dump_draw(original.get(),&q,1,error,sizeof(error)) == HD_TEXTURE_LOOKUP_FOUND,
          "original remains valid and expands union after copy destruction");
    flush(original.get()); check(dumps(root/"same").size() == 2,"copy destruction does not steal original larger crop");
    duck_texture_pack_dump_draw(original.get(),&q,1,error,sizeof(error));
    auto elsewhere = create(root/"different"); check(duck_texture_pack_copy_tracking(elsewhere.get(),original.get()),"different-root clone retains provenance");
    original.reset(); flush(elsewhere.get());
    check(dumps(root/"different").size() == 1,"different root does not share filename deduplication suppression");
}
} // namespace

int main(int argc, char** argv) {
    const fs::path root = fs::temp_directory_path()/fs::u8path("psx-duck-texture-\xCE\xA9-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        fixed_hash_vectors(); names(); page_and_decode(root/"page"); upload(root/"upload"); reload_tracking(root/"reload");
        palettes(root/"palettes"); st_alias_duplicates(root/"keys"); failed_payloads(root/"invalid"); dumping(root/"dump");
        official_binary_oracle(root/"official-oracle"); defaults_and_options(root/"defaults");
        immutable_palettes_and_pages(root/"snapshots"); split_coalesce_copy_parts(root/"lifecycle"); copied_pack_ownership(root/"ownership");
        /* Every artifact is created beneath this fresh, explicitly named test
         * root. No user-provided paths participate in recursive cleanup. */
        if (argc > 1 && !std::strcmp(argv[1],"--keep-artifacts")) std::printf("ARTIFACT_ROOT: %s\n",root.u8string().c_str());
        else fs::remove_all(root);
    } catch (const std::exception& e) { check(false,e.what()); }
    if (failures) { std::fprintf(stderr,"test_duckstation_texture_pack: %d failure(s)\n",failures); return 1; }
    std::puts("PASS: DuckStation format, independent hashes, native matching, decode and dumps");
    return 0;
}

#include "mod_media.h"
#include "iso_reader.h"
#include "psx_sha256.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cctype>
#include <fstream>
#include <stdexcept>

namespace PSXRecompV4 {
namespace {
constexpr uint64_t kMaxMediaBytes = 512ull * 1024 * 1024;

uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint32_t be32(const uint8_t* p) {
    return uint32_t(p[3]) | (uint32_t(p[2]) << 8) |
           (uint32_t(p[1]) << 16) | (uint32_t(p[0]) << 24);
}

void read_disc_sector(PS1::ISOReader& reader, uint32_t lba, uint8_t* out) {
    std::array<uint8_t, 2352> raw{};
    if (reader.ReadRawSector(lba, raw.data())) {
        if (raw[15] != 1 && raw[15] != 2)
            throw std::runtime_error("donor volume contains a non-data sector");
        std::memcpy(out, raw.data() + (raw[15] == 1 ? 16 : 24), 2048);
    } else if (!reader.ReadSector(lba, out)) {
        throw std::runtime_error("cannot read donor volume sector");
    }
}
} // namespace

bool load_mod_media(const std::filesystem::path& path,
                    const std::string& format, uint64_t expected_size,
                    const std::string& expected_sha256,
                    std::shared_ptr<const std::vector<uint8_t>>& bytes,
                    std::string* error) {
    bytes.reset();
    if (error) error->clear();
    try {
        const uint64_t limit = format == "psx-mode2" ? 450000ull * 2336 : kMaxMediaBytes;
        if (!expected_size || expected_size > limit ||
            expected_sha256.size() != 64 ||
            !std::all_of(expected_sha256.begin(), expected_sha256.end(),
                         [](char c) { return (c >= '0' && c <= '9') ||
                                             (c >= 'a' && c <= 'f'); }))
            throw std::runtime_error("invalid donor size or SHA-256");
        auto data = std::make_shared<std::vector<uint8_t>>();
        if (format == "psx-mode2") {
            // Keep XA subheaders and all 2328 payload/EDC/ECC bytes. A cooked
            // ISO loses Form-2 audio/video and cannot represent this resource.
            PS1::ISOReader reader;
            if (!reader.Open(path.string()))
                throw std::runtime_error("cannot open Mode-2 donor disc");
            const uint32_t count = reader.TrackCount() > 1
                ? reader.TrackPregapLBA(2) : reader.GetSectorCount();
            if (reader.TrackIsAudio(1) || reader.TrackStartLBA(1) != 0 ||
                count <= 16 || uint64_t(count) * 2336 != expected_size)
                throw std::runtime_error("Mode-2 donor data-track size does not match");
            data->resize(static_cast<size_t>(expected_size));
            std::array<uint8_t, 2352> raw{};
            for (uint32_t i = 0; i < count; ++i) {
                if (!reader.ReadRawSector(i, raw.data()) || raw[15] != 2 ||
                    std::memcmp(raw.data()+16, raw.data()+20, 4))
                    throw std::runtime_error("donor requires raw Mode-2 sectors (BIN/CUE or raw ISO); cooked ISO loses XA media");
                std::memcpy(data->data()+size_t(i)*2336, raw.data()+16, 2336);
            }
            const uint8_t* pvd = data->data()+16*2336+8;
            if (pvd[0] != 1 || std::memcmp(pvd+1, "CD001", 5) || pvd[6] != 1 ||
                le32(pvd+80) != be32(pvd+84) || le32(pvd+80) < count ||
                pvd[128] != 0 || pvd[129] != 8 || pvd[130] != 8 || pvd[131] != 0)
                throw std::runtime_error("Mode-2 donor has an invalid ISO9660 volume");
        } else if (format == "psx-disc") {
            PS1::ISOReader reader;
            if (!reader.Open(path.string()))
                throw std::runtime_error("cannot open donor disc");
            std::array<uint8_t, 2048> pvd{};
            read_disc_sector(reader, 16, pvd.data());
            if (pvd[0] != 1 || std::memcmp(pvd.data() + 1, "CD001", 5) ||
                pvd[6] != 1 || pvd[128] != 0 || pvd[129] != 8 ||
                pvd[130] != 8 || pvd[131] != 0)
                throw std::runtime_error("donor disc is not a 2048-byte ISO9660 volume");
            const uint32_t declared_count = le32(pvd.data() + 80);
            /* Retail mixed-mode discs may declare the whole disc, including
             * audio, as their ISO volume size. The actual data-track boundary
             * comes from the TOC or the single-file image length. */
            const uint32_t count = reader.TrackCount() > 1
                ? reader.TrackPregapLBA(2) : reader.GetSectorCount();
            if (reader.TrackIsAudio(1) || reader.TrackStartLBA(1) != 0 ||
                declared_count != be32(pvd.data() + 84) || declared_count < count ||
                count <= 16 || uint64_t(count) * 2048 != expected_size)
                throw std::runtime_error("donor data-track size does not match");
            data->resize(static_cast<size_t>(expected_size));
            for (uint32_t i = 0; i < count; ++i)
                read_disc_sector(reader, i, data->data() + size_t(i) * 2048);
        } else if (format == "file" || format == "n64-rom") {
            if (std::filesystem::file_size(path) != expected_size)
                throw std::runtime_error("donor file size does not match");
            if (format == "n64-rom" &&
                (expected_size < 64 || expected_size % 4 || expected_size > 64ull * 1024 * 1024))
                throw std::runtime_error("invalid N64 ROM size");
            std::ifstream input(path, std::ios::binary);
            data->resize(static_cast<size_t>(expected_size));
            if (!input.read(reinterpret_cast<char*>(data->data()),
                            static_cast<std::streamsize>(expected_size)) ||
                input.peek() != std::char_traits<char>::eof())
                throw std::runtime_error("cannot read complete donor file");
            if (format == "n64-rom") {
                const uint32_t magic = be32(data->data());
                if (magic == 0x37804012) {
                    for (size_t i = 0; i < data->size(); i += 2)
                        std::swap((*data)[i], (*data)[i + 1]);
                } else if (magic == 0x40123780) {
                    for (size_t i = 0; i < data->size(); i += 4)
                        std::reverse(data->begin() + i, data->begin() + i + 4);
                } else if (magic != 0x80371240) {
                    throw std::runtime_error("unrecognized N64 ROM byte order");
                }
            }
        } else {
            throw std::runtime_error("unsupported donor media format");
        }
        uint8_t digest[32];
        psx_sha256_compute(data->data(), data->size(), digest);
        static const char hex[] = "0123456789abcdef";
        std::string actual(64, '0');
        for (size_t i = 0; i < 32; ++i) {
            actual[i * 2] = hex[digest[i] >> 4];
            actual[i * 2 + 1] = hex[digest[i] & 15];
        }
        if (actual != expected_sha256)
            throw std::runtime_error("donor SHA-256 does not match");
        bytes = std::move(data);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

bool ModMode2Disc::find(const std::string& path,uint32_t& lba,uint32_t& size) const {
    lba=size=0;
    if(!bytes_ || size_%2336 || size_<17*2336)return false;
    const uint8_t* pvd=bytes_+16*2336+8;
    if(pvd[0]!=1 || std::memcmp(pvd+1,"CD001",5) || pvd[6]!=1)return false;
    uint32_t at_lba=le32(pvd+158),at_size=le32(pvd+166);
    bool directory=true;size_t at=0;
    while(at<path.size()) {
        const size_t slash=path.find_first_of("/\\",at);
        const size_t end=slash==std::string::npos?path.size():slash;
        auto want=path.substr(at,end-at);at=end+1;
        if(want.empty())continue;
        if(want=="." || want==".." || !directory)return false;
        std::transform(want.begin(),want.end(),want.begin(),[](unsigned char c){return char(std::toupper(c));});
        const uint64_t count=(uint64_t(at_size)+2047)/2048;
        if(!count || at_lba>size_/2336 || count>size_/2336-at_lba || at_size>16*1024*1024)return false;
        bool found=false;
        for(uint32_t i=0;i<count && !found;++i) {
            const auto* sector=bytes_+uint64_t(at_lba+i)*2336;
            if(sector[2]&0x20)return false;
            const auto* data=sector+8;
            const uint32_t limit=uint32_t(std::min<uint64_t>(2048,uint64_t(at_size)-uint64_t(i)*2048));
            for(uint32_t p=0;p<limit && data[p];p+=data[p]) {
                const uint32_t n=data[p];
                if(n<34 || n>limit-p)return false;
                const uint32_t len=data[p+32];
                if(33+len>n)return false;
                std::string name(reinterpret_cast<const char*>(data+p+33),len);
                name=name.substr(0,name.find(';'));
                std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return char(std::toupper(c));});
                if(name!=want)continue;
                if(le32(data+p+2)!=be32(data+p+6) || le32(data+p+10)!=be32(data+p+14))return false;
                at_lba=le32(data+p+2);at_size=le32(data+p+10);directory=(data[p+25]&2)!=0;
                found=true;break;
            }
        }
        if(!found)return false;
    }
    const uint64_t count=(uint64_t(at_size)+2047)/2048;
    if(directory || !at_size || at_lba>size_/2336 || count>size_/2336-at_lba)return false;
    lba=at_lba;size=at_size;return true;
}
bool ModMode2Disc::read(const std::string& path,std::vector<uint8_t>& out) const {
    out.clear();uint32_t lba=0,size=0;
    if(!find(path,lba,size) || size>16*1024*1024)return false;
    std::vector<uint8_t> result(size);
    for(uint32_t p=0;p<size;) {
        const auto* sector=bytes_+uint64_t(lba+p/2048)*2336;
        if(sector[2]&0x20)return false;
        const uint32_t n=std::min(2048u,size-p);
        std::memcpy(result.data()+p,sector+8,n);p+=n;
    }
    out=std::move(result);return true;
}
} // namespace PSXRecompV4

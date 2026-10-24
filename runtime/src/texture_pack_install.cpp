/* Small read-only installer validation. Target metadata is a declaration,
 * never proof that every translated asset has a replacement. */
#include "gpu_hd_textures.h"
#include "duckstation_texture_pack.h"
#include "texture_image_decode.h"
#include "toml.hpp"
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>
#include <memory>
#include <cctype>
#include <cstdio>
#include <stdexcept>
namespace fs=std::filesystem;
static std::string normalized(std::string s) {
    std::string out; for(unsigned char c:s) if(std::isalnum(c)) out+=char(std::toupper(c)); return out;
}
static bool target_matches(const fs::path& root,const std::string& game) {
    const auto metadata=root/"texture-pack.toml";
    if(fs::is_regular_file(metadata)) {
        auto data=toml::parse(metadata.u8string());
        auto targets=toml::find<std::vector<std::string>>(data,"game_ids");
        return std::any_of(targets.begin(),targets.end(),[&](const std::string& id){return normalized(id)==normalized(game);});
    }
    return normalized(root.filename().u8string())==normalized(game);
}
extern "C" int gpu_hd_textures_validate_install(const char* input,const char* game,
    char* resolved,size_t resolved_cap,char* message,size_t cap) {
    auto say=[&](const std::string& s){if(message&&cap)std::snprintf(message,cap,"%s",s.c_str());};
    if(resolved&&resolved_cap)resolved[0]=0;
    try {
        if(!input||!input[0]) {say("No texture pack installed");return 0;}
        fs::path root=fs::u8path(input);
        if(!fs::is_directory(root)){say("No texture pack installed - selected folder is missing.");return 0;}
        if(root.filename()=="replacements") root=root.parent_path();
        std::vector<fs::path> candidates;
        auto candidate=[&](const fs::path& dir){
            if(fs::is_directory(dir/"replacements") && target_matches(dir,game?game:""))candidates.push_back(dir);
        };
        candidate(root);
        if(candidates.empty()) {
            size_t visited=0;
            for(fs::recursive_directory_iterator it(root),end;it!=end;++it) {
                if(++visited>4096)throw std::runtime_error("Folder scan limit reached; select the pack folder itself.");
                if(it->is_symlink()){if(it->is_directory())it.disable_recursion_pending();continue;}
                if(it->is_directory()) {
                    if(it.depth()>=4||it->path().filename()=="replacements"||it->path().filename()=="dumps")it.disable_recursion_pending();
                    else candidate(it->path());
                }
            }
        }
        if(candidates.empty())throw std::runtime_error("Incompatible or unidentified pack: no target for "+std::string(game?game:"")+". Select a matching game-ID folder or a pack declaring game_ids in texture-pack.toml.");
        if(candidates.size()!=1)throw std::runtime_error("Several compatible packs found; select the desired pack folder.");
        root=candidates.front();
        DuckTexturePack* raw=nullptr;char error[1024]{};
        if(!duck_texture_pack_create(root.u8string().c_str(),&raw,error,sizeof error))throw std::runtime_error(error);
        std::unique_ptr<DuckTexturePack,decltype(&duck_texture_pack_destroy)> duck(raw,duck_texture_pack_destroy);
        DuckTexturePackInfo info{};duck_texture_pack_get_info(duck.get(),&info);
        if(info.ambiguous_count || (info.diagnostic&&info.diagnostic[0]))throw std::runtime_error(info.diagnostic&&info.diagnostic[0]?info.diagnostic:"Ambiguous replacement names.");
        // This installer deliberately accepts the shared DuckStation wire format.
        // Beetle packs remain usable by legacy manifests without texture_game_id.
        size_t count=0;uint64_t total=0;
        for(const auto& entry:fs::recursive_directory_iterator(root/"replacements")) {
            if(entry.is_symlink())throw std::runtime_error("Linked replacement files are not supported.");
            if(!entry.is_regular_file())continue;
            std::string ext=entry.path().extension().u8string();std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return char(std::tolower(c));});
            if(ext!=".png"&&ext!=".jpg"&&ext!=".jpeg"&&ext!=".webp")continue;
            DuckTextureKey key{};
            if(!duck_texture_parse_name(entry.path().filename().u8string().c_str(),&key))throw std::runtime_error("Invalid replacement filename: "+entry.path().filename().u8string());
            auto bytes=entry.file_size();total+=bytes;
            if(!bytes||bytes>TEXTURE_IMAGE_MAX_ENCODED_BYTES||total>4ull*1024*1024*1024||++count>65536)throw std::runtime_error("Replacement image exceeds installer limits.");
            std::vector<uint8_t> encoded(static_cast<size_t>(bytes));std::ifstream in(entry.path(),std::ios::binary);
            DecodedTextureImage image;std::string diagnostic;
            if(!in.read(reinterpret_cast<char*>(encoded.data()),encoded.size())||!texture_image_decode(encoded.data(),encoded.size(),TEXTURE_IMAGE_DEFAULT_DECODED_BUDGET,image,diagnostic))throw std::runtime_error("Invalid image "+entry.path().filename().u8string()+": "+diagnostic);
            if(image.width%key.width||image.height%key.height||image.width/key.width!=image.height/key.height)throw std::runtime_error("Replacement image has an incompatible scale: "+entry.path().filename().u8string());
        }
        if(!count){say("No texture pack installed");return 0;}
        if(count!=info.replacement_count)throw std::runtime_error("Incomplete or aliased pack: replacement/image counts differ.");
        const std::string path=fs::absolute(root).u8string();
        if(!resolved||path.size()+1>resolved_cap)throw std::runtime_error("Selected path is too long.");
        std::snprintf(resolved,resolved_cap,"%s",path.c_str());say("Installed - "+std::to_string(count)+" validated images; target "+std::string(game)+". Asset coverage depends on the pack.");return 1;
    }catch(const std::exception& e){say(std::string("Not installed - ")+e.what());return 0;}
}

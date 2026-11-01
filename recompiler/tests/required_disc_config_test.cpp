#include "config_loader.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace fs=std::filesystem;
int main() {
    const auto root=fs::temp_directory_path()/("required-disc-config-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    const auto config=root/"game.toml";
    auto load=[&](const std::string& hashes) {
        {std::ofstream f(config);f<<"[game]\nname=\"Test\"\nid=\"SLUS-TEST\"\nexe=\"stock.exe\"\nload_address=\"0x80010000\"\nentry_pc=\"0x80010000\"\ntext_size=\"0x1000\"\nstack_base=\"0x801ffff0\"\n"<<hashes<<"[recompiler]\nseeds=\"seeds.txt\"\n";}
        return PSXRecompV4::load_game_config(config);
    };
    auto check=[](bool ok){if(!ok)throw std::runtime_error("required-disc config regression");};
    check(load("").required_disc_sha256.empty());
    check(load("required_disc_sha256=[\""+std::string(64,'A')+"\"]\n").required_disc_sha256==std::vector<std::string>{std::string(64,'a')});
    check(load("required_disc_fps=[\""+std::string(64,'B')+"\"]\n").required_disc_fps==std::vector<std::string>{std::string(64,'b')});
    for(const auto* key:{"required_disc_sha256","required_disc_fps"}) {
        for(auto bad:std::vector<std::string>{"[]","[\"\"]","[\"abc\"]","[\""+std::string(64,'z')+"\"]"}) {
            bool rejected=false;try{(void)load(std::string(key)+"="+bad+"\n");}catch(const std::exception&){rejected=true;}check(rejected);
        }
    }
    fs::remove(config);fs::remove(root);
    std::cout<<"Required original-disc config: normalization and malformed/empty policies pass\n";
}

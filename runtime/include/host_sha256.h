#pragma once
/* Optional Windows system SHA-256 acceleration. Every byte is still hashed.
 * No persistent trust cache; callers retain their portable implementation. */
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <array>
#if defined(_WIN32) && !defined(PSX_DISABLE_NATIVE_SHA256)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
namespace PSXRecompV4 {
class HostSha256 {
    struct Api {
        HMODULE dll=nullptr;BCRYPT_ALG_HANDLE algorithm=nullptr;
        decltype(&BCryptOpenAlgorithmProvider) open=nullptr;
        decltype(&BCryptCloseAlgorithmProvider) close=nullptr;
        decltype(&BCryptCreateHash) create=nullptr;
        decltype(&BCryptHashData) update=nullptr;
        decltype(&BCryptFinishHash) finish=nullptr;
        decltype(&BCryptDestroyHash) destroy=nullptr;
        Api() {
            dll=LoadLibraryExW(L"bcrypt.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
            if(!dll)return;
#define PSX_CNG_LOAD(member,name) member=reinterpret_cast<decltype(member)>(GetProcAddress(dll,name))
            PSX_CNG_LOAD(open,"BCryptOpenAlgorithmProvider");PSX_CNG_LOAD(close,"BCryptCloseAlgorithmProvider");
            PSX_CNG_LOAD(create,"BCryptCreateHash");PSX_CNG_LOAD(update,"BCryptHashData");
            PSX_CNG_LOAD(finish,"BCryptFinishHash");PSX_CNG_LOAD(destroy,"BCryptDestroyHash");
#undef PSX_CNG_LOAD
            if(open&&close&&create&&update&&finish&&destroy)
                if(open(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)algorithm=nullptr;
        }
        ~Api(){if(algorithm)close(algorithm,0);if(dll)FreeLibrary(dll);}
    };
    static Api& api(){static Api value;return value;}
    BCRYPT_HASH_HANDLE hash_=nullptr;
public:
    HostSha256(){auto& a=api();if(a.algorithm&&a.create(a.algorithm,&hash_,nullptr,0,nullptr,0,0)<0)hash_=nullptr;}
    ~HostSha256(){if(hash_)api().destroy(hash_);}
    HostSha256(const HostSha256&)=delete;HostSha256& operator=(const HostSha256&)=delete;
    bool update(const uint8_t* bytes,size_t size) {
        if(!hash_)return false;
        while(size){ULONG take=static_cast<ULONG>(size>0x40000000u?0x40000000u:size);
            if(api().update(hash_,const_cast<PUCHAR>(bytes),take,0)<0)return false;
            bytes+=take;size-=take;}
        return true;
    }
    bool finish(uint8_t out[32]){return hash_&&api().finish(hash_,out,32,0)>=0;}
};
inline bool host_sha256_bytes(const uint8_t* bytes,size_t size,uint8_t out[32]) {
    HostSha256 h;return h.update(bytes,size)&&h.finish(out);
}
inline bool host_sha256_file(const std::filesystem::path& path,uint8_t out[32]) {
    HostSha256 h;std::ifstream in(path,std::ios::binary);if(!in)return false;
    std::array<uint8_t,64*1024> bytes;
    while(in){in.read(reinterpret_cast<char*>(bytes.data()),bytes.size());auto n=in.gcount();if(n>0&&!h.update(bytes.data(),size_t(n)))return false;}
    return in.eof()&&h.finish(out);
}
}
#else
namespace PSXRecompV4 {
inline bool host_sha256_bytes(const uint8_t*,size_t,uint8_t[32]){return false;}
inline bool host_sha256_file(const std::filesystem::path&,uint8_t[32]){return false;}
}
#endif

// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Direct updates require explicit user confirmation for the selected local
// card. Automated tests remain on isolated copies. Never format a missing input.
// Windows local NTFS is the only supported output/backup filesystem for now.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#include <cctype>
#include <filesystem>
#include <atomic>
#include <memory>
#include "ps1_card.hpp"

namespace cardstore {
namespace fs=std::filesystem;
struct Handle {
    HANDLE h=INVALID_HANDLE_VALUE;
    Handle()=default;
    explicit Handle(HANDLE p):h(p){}
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
    ~Handle(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}
};
inline std::runtime_error error(const char* text) {
    return std::runtime_error(std::string(text)+" (Windows error "+std::to_string(GetLastError())+")");
}
inline fs::path full_path(const fs::path& p) {return fs::absolute(p).lexically_normal();}
inline std::wstring native(const fs::path& input) {
    auto s=full_path(input).wstring();
    if(s.rfind(L"\\\\?\\",0)==0)return s;
    if(s.rfind(L"\\\\",0)==0)return L"\\\\?\\UNC\\"+s.substr(2);
    return L"\\\\?\\"+s;
}
inline DWORD local_volume(const fs::path& path) {
    auto full=full_path(path).wstring();
    if(full.rfind(L"\\\\",0)==0)throw std::runtime_error("Network paths are read-only; use a verified local card copy");
    wchar_t root[32768],type[32];DWORD serial=0;
    if(!GetVolumePathNameW(native(path).c_str(),root,32768))throw error("Cannot identify filesystem");
    if(GetDriveTypeW(root)!=DRIVE_FIXED)throw std::runtime_error("Editing/output requires a local fixed NTFS volume. Network/removable locations are read-only.");
    if(!GetVolumeInformationW(root,nullptr,0,&serial,nullptr,nullptr,type,32))throw error("Cannot inspect filesystem");
    if(_wcsicmp(type,L"NTFS"))throw std::runtime_error("Only local NTFS output is supported");
    return serial;
}
inline void no_reparse(const fs::path& path) {
    for(auto p=full_path(path);!p.empty();) {
        DWORD a=GetFileAttributesW(native(p).c_str());
        if(a!=INVALID_FILE_ATTRIBUTES && (a&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("Links/junctions are not accepted for memory-card operations");
        auto up=p.parent_path();if(up==p)break;p=up;
    }
}
struct Locked {
    fs::path path;
    Handle handle;
    ps1card::Bytes data;
    DWORD attributes=0;
    BY_HANDLE_FILE_INFORMATION identity{};
    explicit Locked(const fs::path& p,bool edit=false):path(full_path(p)),handle(CreateFileW(native(p).c_str(),GENERIC_READ|(edit?DELETE:0),edit?FILE_SHARE_DELETE:0,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr)) {
        if(handle.h==INVALID_HANDLE_VALUE)throw error("Cannot open card/file exclusively. Close games and other card editors; check the path");
        no_reparse(path);
        BY_HANDLE_FILE_INFORMATION info{};
        if(!GetFileInformationByHandle(handle.h,&info))throw error("Cannot inspect open file");
        identity=info;
        if(edit && info.nNumberOfLinks!=1)throw std::runtime_error("Editing linked cards is not supported");
        attributes=info.dwFileAttributes;
        if(attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("Not a regular memory-card file");
        LARGE_INTEGER size{};
        if(!GetFileSizeEx(handle.h,&size) || size.QuadPart<0 || uint64_t(size.QuadPart)>ps1card::card_size)throw std::runtime_error("Unsupported or oversized card/save file");
        data.resize(size_t(size.QuadPart));DWORD got=0;
        if(!ReadFile(handle.h,data.data(),DWORD(data.size()),&got,nullptr) || got!=data.size())throw error("Incomplete file read");
    }
};
struct Snapshot {
    fs::path path;
    ps1card::Bytes data;
    bool readonly=false, local=false;
};
inline Snapshot read(const fs::path& p) {
    Locked file(p);bool local=false;
    try {local_volume(file.path);local=true;}catch(const std::exception&){}
    return {file.path,file.data,(file.attributes&FILE_ATTRIBUTE_READONLY)!=0,local};
}
inline std::string unique_id() {
    static std::atomic<unsigned> seq{0};FILETIME t{};GetSystemTimeAsFileTime(&t);
    return std::to_string((uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime)+"-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(++seq);
}
inline void create_verified(const fs::path& path,const ps1card::Bytes& data) {
    no_reparse(path);local_volume(path.parent_path());
    {
        Handle h(CreateFileW(native(path).c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
        if(h.h==INVALID_HANDLE_VALUE)throw error("Cannot create new file; existing files are never overwritten");
        DWORD wrote=0;
        if(!WriteFile(h.h,data.data(),DWORD(data.size()),&wrote,nullptr) || wrote!=data.size() || !FlushFileBuffers(h.h))throw error("New file could not be written and synchronized");
        LARGE_INTEGER zero{};
        if(!SetFilePointerEx(h.h,zero,nullptr,FILE_BEGIN))throw error("Cannot verify new file");
        ps1card::Bytes check(data.size());DWORD got=0;
        if(!ReadFile(h.h,check.data(),DWORD(check.size()),&got,nullptr) || got!=check.size() || check!=data)throw std::runtime_error("New file verification failed");
        HANDLE raw=h.h;h.h=INVALID_HANDLE_VALUE;if(!CloseHandle(raw))throw error("New file close failed");
    }
    Locked reopened(path);if(reopened.data!=data)throw std::runtime_error("Persisted file verification failed");
}
struct Roots {fs::path primary, secondary;};
inline Roots environment_roots() {
    const wchar_t* a=_wgetenv(L"PSX_CARD_BACKUP_PRIMARY"),*b=_wgetenv(L"PSX_CARD_BACKUP_SECONDARY");
    if(!a || !*a || !b || !*b)throw std::runtime_error("Choose two backup folders on distinct local NTFS volumes before saving.");
    return {fs::path(a)/"manager",fs::path(b)/"manager"};
}
enum class Fault {none, backup, before_publish, replace}; // deterministic failure tests only
inline std::string sha256(const ps1card::Bytes& data) {
    HCRYPTPROV provider=0;HCRYPTHASH hash=0;
    if(!CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT))throw error("Cannot initialize backup digest");
    std::array<BYTE,32> digest{};DWORD size=DWORD(digest.size());
    bool ok=CryptCreateHash(provider,CALG_SHA_256,0,0,&hash)!=0;
    if(ok)ok=CryptHashData(hash,data.data(),DWORD(data.size()),0)!=0 && CryptGetHashParam(hash,HP_HASHVAL,digest.data(),&size,0)!=0;
    if(hash)CryptDestroyHash(hash);CryptReleaseContext(provider,0);
    if(!ok || size!=32)throw error("Cannot calculate backup digest");
    static const char* hex="0123456789abcdef";std::string result;
    for(BYTE b:digest){result+=hex[b>>4];result+=hex[b&15];}return result;
}
inline void keep_verified(const fs::path& path,const ps1card::Bytes& data) {
    if(fs::exists(path)) {Locked old(path);if(old.data!=data)throw std::runtime_error("Existing backup differs; operation cancelled");}
    else create_verified(path,data);
}
inline void backups(const Roots& roots,const Locked& source,Fault fault) {
    if(fault==Fault::backup)throw std::runtime_error("Injected backup failure");
    no_reparse(roots.primary);no_reparse(roots.secondary);
    if(local_volume(roots.primary.parent_path())==local_volume(roots.secondary.parent_path()))throw std::runtime_error("Backup locations must be on two distinct local NTFS volumes");
    fs::create_directories(roots.primary);fs::create_directories(roots.secondary);
    // Only writing actions call this. Reuse the same verified content version;
    // do not create another backup for opening, refreshing, selecting or export.
    auto key=source.path.u8string();std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c){return char(std::tolower(c));});
    std::string name=sha256(source.data)+"-"+sha256(ps1card::Bytes(key.begin(),key.end())).substr(0,16)+(source.data.size()==ps1card::card_size?".mcd":".mcs");
    keep_verified(roots.primary/name,source.data);keep_verified(roots.secondary/name,source.data);
    std::string provenance="Source: "+source.path.u8string()+"\nSHA-256: "+sha256(source.data)+"\nCreated (UTC FILETIME/PID/sequence): "+unique_id()+"\nFull byte verification: passed on both storage volumes\n";
    ps1card::Bytes note(provenance.begin(),provenance.end());
    for(const auto& root:{roots.primary,roots.secondary})if(!fs::exists(root/(name+".txt")))create_verified(root/(name+".txt"),note);
}
inline void publish(const fs::path& out,const ps1card::Bytes& data,Fault fault) {
    no_reparse(out);local_volume(out.parent_path());
    auto temp=out;temp+=L".working-"+fs::u8path(unique_id()).wstring();
    create_verified(temp,data);
    if(fault==Fault::before_publish)throw std::runtime_error("Injected interruption; original untouched, verified working file retained");
    // Same-directory rename, NO REPLACE flag. Never target an existing file.
    // An interruption can leave the verified .working-* copy, never a partial card.
    if(!MoveFileExW(native(temp).c_str(),native(out).c_str(),MOVEFILE_WRITE_THROUGH))throw error("Cannot publish new file (existing destinations are never replaced)");
}
inline void export_save(const Snapshot& input,size_t save,const fs::path& output) {
    Locked lock(input.path);if(lock.data!=input.data)throw std::runtime_error("Card changed since it was opened; reopen it before exporting");
    auto card=ps1card::parse(lock.data);publish(output,ps1card::export_mcs(card,save),Fault::none);
}
inline void commit_new(const Snapshot& destination,const Snapshot* source,const ps1card::Card& edited,const fs::path& out,const Roots& roots,Fault fault=Fault::none) {
    if(!edited.editable())throw std::runtime_error("Modified card did not pass validation");
    Locked dest(destination.path);
    if(dest.data!=destination.data)throw std::runtime_error("Destination changed; reopen both cards before retrying");
    local_volume(dest.path);
    if(dest.attributes&FILE_ATTRIBUTE_READONLY)throw std::runtime_error("Read-only card: modification refused");
    std::unique_ptr<Locked> src;
    if(source) {
        src=std::make_unique<Locked>(source->path);
        if(src->data!=source->data)throw std::runtime_error("Source changed; reopen it before retrying");
    }
    // Preserve every full input card before writing even the separate result.
    backups(roots,dest,fault);
    if(src)backups(roots,*src,fault);
    publish(out,ps1card::bytes(edited),fault);
}
inline void commit_existing(const Snapshot& destination,const Snapshot* source,const ps1card::Card& edited,const Roots& roots,Fault fault=Fault::none,bool user_confirmed=false) {
    // A host must obtain confirmation before crossing the storage boundary.
    if(!user_confirmed)throw std::runtime_error("Save requires explicit user confirmation.");
    if(!edited.editable() || !ps1card::parse(destination.data).editable())throw std::runtime_error("Card structure is not safe to edit");
    local_volume(destination.path);no_reparse(destination.path);
    // Keep the old handle open: deny all data readers/writers; sharing DELETE
    // permits NTFS's atomic rename while retaining the old complete image.
    Locked dest(destination.path,true);
    if(dest.attributes&FILE_ATTRIBUTE_READONLY)throw std::runtime_error("This card is read-only");
    if(dest.data!=destination.data)throw std::runtime_error("Card changed since opening. Refresh before retrying.");
    std::unique_ptr<Locked> src;
    if(source){src=std::make_unique<Locked>(source->path);if(src->data!=source->data)throw std::runtime_error("Source changed. Refresh before copying.");}
    backups(roots,dest,fault);
    auto temp=dest.path;temp+=L".working-"+fs::u8path(unique_id()).wstring();
    create_verified(temp,ps1card::bytes(edited));
    Handle fresh(CreateFileW(native(temp).c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(fresh.h==INVALID_HANDLE_VALUE)throw error("Cannot lock the verified working card");
    if(fault==Fault::before_publish || fault==Fault::replace)throw std::runtime_error("Update stopped before commit; original working card unchanged");
    Handle probe(CreateFileW(native(dest.path).c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    BY_HANDLE_FILE_INFORMATION now{};
    if(probe.h==INVALID_HANDLE_VALUE || !GetFileInformationByHandle(probe.h,&now) || now.dwVolumeSerialNumber!=dest.identity.dwVolumeSerialNumber || now.nFileIndexHigh!=dest.identity.nFileIndexHigh || now.nFileIndexLow!=dest.identity.nFileIndexLow)
        throw std::runtime_error("The card path changed during the operation; refresh before retrying");
    auto target=native(dest.path);size_t count=offsetof(FILE_RENAME_INFO,FileName)+(target.size()+1)*sizeof(wchar_t);
    std::vector<uint8_t> buffer(count,0);auto* info=reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    info->Flags=FILE_RENAME_FLAG_REPLACE_IF_EXISTS|FILE_RENAME_FLAG_POSIX_SEMANTICS;
    info->FileNameLength=DWORD(target.size()*sizeof(wchar_t));std::memcpy(info->FileName,target.data(),info->FileNameLength);
    // No truncate or close/reopen fallback. Unsupported filesystems/Windows
    // versions fail closed, leaving the verified backup and working file.
    // SDK enum value 22 (FileRenameInfoEx); older MinGW SDK declarations hide
    // the name behind NTDDI_VERSION, although the Windows 10 ABI is unchanged.
    constexpr auto rename_ex=static_cast<FILE_INFO_BY_HANDLE_CLASS>(22);
    if(!SetFileInformationByHandle(fresh.h,rename_ex,info,DWORD(count)))throw error("Atomic update unavailable or card in use; original working card retained");
}
inline void save_as(const Snapshot& original,const ps1card::Card& edited,const fs::path& output,const Roots& roots) {
    if(!edited.editable())throw std::runtime_error("The card draft is not valid");
    local_volume(output.parent_path());no_reparse(output);
    if(fs::exists(output))throw std::runtime_error("Save Card As requires a new filename. Existing files are never overwritten.");
    Locked source(original.path);
    if(source.data!=original.data)throw std::runtime_error("Original changed on disk. Save cancelled; draft retained.");
    backups(roots,source,Fault::none);
    publish(output,ps1card::bytes(edited),Fault::none); // exclusively NEW destination
}
} // namespace cardstore

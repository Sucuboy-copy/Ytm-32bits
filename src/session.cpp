#include <windows.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <vector>
#include <set>
#include <cstdlib>
#include <mutex>
#include "ytm/session.hpp"
#include "ytm/json.hpp"
namespace ytm {
namespace { std::wstring g_cookieHeader; std::mutex g_authMutex; }
std::wstring sessionPath() {
    wchar_t base[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr,CSIDL_LOCAL_APPDATA|CSIDL_FLAG_CREATE,nullptr,SHGFP_TYPE_CURRENT,base))) return {};
    std::wstring dir=base;dir+=L"\\Ytm32";CreateDirectoryW(dir.c_str(),nullptr);return dir+L"\\session.bin";
}
bool saveSession(const std::wstring& cookie) {
    const std::wstring path=sessionPath();if(path.empty())return false;
    DATA_BLOB input{(DWORD)((cookie.size()+1)*sizeof(wchar_t)),(BYTE*)cookie.c_str()}, encrypted{};
    if(!CryptProtectData(&input,L"Ytm32 YouTube session",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&encrypted))return false;
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_HIDDEN,nullptr);DWORD written=0;
    bool ok=file!=INVALID_HANDLE_VALUE&&WriteFile(file,encrypted.pbData,encrypted.cbData,&written,nullptr)&&written==encrypted.cbData;
    if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);SecureZeroMemory(encrypted.pbData,encrypted.cbData);LocalFree(encrypted.pbData);return ok;
}
void loadSession() {
    const std::wstring path=sessionPath();if(path.empty())return;
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_HIDDEN,nullptr);if(file==INVALID_HANDLE_VALUE)return;
    DWORD size=GetFileSize(file,nullptr);if(!size||size>256*1024){CloseHandle(file);return;}
    std::vector<BYTE> bytes(size);DWORD got=0;bool ok=ReadFile(file,bytes.data(),size,&got,nullptr)&&got==size;CloseHandle(file);if(!ok)return;
    DATA_BLOB input{size,bytes.data()},plain{};
    if(CryptUnprotectData(&input,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&plain)){
        if(plain.cbData>=sizeof(wchar_t)&&plain.cbData%sizeof(wchar_t)==0){std::lock_guard<std::mutex> lock(g_authMutex);g_cookieHeader.assign((wchar_t*)plain.pbData,plain.cbData/sizeof(wchar_t));if(!g_cookieHeader.empty()&&g_cookieHeader.back()==L'\0')g_cookieHeader.pop_back();}
        SecureZeroMemory(plain.pbData,plain.cbData);LocalFree(plain.pbData);
    }
    SecureZeroMemory(bytes.data(),bytes.size());
}
std::wstring cookieValue(const std::wstring& header,const wchar_t* key) {
    size_t pos=0;while(pos<header.size()){
        while(pos<header.size()&&(header[pos]==L' '||header[pos]==L';'||header[pos]==L'\t'))++pos;
        size_t end=header.find(L';',pos);if(end==std::wstring::npos)end=header.size();size_t eq=header.find(L'=',pos);
        if(eq!=std::wstring::npos&&eq<end&&header.compare(pos,eq-pos,key)==0)return header.substr(eq+1,end-eq-1);
        pos=end+1;
    }return {};
}
bool importNetscapeCookies(const std::wstring& path,std::wstring& cookieHeader,size_t& imported) {
    imported=0;HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return false;
    DWORD size=GetFileSize(file,nullptr);if(size==INVALID_FILE_SIZE||size==0||size>4*1024*1024){CloseHandle(file);return false;}
    std::string data(size,'\0');DWORD got=0;bool ok=ReadFile(file,&data[0],size,&got,nullptr)&&got==size;CloseHandle(file);if(!ok)return false;
    FILETIME ft{};GetSystemTimeAsFileTime(&ft);ULARGE_INTEGER ticks{};ticks.LowPart=ft.dwLowDateTime;ticks.HighPart=ft.dwHighDateTime;
    const unsigned long long unixNow=(ticks.QuadPart-116444736000000000ULL)/10000000ULL;
    std::set<std::string> names;std::string result;size_t at=0;
    while(at<data.size()){
        size_t end=data.find('\n',at);if(end==std::string::npos)end=data.size();std::string line=data.substr(at,end-at);at=end+1;if(!line.empty()&&line.back()=='\r')line.pop_back();
        if(line.compare(0,10,"#HttpOnly_")==0)line.erase(0,10);else if(line.empty()||line[0]=='#')continue;
        std::vector<std::string> fields;size_t start=0;while(fields.size()<6){size_t tab=line.find('\t',start);if(tab==std::string::npos)break;fields.push_back(line.substr(start,tab-start));start=tab+1;}if(fields.size()!=6)continue;fields.push_back(line.substr(start));
        std::string domain=fields[0];while(!domain.empty()&&domain[0]=='.')domain.erase(domain.begin());
        if(domain!="youtube.com"&&(domain.size()<12||domain.compare(domain.size()-12,12,".youtube.com")!=0))continue;
        const std::string& name=fields[5];const std::string& value=fields[6];if(name.empty()||value.empty())continue;
        char* expiryEnd=nullptr;unsigned long long expires=strtoull(fields[4].c_str(),&expiryEnd,10);if(expiryEnd!=fields[4].c_str()&&expires&&expires<unixNow)continue;
        if(names.insert(name).second){if(!result.empty())result+="; ";result+=name;result+='=';result+=value;++imported;}
    }
    SecureZeroMemory(data.data(),data.size());
    if(imported==0)return false;
    cookieHeader=utf8(result);
    SecureZeroMemory(result.data(),result.size());
    return !cookieHeader.empty();
}
std::wstring makeSidHash(const std::wstring& sid,const std::wstring& origin,const wchar_t* scheme,const std::wstring& timestamp) {
    if(sid.empty())return {};
    std::wstring source=timestamp+L" "+sid+L" "+origin;int bytes=WideCharToMultiByte(CP_UTF8,0,source.c_str(),-1,nullptr,0,nullptr,nullptr);if(bytes<=1)return {};
    std::string input(bytes,'\0');WideCharToMultiByte(CP_UTF8,0,source.c_str(),-1,&input[0],bytes,nullptr,nullptr);input.pop_back();
    HCRYPTPROV provider=0;HCRYPTHASH hash=0;BYTE digest[20]{};DWORD digestSize=sizeof(digest);std::wstring result;
    if(CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT)&&CryptCreateHash(provider,CALG_SHA1,0,0,&hash)&&CryptHashData(hash,(BYTE*)input.data(),(DWORD)input.size(),0)&&CryptGetHashParam(hash,HP_HASHVAL,digest,&digestSize,0)){
        static const wchar_t hex[]=L"0123456789abcdef";result=scheme;result+=L" ";result+=timestamp;result+=L"_";for(DWORD i=0;i<digestSize;++i){result+=hex[digest[i]>>4];result+=hex[digest[i]&15];}
    }
    if(hash)CryptDestroyHash(hash);if(provider)CryptReleaseContext(provider,0);SecureZeroMemory(input.data(),input.size());SecureZeroMemory(digest,sizeof(digest));return result;
}
std::wstring authenticatedHeaders(const std::wstring& origin) {
    std::wstring cookie;{std::lock_guard<std::mutex> lock(g_authMutex);cookie=g_cookieHeader;}
    if(cookie.empty())return {};
    if(cookie.size()>=7&&_wcsnicmp(cookie.c_str(),L"Cookie:",7)==0)cookie.erase(0,7);
    for(wchar_t c:cookie)if(c==L'\r'||c==L'\n')return {};
    std::wstring sid=cookieValue(cookie,L"SAPISID");if(sid.empty())sid=cookieValue(cookie,L"__Secure-3PAPISID");
    FILETIME ft{};GetSystemTimeAsFileTime(&ft);ULARGE_INTEGER ticks{};ticks.LowPart=ft.dwLowDateTime;ticks.HighPart=ft.dwHighDateTime;
    std::wstring timestamp=std::to_wstring((ticks.QuadPart-116444736000000000ULL)/10000000ULL);
    std::wstring auth=makeSidHash(sid,origin,L"SAPISIDHASH",timestamp);
    std::wstring sid1=cookieValue(cookie,L"__Secure-1PAPISID"),sid3=cookieValue(cookie,L"__Secure-3PAPISID");
    std::wstring auth1=makeSidHash(sid1,origin,L"SAPISID1PHASH",timestamp),auth3=makeSidHash(sid3,origin,L"SAPISID3PHASH",timestamp);
    std::wstring headers=L"Cookie: "+cookie+L"\r\nX-Origin: "+origin+L"\r\nX-Goog-AuthUser: 0\r\nX-YouTube-Bootstrap-Logged-In: true\r\n";
    if(!auth.empty()||!auth1.empty()||!auth3.empty()){headers+=L"Authorization: ";if(!auth.empty()){headers+=auth;headers+=L" ";}if(!auth1.empty()){headers+=auth1;headers+=L" ";}headers+=auth3;headers+=L"\r\n";}
    return headers;
}

std::wstring sessionCookie(){std::lock_guard<std::mutex> lock(g_authMutex);return g_cookieHeader;}
bool hasSession(){std::lock_guard<std::mutex> lock(g_authMutex);return !g_cookieHeader.empty();}
void setSessionCookie(const std::wstring& value){std::lock_guard<std::mutex> lock(g_authMutex);g_cookieHeader=value;}
void clearSessionCookie(){std::lock_guard<std::mutex> lock(g_authMutex);if(!g_cookieHeader.empty())SecureZeroMemory(&g_cookieHeader[0],g_cookieHeader.size()*sizeof(wchar_t));g_cookieHeader.clear();}
}

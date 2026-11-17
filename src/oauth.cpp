#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <mutex>
#include <vector>
#include "ytm/json.hpp"
#include "ytm/oauth.hpp"

namespace ytm {
namespace {
const char* CLIENT_ID="652469312169-4lvs9bnhr9lpns9v451j5oivd81vjvu1.apps.googleusercontent.com";
const char* CLIENT_SECRET="3fTWrBJI5Uojm1TK7_iJCW5Z";
const char* DEVICE_ENDPOINT="/o/oauth2/device/code";
const char* TOKEN_ENDPOINT="/o/oauth2/token";
const char* DEVICE_GRANT="http://oauth.net/grant_type/device/1.0";
const char* SCOPE="https://www.googleapis.com/auth/youtube";
const wchar_t* VR_AGENT=L"Mozilla/5.0 (Linux; Android 12; Quest 2) AppleWebKit/537.36";
std::mutex g_mutex;
std::string g_deviceCode,g_refreshToken,g_accessToken,g_verificationUrl,g_userCode;
int g_pollSeconds=5;
long long g_accessExpiry=0;

std::string formEncode(const std::string& value){
    static const char hex[]="0123456789ABCDEF";std::string out;
    for(unsigned char c:value){if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~')out.push_back((char)c);else if(c==' ')out.push_back('+');else{out.push_back('%');out.push_back(hex[c>>4]);out.push_back(hex[c&15]);}}
    return out;
}
std::string post(const char* path,const std::string& body,DWORD& status,std::string& diagnostic){
    HINTERNET session=WinHttpOpen(VR_AGENT,WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!session){diagnostic="OAuth WinHttpOpen failed="+std::to_string(GetLastError());return {};}
    WinHttpSetTimeouts(session,5000,5000,10000,10000);
    HINTERNET connection=WinHttpConnect(session,L"www.youtube.com",INTERNET_DEFAULT_HTTPS_PORT,0);
    HINTERNET request=connection?WinHttpOpenRequest(connection,L"POST",std::wstring(path,path+strlen(path)).c_str(),nullptr,L"https://www.youtube.com/",WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE):nullptr;
    std::string response;
    if(!request)diagnostic="OAuth WinHttpOpenRequest failed="+std::to_string(GetLastError());
    else{
        const wchar_t* headers=L"Content-Type: application/x-www-form-urlencoded\r\n";
        BOOL sent=WinHttpSendRequest(request,headers,(DWORD)-1,(LPVOID)body.data(),(DWORD)body.size(),(DWORD)body.size(),0);
        BOOL received=sent&&WinHttpReceiveResponse(request,nullptr);
        if(!sent)diagnostic="OAuth send failed="+std::to_string(GetLastError());else if(!received)diagnostic="OAuth receive failed="+std::to_string(GetLastError());
        DWORD size=sizeof(status);if(received&&!WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX))diagnostic="OAuth status query failed="+std::to_string(GetLastError());
        if(received){DWORD available=0;while(WinHttpQueryDataAvailable(request,&available)&&available){if(response.size()+available>1024*1024){diagnostic="OAuth response exceeded 1 MiB";break;}size_t old=response.size();response.resize(old+available);DWORD read=0;if(!WinHttpReadData(request,&response[old],available,&read)){response.resize(old);diagnostic="OAuth read failed="+std::to_string(GetLastError());break;}response.resize(old+read);if(!read)break;}}
        WinHttpCloseHandle(request);
    }
    if(connection)WinHttpCloseHandle(connection);WinHttpCloseHandle(session);return response;
}
const JsonValue* get(const JsonValue& value,const char* key){return member(value,key);}
std::string scalar(const JsonValue& root,const char* key){const auto* value=get(root,key);return value?value->scalar:std::string();}
long long nowSeconds(){return (long long)std::time(nullptr);}
std::wstring tokenPath(){wchar_t base[MAX_PATH]{};if(FAILED(SHGetFolderPathW(nullptr,CSIDL_LOCAL_APPDATA|CSIDL_FLAG_CREATE,nullptr,SHGFP_TYPE_CURRENT,base)))return {};std::wstring dir=base;dir+=L"\\Ytm32";CreateDirectoryW(dir.c_str(),nullptr);return dir+L"\\oauth.bin";}
bool persistTokens(){
    const std::wstring path=tokenPath();if(path.empty())return false;
    std::string plain=g_refreshToken+"\n"+g_accessToken+"\n"+std::to_string(g_accessExpiry);
    DATA_BLOB input{(DWORD)plain.size(),(BYTE*)plain.data()},encrypted{};
    if(!CryptProtectData(&input,L"Ytm32 Android VR OAuth",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&encrypted)){SecureZeroMemory(plain.data(),plain.size());return false;}
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_HIDDEN,nullptr);DWORD written=0;
    bool ok=file!=INVALID_HANDLE_VALUE&&WriteFile(file,encrypted.pbData,encrypted.cbData,&written,nullptr)&&written==encrypted.cbData;
    if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);SecureZeroMemory(encrypted.pbData,encrypted.cbData);LocalFree(encrypted.pbData);SecureZeroMemory(plain.data(),plain.size());return ok;
}
OAuthDeviceInfo fail(const std::string& reason){OAuthDeviceInfo info;info.state=-1;info.message=reason;return info;}
}

OAuthDeviceInfo beginOAuthDeviceFlow(){
    std::string id="windows-8.1-x86-"+std::to_string(nowSeconds());
    std::string body="client_id="+formEncode(CLIENT_ID)+"&scope="+formEncode(SCOPE)+"&device_id="+formEncode(id)+"&device_model="+formEncode("WINDOWS_8_1_X86");
    DWORD status=0;std::string diagnostic;std::string json=post(DEVICE_ENDPOINT,body,status,diagnostic);
    SecureZeroMemory(body.data(),body.size());
    if(status!=200){return fail(diagnostic.empty()?"Device authorization HTTP "+std::to_string(status):diagnostic);}
    JsonValue root;JsonParser parser(json);if(!parser.parse(root))return fail("No se pudo interpretar la respuesta OAuth.");
    if(!scalar(root,"error").empty())return fail("OAuth: "+scalar(root,"error"));
    std::lock_guard<std::mutex> lock(g_mutex);
    g_deviceCode=scalar(root,"device_code");g_userCode=scalar(root,"user_code");g_verificationUrl=scalar(root,"verification_url");
    const std::string interval=scalar(root,"interval");g_pollSeconds=interval.empty()?5:std::max(2,atoi(interval.c_str()));
    if(g_deviceCode.empty()||g_userCode.empty()||g_verificationUrl.empty())return fail("La respuesta OAuth no incluyó el código de dispositivo.");
    OAuthDeviceInfo info;info.state=0;info.pollSeconds=g_pollSeconds;info.userCode=g_userCode;info.verificationUrl=g_verificationUrl;return info;
}

OAuthDeviceInfo pollOAuthDeviceFlow(){
    std::string code;{std::lock_guard<std::mutex> lock(g_mutex);code=g_deviceCode;}
    if(code.empty())return fail("No hay una autorización de dispositivo pendiente.");
    std::string body="client_id="+formEncode(CLIENT_ID)+"&client_secret="+formEncode(CLIENT_SECRET)+"&code="+formEncode(code)+"&grant_type="+formEncode(DEVICE_GRANT);
    DWORD status=0;std::string diagnostic;std::string json=post(TOKEN_ENDPOINT,body,status,diagnostic);SecureZeroMemory(body.data(),body.size());
    if(!diagnostic.empty())return fail(diagnostic);
    if(status==428)return OAuthDeviceInfo{0,g_pollSeconds,g_userCode,g_verificationUrl,{}};
    if(status!=200&&status!=400)return fail("OAuth token HTTP "+std::to_string(status));
    JsonValue root;JsonParser parser(json);if(!parser.parse(root))return fail("No se pudo interpretar la respuesta del token OAuth.");
    const std::string error=scalar(root,"error");
    if(error=="authorization_pending")return OAuthDeviceInfo{0,g_pollSeconds,g_userCode,g_verificationUrl,{}};
    if(error=="slow_down"){std::lock_guard<std::mutex> lock(g_mutex);g_pollSeconds+=5;return OAuthDeviceInfo{0,g_pollSeconds,g_userCode,g_verificationUrl,{}};}
    if(!error.empty()){std::lock_guard<std::mutex> lock(g_mutex);g_deviceCode.clear();return fail("OAuth: "+error);}
    const std::string access=scalar(root,"access_token"),refresh=scalar(root,"refresh_token"),expiresText=scalar(root,"expires_in");
    if(access.empty()||refresh.empty())return fail("La respuesta OAuth no entregó access_token y refresh_token.");
    {std::lock_guard<std::mutex> lock(g_mutex);g_accessToken=access;g_refreshToken=refresh;g_accessExpiry=nowSeconds()+(expiresText.empty()?3600:atoll(expiresText.c_str()));g_deviceCode.clear();g_userCode.clear();g_verificationUrl.clear();persistTokens();}
    OAuthDeviceInfo info;info.state=1;info.message="Android VR autorizado; tokens cifrados con DPAPI.";return info;
}

std::wstring oauthBearerToken(std::string& diagnostic){
    std::string refresh,access;long long expiry;
    {std::lock_guard<std::mutex> lock(g_mutex);refresh=g_refreshToken;access=g_accessToken;expiry=g_accessExpiry;}
    if(refresh.empty()){diagnostic="OAuth Bearer ausente; inicia sesión con Android VR.";return {};}
    if(!access.empty()&&expiry>nowSeconds()+60)return utf8(access);
    std::string body="client_id="+formEncode(CLIENT_ID)+"&client_secret="+formEncode(CLIENT_SECRET)+"&refresh_token="+formEncode(refresh)+"&grant_type=refresh_token";
    DWORD status=0;std::string networkDiagnostic;std::string json=post(TOKEN_ENDPOINT,body,status,networkDiagnostic);SecureZeroMemory(body.data(),body.size());
    if(!networkDiagnostic.empty()){diagnostic=networkDiagnostic;return {};}
    JsonValue root;JsonParser parser(json);if(status!=200||!parser.parse(root)){diagnostic="OAuth refresh HTTP "+std::to_string(status);return {};}
    if(!scalar(root,"error").empty()){diagnostic="OAuth refresh: "+scalar(root,"error");return {};}
    const std::string newAccess=scalar(root,"access_token"),expiresText=scalar(root,"expires_in");if(newAccess.empty()){diagnostic="OAuth refresh no devolvió access_token.";return {};}
    {std::lock_guard<std::mutex> lock(g_mutex);g_accessToken=newAccess;g_accessExpiry=nowSeconds()+(expiresText.empty()?3600:atoll(expiresText.c_str()));persistTokens();}
    return utf8(newAccess);
}

bool hasOAuthSession(){std::lock_guard<std::mutex> lock(g_mutex);return !g_refreshToken.empty();}
void loadOAuthSession(){
    const std::wstring path=tokenPath();if(path.empty())return;HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_HIDDEN,nullptr);if(file==INVALID_HANDLE_VALUE)return;
    DWORD size=GetFileSize(file,nullptr);if(!size||size>64*1024){CloseHandle(file);return;}std::vector<BYTE> bytes(size);DWORD read=0;bool ok=ReadFile(file,bytes.data(),size,&read,nullptr)&&read==size;CloseHandle(file);if(!ok)return;
    DATA_BLOB input{size,bytes.data()},plain{};if(CryptUnprotectData(&input,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&plain)){
        std::string data((char*)plain.pbData,plain.cbData);size_t a=data.find('\n'),b=a==std::string::npos?a:data.find('\n',a+1);
        if(a!=std::string::npos&&b!=std::string::npos){std::lock_guard<std::mutex> lock(g_mutex);g_refreshToken=data.substr(0,a);g_accessToken=data.substr(a+1,b-a-1);g_accessExpiry=atoll(data.substr(b+1).c_str());}
        SecureZeroMemory(plain.pbData,plain.cbData);LocalFree(plain.pbData);SecureZeroMemory(data.data(),data.size());
    }SecureZeroMemory(bytes.data(),bytes.size());
}
void clearOAuthSession(){std::lock_guard<std::mutex> lock(g_mutex);g_deviceCode.clear();g_accessToken.clear();g_refreshToken.clear();g_accessExpiry=0;const std::wstring path=tokenPath();if(!path.empty())DeleteFileW(path.c_str());}
}

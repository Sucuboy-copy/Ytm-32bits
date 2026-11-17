#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <cstdlib>
#include "ytm/innertube.hpp"
#include "ytm/json.hpp"
#include "ytm/session.hpp"
#include "ytm/oauth.hpp"
namespace ytm {
namespace {
struct ClientProfile { const wchar_t* name; const wchar_t* version; const wchar_t* agent; const wchar_t* numericId; };
const ClientProfile CLIENTS[] = {
 {L"ANDROID_VR",L"1.65.10",L"com.google.android.apps.youtube.vr.oculus/1.65.10 (Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip",L"28"},
 {L"ANDROID",L"20.10.38",L"com.google.ios.youtube/19.01.1 (iPhone9,1; U; CPU iOS 15_0_0 like Mac OS X;)",L"3"},
 {L"VISIONOS",L"0.1",L"com.google.ios.youtube/20.10.4 (iPhone16,2; U; CPU iOS 18_3_2 like Mac OS X;)",L"101"},
 {L"TVHTML5",L"7.20210224.00.00",L"Mozilla/5.0 (Linux; Android 5.1.1; AFTT Build/LVY48F; wv) AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 Chrome/49.0.2623.10",L"7"}
};
std::string narrow(const wchar_t* value){int n=WideCharToMultiByte(CP_UTF8,0,value,-1,nullptr,0,nullptr,nullptr);std::string out(n?n:0,'\0');if(n){WideCharToMultiByte(CP_UTF8,0,value,-1,&out[0],n,nullptr,nullptr);out.pop_back();}return out;}
}
std::string searchInnerTube(const std::wstring& query, std::string& log) {
    HINTERNET session = WinHttpOpen(L"Ytm32/0.1 (Windows NT)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { log += "WinHttpOpen failed; error=" + std::to_string(GetLastError()) + "\r\n"; return {}; }
    WinHttpSetTimeouts(session, 5000, 5000, 7000, 7000);
    std::string response;
    log += "Search client: WEB_REMIX 1.20250616.03.00\r\nEndpoint: music.youtube.com/youtubei/v1/search\r\n";
    std::string body = "{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\",\"clientVersion\":\"1.20250616.03.00\",\"hl\":\"es\",\"gl\":\"ES\"}},\"query\":\"" + jsonEscape(query) + "\"}";
    HINTERNET conn = WinHttpConnect(session, L"music.youtube.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"POST", L"/youtubei/v1/search?prettyPrint=false", nullptr, L"https://music.youtube.com/", WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    DWORD error = ERROR_SUCCESS, status = 0;
    if (req) {
        std::wstring headers = L"Content-Type: application/json; charset=utf-8\r\nOrigin: https://music.youtube.com\r\nReferer: https://music.youtube.com/\r\nX-YouTube-Client-Name: 67\r\nX-YouTube-Client-Version: 1.20250616.03.00\r\n";
        const std::wstring auth=authenticatedHeaders(L"https://music.youtube.com");headers+=auth;
        log+=auth.empty()?"Session headers: none\r\n":"Session headers: attached (redacted)\r\n";
        BOOL sent = WinHttpSendRequest(req, headers.c_str(), (DWORD)-1, (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);
        if (!sent) error = GetLastError();
        BOOL received = sent && WinHttpReceiveResponse(req, nullptr);
        if (sent && !received) error = GetLastError();
        DWORD statusSize = sizeof(status);
        if (received && !WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) error = GetLastError();
        log += "HTTP status: " + std::to_string(status) + "\r\n";
        if (error != ERROR_SUCCESS) log += "WinHTTP error: " + std::to_string(error) + "\r\n";
        std::string attemptBody;
        if (received) {
            DWORD available = 0;
            while (WinHttpQueryDataAvailable(req, &available) && available) {
                size_t old = attemptBody.size(); attemptBody.resize(old + available); DWORD got = 0;
                if (!WinHttpReadData(req, &attemptBody[old], available, &got)) { error = GetLastError(); attemptBody.resize(old); break; }
                attemptBody.resize(old + got); if (!got) break;
                if (attemptBody.size() > 4 * 1024 * 1024) { log += "Response truncated at 4 MiB.\r\n"; break; }
            }
            log += "Response bytes: " + std::to_string(attemptBody.size()) + "\r\n";
            if (status == 200) response = std::move(attemptBody);
            else if (!attemptBody.empty()) log += "Error response body:\r\n" + attemptBody.substr(0,60000) + "\r\n";
        }
    } else { error = GetLastError(); log += "Could not open request; WinHTTP error: " + std::to_string(error) + "\r\n"; }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    if (response.empty() && status == 200) {
        log += "Empty response body.\r\n";
    }
    WinHttpCloseHandle(session);
    return response;
}
std::string fetchVisitorData(std::string& log){
    HINTERNET session=WinHttpOpen(L"Ytm32/0.1 (Windows NT)",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!session){log+="VisitorData WinHttpOpen failed: "+std::to_string(GetLastError())+"\r\n";return {};}
    WinHttpSetTimeouts(session,5000,5000,7000,7000);
    HINTERNET conn=WinHttpConnect(session,L"www.youtube.com",INTERNET_DEFAULT_HTTPS_PORT,0);
    HINTERNET req=conn?WinHttpOpenRequest(conn,L"GET",L"/sw.js_data",nullptr,L"https://www.youtube.com/",WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE):nullptr;
    std::string body;DWORD status=0;
    if(req){
        const wchar_t* headers=L"Origin: https://www.youtube.com\r\nReferer: https://www.youtube.com/\r\n";
        BOOL received=WinHttpSendRequest(req,headers,(DWORD)-1,WINHTTP_NO_REQUEST_DATA,0,0,0)&&WinHttpReceiveResponse(req,nullptr);
        DWORD size=sizeof(status);if(received)WinHttpQueryHeaders(req,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX);
        log+="VisitorData HTTP status: "+std::to_string(status)+"\r\n";
        if(received&&status==200){DWORD available=0;while(WinHttpQueryDataAvailable(req,&available)&&available){size_t old=body.size();body.resize(old+available);DWORD got=0;if(!WinHttpReadData(req,&body[old],available,&got)){body.resize(old);break;}body.resize(old+got);if(!got||body.size()>1024*1024)break;}}
        else if(!received)log+="VisitorData WinHTTP error: "+std::to_string(GetLastError())+"\r\n";
    }else log+="VisitorData WinHttpOpenRequest error: "+std::to_string(GetLastError())+"\r\n";
    if(req)WinHttpCloseHandle(req);if(conn)WinHttpCloseHandle(conn);WinHttpCloseHandle(session);
    if(body.compare(0,5,")]}'\n")==0)body.erase(0,5);
    JsonValue root;JsonParser parser(body);if(!parser.parse(root)){log+="VisitorData JSON parse failed.\r\n";return {};}
    const JsonValue* value=arrayItem(arrayItem(arrayItem(arrayItem(arrayItem(&root,0),2),0),0),13);
    if(value&&value->kind==JsonValue::Kind::Scalar){log+="VisitorData extracted from YouTube bootstrap.\r\n";return value->scalar;}
    log+="VisitorData field missing from bootstrap.\r\n";return {};
}
std::string requestPlayer(const ClientProfile& client,const std::string& videoId,const std::string& visitorData,const std::wstring& bearer,std::string& log,DWORD& httpStatus){
    HINTERNET session=WinHttpOpen(client.agent,WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!session){log+="WinHttpOpen failed: "+std::to_string(GetLastError())+"\r\n";return {};}
    WinHttpSetTimeouts(session,5000,5000,9000,9000);
    std::string clientContext="\"clientName\":\""+narrow(client.name)+"\",\"clientVersion\":\""+narrow(client.version)+"\",\"hl\":\"en\",\"gl\":\"US\",\"userAgent\":\""+jsonEscape(client.agent)+"\"";
    const std::wstring clientName(client.name);
    if(clientName==L"ANDROID_VR")clientContext+=",\"deviceMake\":\"Oculus\",\"deviceModel\":\"Quest 3\",\"androidSdkVersion\":\"34\",\"osName\":\"Android\",\"osVersion\":\"14\"";
    else if(clientName==L"VISIONOS")clientContext+=",\"deviceMake\":\"Apple\",\"deviceModel\":\"iPhone16,2\",\"osName\":\"iPhone\",\"osVersion\":\"2.2.22N842\"";
    else if(clientName==L"ANDROID")clientContext+=",\"deviceMake\":\"Apple\",\"deviceModel\":\"iPhone9,1\",\"osName\":\"iPhone\",\"osVersion\":\"15.0.0.19A346\"";
    else clientContext+=",\"platform\":\"TV\"";
    if(!visitorData.empty())clientContext+=",\"visitorData\":\""+jsonEscape(utf8(visitorData))+"\"";
    std::string body="{\"videoId\":\""+videoId+"\",\"context\":{\"client\":{"+clientContext+"}},\"playbackContext\":{\"contentPlaybackContext\":{\"html5Preference\":\"HTML5_PREF_WANTS\",\"signatureTimestamp\":0}},\"contentCheckOk\":true,\"racyCheckOk\":true}";
    HINTERNET conn=WinHttpConnect(session,L"m.youtube.com",INTERNET_DEFAULT_HTTPS_PORT,0);
    HINTERNET req=conn?WinHttpOpenRequest(conn,L"POST",L"/youtubei/v1/player?key=AIzaSyAO_F4S2JSlqU8Q4STEHLGCilw_Y9_11qcW8&prettyPrint=false",nullptr,L"https://m.youtube.com/",WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE):nullptr;
    std::string response;
    if(req){
        std::wstring headers=L"Content-Type: application/json; charset=utf-8\r\nX-YouTube-Client-Name: ";headers+=client.numericId;headers+=L"\r\nX-YouTube-Client-Version: ";headers+=client.version;headers+=L"\r\nUser-Agent: ";headers+=client.agent;headers+=L"\r\n";headers+=L"Origin: https://m.youtube.com\r\nReferer: https://m.youtube.com/\r\n";
        std::wstring auth;
        if(clientName==L"ANDROID_VR"&&!bearer.empty()){auth=L"Authorization: Bearer "+bearer+L"\r\n";log+="Android VR OAuth Bearer: attached (redacted)\r\n";}
        else{auth=authenticatedHeaders(L"https://m.youtube.com");log+=auth.empty()?"Session headers: none\r\n":"Session headers: attached (redacted)\r\n";}
        headers+=auth;
        BOOL sent=WinHttpSendRequest(req,headers.c_str(),(DWORD)-1,(LPVOID)body.data(),(DWORD)body.size(),(DWORD)body.size(),0);
        BOOL received=sent&&WinHttpReceiveResponse(req,nullptr);if(!sent)log+="WinHttpSendRequest error: "+std::to_string(GetLastError())+"\r\n";else if(!received)log+="WinHttpReceiveResponse error: "+std::to_string(GetLastError())+"\r\n";
        DWORD size=sizeof(httpStatus);if(received&&!WinHttpQueryHeaders(req,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&httpStatus,&size,WINHTTP_NO_HEADER_INDEX))log+="Status query error: "+std::to_string(GetLastError())+"\r\n";
        log+="Player HTTP status: "+std::to_string(httpStatus)+"\r\n";
        if(received){DWORD available=0;while(WinHttpQueryDataAvailable(req,&available)&&available){size_t old=response.size();response.resize(old+available);DWORD got=0;if(!WinHttpReadData(req,&response[old],available,&got)){log+="WinHttpReadData error: "+std::to_string(GetLastError())+"\r\n";response.resize(old);break;}response.resize(old+got);if(!got)break;if(response.size()>2*1024*1024){response.resize(2*1024*1024);log+="Player JSON clipped at 2 MiB.\r\n";break;}}}
    }else log+="WinHttpOpenRequest error: "+std::to_string(GetLastError())+"\r\n";
    if(req)WinHttpCloseHandle(req);if(conn)WinHttpCloseHandle(conn);WinHttpCloseHandle(session);return response;
}
std::vector<AudioFormat> resolveAudio(const std::string& videoId,std::string visitorData,std::string& log,bool& loginRequired){
    std::vector<AudioFormat> all;
    loginRequired=false;
    if(visitorData.empty())visitorData=fetchVisitorData(log);
    std::string oauthDiagnostic;const std::wstring bearer=oauthBearerToken(oauthDiagnostic);
    if(!bearer.empty())log+="Android VR OAuth token ready.\r\n";else if(!oauthDiagnostic.empty())log+=oauthDiagnostic+"\r\n";
    for(const auto& client:CLIENTS){
        if(std::wstring(client.name)==L"ANDROID_VR"&&bearer.empty()){log+="FourthTube auth flow uses ANDROID_VR only with OAuth; skipped without a Bearer token.\r\n";continue;}
        log+="Player client: "+narrow(client.name)+" "+narrow(client.version)+"\r\n";DWORD status=0;std::string json=requestPlayer(client,videoId,visitorData,bearer,log,status);if(json.empty())continue;
        std::vector<AudioFormat> found=audioFormatsFromPlayerResponse(json,log,loginRequired);
        if(!found.empty()){for(const auto& f:found){log+="Direct audio format available: itag "+std::to_string(f.itag)+"\r\n";all.push_back(f);}continue;}
        log+="No direct URL for audio itag 251/140 in this response.\r\n";
    }
    return all;
}

}

#include <windows.h>
#include <commctrl.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commdlg.h>
#include <shlwapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <string>
#include <vector>
#include <thread>
#include <memory>
#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <set>
#include "ytm/json.hpp"
#include "ytm/innertube.hpp"
#include "ytm/session.hpp"
#include "ytm/oauth.hpp"
#include "ytm/opus_decoder.hpp"
#include "ytm/memory_stream.hpp"

#pragma comment(lib, "winhttp.lib")

#ifdef __MINGW32__
// Las cabeceras de MinGW-w64 no declaran esta función de mfplat.dll (sí existe en libmfplat.a).
extern "C" HRESULT WINAPI MFCreateMFByteStreamOnStream(IStream* stream, IMFByteStream** byteStream);
#endif

namespace {
using namespace ytm;
constexpr UINT WM_SEARCH_DONE = WM_APP + 1;
constexpr UINT WM_PLAYBACK_READY = WM_APP + 2, WM_MEDIA_ENGINE_EVENT = WM_APP + 3;
constexpr UINT WM_OAUTH_BEGIN_DONE = WM_APP + 4, WM_OAUTH_POLL_DONE = WM_APP + 5;
constexpr UINT WM_OPUS_DECODE_DONE = WM_APP + 6;
constexpr int ID_QUERY = 101, ID_SEARCH = 102, ID_RESULTS = 103, ID_STATUS = 104, ID_LOG = 105, ID_PLAY = 106, ID_PAUSE = 107, ID_LOGIN = 108;
constexpr int ID_COOKIE = 201, ID_OPEN_BROWSER = 202, ID_SAVE_SESSION = 203, ID_CLEAR_SESSION = 204, ID_CANCEL_SESSION = 205, ID_IMPORT_COOKIES = 206;
constexpr int ID_OAUTH_BEGIN = 207, ID_OAUTH_VERIFY = 208;
HWND g_window, g_query, g_results, g_status, g_search, g_logButton, g_loginButton, g_logWindow, g_logEdit, g_authWindow, g_cookieEdit, g_playButton, g_pauseButton;
HWND g_oauthCode, g_authStatus;
HFONT g_font, g_heading;
COLORREF const BG = RGB(17, 19, 25), PANEL = RGB(25, 28, 36), TEXT = RGB(238, 240, 245), MUTED = RGB(155, 163, 178), ACCENT = RGB(255, 75, 93);
std::wstring g_lastLog;
std::string g_searchVisitorData;
struct SearchResultPayload { std::string body, log, visitorData; };
struct PlaybackPayload { DWORD requestToken = 0; bool loginRequired = false; std::wstring title; std::vector<AudioFormat> formats; std::string log; };
struct MediaEventPayload { DWORD_PTR param1; DWORD param2; };
std::vector<YouTubeVideoResult> g_searchResults;
std::vector<AudioFormat> g_activeFormats;
size_t g_activeFormat = 0;
DWORD g_playRequestToken = 0;
LONG g_oauthPollBusy = 0;
std::wstring g_oauthVerificationUrl;
struct OAuthPayload { ytm::OAuthDeviceInfo info; };
struct OpusDecodePayload { DWORD requestToken = 0; size_t formatIndex = 0; bool success = false; std::shared_ptr<std::vector<uint8_t>> wav = std::make_shared<std::vector<uint8_t>>(); std::string diagnostic; };
bool g_mfStarted = false, g_comInitialized = false;
IMFMediaEngine* g_mediaEngine = nullptr;
class MediaEngineCallback;
MediaEngineCallback* g_mediaCallback = nullptr;

void refreshLog() {
    if (g_logEdit) { SetWindowText(g_logEdit, g_lastLog.c_str()); SendMessage(g_logEdit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1); SendMessage(g_logEdit, EM_SCROLLCARET, 0, 0); }
    if (g_logButton) SetWindowText(g_logButton, g_lastLog.empty() ? L"Registro" : L"Registro •");
}
void addLog(const std::wstring& line) {
    SYSTEMTIME now{}; GetLocalTime(&now);
    wchar_t stamp[24]{}; wsprintf(stamp, L"[%02u:%02u:%02u] ", now.wHour, now.wMinute, now.wSecond);
    g_lastLog += stamp; g_lastLog += line; g_lastLog += L"\r\n";
    refreshLog();
}
LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    static WNDPROC old = nullptr;
    if (!old) old = (WNDPROC)GetWindowLongPtr(h, GWLP_USERDATA);
    if (m == WM_KEYDOWN && w == VK_RETURN) { SendMessage(g_window, WM_COMMAND, MAKEWPARAM(ID_SEARCH, BN_CLICKED), 0); return 0; }
    return CallWindowProc(old, h, m, w, l);
}
class MediaEngineCallback final : public IMFMediaEngineNotify {
    volatile LONG refs=1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        static const GUID callbackId={0xfee7c112,0xe776,0x42b5,{0x9b,0xbf,0x00,0x48,0x52,0x4e,0x2b,0xd5}};
        if(IsEqualIID(riid,IID_IUnknown)||IsEqualIID(riid,callbackId)){*out=static_cast<IMFMediaEngineNotify*>(this);AddRef();return S_OK;}
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return (ULONG)InterlockedIncrement(&refs);}
    ULONG STDMETHODCALLTYPE Release() override{LONG n=InterlockedDecrement(&refs);if(!n)delete this;return (ULONG)n;}
    HRESULT STDMETHODCALLTYPE EventNotify(DWORD event,DWORD_PTR param1,DWORD param2) override {
        auto* payload=new(std::nothrow) MediaEventPayload{param1,param2};
        if(payload&&!PostMessage(g_window,WM_MEDIA_ENGINE_EVENT,(WPARAM)event,(LPARAM)payload))delete payload;
        return S_OK;
    }
};
bool initializeMediaPlayer(){
    if(g_mediaEngine)return true;
    HRESULT hr=MFStartup(MF_VERSION,MFSTARTUP_FULL);if(FAILED(hr)){g_lastLog+=L"\r\nMFStartup failed: "+std::to_wstring((unsigned long)hr);return false;}g_mfStarted=true;
    g_mediaCallback=new(std::nothrow) MediaEngineCallback();if(!g_mediaCallback){MFShutdown();g_mfStarted=false;return false;}
    IMFAttributes* attributes=nullptr;hr=MFCreateAttributes(&attributes,2);
    if(SUCCEEDED(hr)){
        static const GUID callbackKey={0xc60381b8,0x83a4,0x41f8,{0xa3,0xd0,0xde,0x05,0x07,0x68,0x49,0xa9}};
        hr=attributes->SetUnknown(callbackKey,g_mediaCallback);
        if(SUCCEEDED(hr)){
            static const GUID factoryClsid={0xb44392da,0x499b,0x446b,{0xa4,0xcb,0x00,0x5f,0xea,0xd0,0xe6,0xd5}};
            static const GUID factoryIid={0x4d645ace,0x26aa,0x4688,{0x9b,0xe1,0xdf,0x35,0x16,0x99,0x0b,0x93}};
            IMFMediaEngineClassFactory* factory=nullptr;
            hr=CoCreateInstance(factoryClsid,nullptr,CLSCTX_INPROC_SERVER,factoryIid,(void**)&factory);
            if(SUCCEEDED(hr)){hr=factory->CreateInstance(MF_MEDIA_ENGINE_AUDIOONLY,attributes,&g_mediaEngine);factory->Release();}
        }
        attributes->Release();
    }
    if(FAILED(hr)){addLog(L"No se pudo iniciar Media Engine. HRESULT: "+std::to_wstring((unsigned long)hr));g_mediaCallback->Release();g_mediaCallback=nullptr;MFShutdown();g_mfStarted=false;return false;}
    return true;
}
HRESULT playPcmFromMemory(const std::shared_ptr<std::vector<uint8_t>>& wav){
    if(!g_mediaEngine||!wav||wav->empty())return E_UNEXPECTED;
    static const GUID mediaEngineExIid={0x83015ead,0xb1e6,0x40d0,{0xa9,0x8a,0x37,0x14,0x5f,0xfe,0x1a,0xd1}};
    static const GUID byteStreamContentType={0xfc358289,0x3cb6,0x460c,{0xa4,0x24,0xb6,0x68,0x12,0x60,0x37,0x5a}};
    static const GUID byteStreamOriginName={0xfc358288,0x3cb6,0x460c,{0xa4,0x24,0xb6,0x68,0x12,0x60,0x37,0x5a}};
    IStream* stream=new(std::nothrow) SharedMemoryStream(wav);if(!stream)return E_OUTOFMEMORY;
    IMFByteStream* byteStream=nullptr;HRESULT hr=MFCreateMFByteStreamOnStream(stream,&byteStream);stream->Release();
    if(FAILED(hr))return hr;
    IMFAttributes* attributes=nullptr;
    if(SUCCEEDED(byteStream->QueryInterface(IID_PPV_ARGS(&attributes)))){attributes->SetString(byteStreamContentType,L"audio/wav");attributes->SetString(byteStreamOriginName,L"ytm-opus.wav");attributes->Release();}
    IMFMediaEngineEx* engine=nullptr;hr=g_mediaEngine->QueryInterface(mediaEngineExIid,(void**)&engine);
    if(SUCCEEDED(hr)){BSTR name=SysAllocString(L"ytm-opus.wav");hr=name?engine->SetSourceFromByteStream(byteStream,name):E_OUTOFMEMORY;if(name)SysFreeString(name);engine->Release();}
    byteStream->Release();
    if(SUCCEEDED(hr))hr=g_mediaEngine->Play();
    return hr;
}
void loadFormat(size_t index){
    if(!g_mediaEngine||index>=g_activeFormats.size())return;
    g_activeFormat=index;
    if(g_activeFormats[index].itag==251){
        const DWORD requestToken=g_playRequestToken;const std::string url=g_activeFormats[index].url;
        SetWindowText(g_status,L"Decodificando WebM/Opus…");addLog(L"Descargando y decodificando itag 251 con libopus (en memoria).");
        std::thread([requestToken,index,url]{auto* p=new OpusDecodePayload();p->requestToken=requestToken;p->formatIndex=index;p->success=decodeWebmOpusToWav(url,*p->wav,p->diagnostic);if(!PostMessage(g_window,WM_OPUS_DECODE_DONE,0,(LPARAM)p))delete p;}).detach();return;
    }
    std::wstring url=utf8(g_activeFormats[index].url);
    BSTR source=SysAllocString(url.c_str());
    HRESULT hr=source?g_mediaEngine->SetSource(source):E_OUTOFMEMORY;
    if(source)SysFreeString(source);
    if(SUCCEEDED(hr))hr=g_mediaEngine->Play();
    if(FAILED(hr)){addLog(L"Media Engine no pudo abrir la URL. HRESULT: "+std::to_wstring((unsigned long)hr));SetWindowText(g_status,L"No se pudo abrir el stream. Revisa el registro.");return;}
    addLog(L"Abriendo audio itag "+std::to_wstring(g_activeFormats[index].itag));
    SetWindowText(g_status,(L"Cargando audio itag "+std::to_wstring(g_activeFormats[index].itag)+L"…").c_str());
}
void startPlayback(const YouTubeVideoResult& item){
    SetWindowText(g_status,(L"Resolviendo: "+item.display).c_str());
    addLog(L"Resolviendo reproducción: "+item.display);
    const std::string videoId=item.videoId;const std::wstring title=item.display;const DWORD requestToken=++g_playRequestToken;const std::string visitorData=g_searchVisitorData;
    std::thread([videoId,title,requestToken,visitorData]{auto* payload=new PlaybackPayload();payload->requestToken=requestToken;payload->title=title;payload->formats=resolveAudio(videoId,visitorData,payload->log,payload->loginRequired);if(!PostMessage(g_window,WM_PLAYBACK_READY,(WPARAM)requestToken,(LPARAM)payload))delete payload;}).detach();
}
void startSearch() {
    wchar_t q[512]{}; GetWindowText(g_query, q, 512);
    if (!*q) { SetWindowText(g_status,L"Escribe un título, artista o consulta para buscar."); SetFocus(g_query); addLog(L"Búsqueda cancelada: el campo está vacío."); return; }
    EnableWindow(g_search, FALSE); EnableWindow(g_playButton,FALSE); SetWindowText(g_status, L"Buscando en YouTube…");
    g_lastLog.clear(); addLog(L"Buscando videos de YouTube: "+std::wstring(q));
    std::wstring query(q);
    std::thread([query] {
        std::string log;
        auto body = searchInnerTube(query, log);
        std::string visitorData=visitorDataFromJson(body);
        auto* payload = new SearchResultPayload{std::move(body), std::move(log), std::move(visitorData)};
        if (!PostMessage(g_window, WM_SEARCH_DONE, 0, (LPARAM)payload)) delete payload;
    }).detach();
}
LRESULT CALLBACK logProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        g_logEdit = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"Todavía no hay actividad. Las búsquedas, respuestas y errores aparecerán aquí.", WS_CHILD|WS_VISIBLE|WS_VSCROLL|WS_HSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_AUTOHSCROLL|ES_READONLY, 0,0,700,450,h,nullptr,nullptr,nullptr);
        SendMessage(g_logEdit, WM_SETFONT, (WPARAM)g_font, TRUE); return 0;
    case WM_SIZE: MoveWindow(g_logEdit, 8, 8, LOWORD(l)-16, HIWORD(l)-16, TRUE); return 0;
    case WM_CLOSE: ShowWindow(h, SW_HIDE); return 0;
    }
    return DefWindowProc(h,m,w,l);
}
void closeAuthWindow() {
    if(g_authWindow){KillTimer(g_authWindow,1);DestroyWindow(g_authWindow);g_authWindow=nullptr;g_cookieEdit=nullptr;g_oauthCode=nullptr;g_authStatus=nullptr;EnableWindow(g_window,TRUE);SetForegroundWindow(g_window);}
}
LRESULT CALLBACK authProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch(m){
    case WM_CREATE:{
        CreateWindowEx(0,L"STATIC",L"Android VR usa OAuth de dispositivo, igual que FourthTube. Pulsa Iniciar, abre la página de verificación e introduce el código. El token de renovación se cifra con DPAPI.",WS_CHILD|WS_VISIBLE,20,18,540,58,h,nullptr,nullptr,nullptr);
        g_oauthCode=CreateWindowEx(WS_EX_CLIENTEDGE,L"EDIT",L"Pulsa Iniciar Android VR para obtener un código",WS_CHILD|WS_VISIBLE|ES_READONLY|ES_CENTER,20,84,540,34,h,nullptr,nullptr,nullptr);
        g_authStatus=CreateWindowEx(0,L"STATIC",L"Sin sesión Android VR",WS_CHILD|WS_VISIBLE,20,126,540,28,h,nullptr,nullptr,nullptr);
        CreateWindowEx(0,L"BUTTON",L"Iniciar Android VR",WS_CHILD|WS_VISIBLE|WS_TABSTOP,20,170,145,34,h,(HMENU)ID_OAUTH_BEGIN,nullptr,nullptr);
        HWND verify=CreateWindowEx(0,L"BUTTON",L"Abrir verificación",WS_CHILD|WS_VISIBLE|WS_TABSTOP,175,170,145,34,h,(HMENU)ID_OAUTH_VERIFY,nullptr,nullptr);EnableWindow(verify,FALSE);
        CreateWindowEx(0,L"BUTTON",L"Borrar sesión",WS_CHILD|WS_VISIBLE|WS_TABSTOP,330,170,105,34,h,(HMENU)ID_CLEAR_SESSION,nullptr,nullptr);
        CreateWindowEx(0,L"BUTTON",L"Cerrar",WS_CHILD|WS_VISIBLE|WS_TABSTOP,445,170,115,34,h,(HMENU)ID_CANCEL_SESSION,nullptr,nullptr);
        for(HWND child=GetWindow(h,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))SendMessage(child,WM_SETFONT,(WPARAM)g_font,TRUE);
        if(ytm::hasOAuthSession()){SetWindowText(g_authStatus,L"Sesión Android VR guardada; se renovará cuando haga falta.");SetWindowText(g_oauthCode,L"Android VR autorizado");}
        return 0;}
    case WM_COMMAND:
        if(LOWORD(w)==ID_OAUTH_BEGIN){
            EnableWindow(GetDlgItem(h,ID_OAUTH_BEGIN),FALSE);SetWindowText(g_authStatus,L"Solicitando código a YouTube…");
            std::thread([h]{auto* result=new OAuthPayload{ytm::beginOAuthDeviceFlow()};if(!PostMessage(h,WM_OAUTH_BEGIN_DONE,0,(LPARAM)result))delete result;}).detach();return 0;
        }
        if(LOWORD(w)==ID_OAUTH_VERIFY){
            if(!g_oauthVerificationUrl.empty())ShellExecuteW(h,L"open",g_oauthVerificationUrl.c_str(),nullptr,nullptr,SW_SHOWNORMAL);return 0;
        }
        if(LOWORD(w)==ID_CLEAR_SESSION){
            ytm::clearOAuthSession();ytm::clearSessionCookie();const std::wstring path=ytm::sessionPath();if(!path.empty())DeleteFileW(path.c_str());
            KillTimer(h,1);SetWindowText(g_oauthCode,L"Sesión borrada");SetWindowText(g_authStatus,L"Sin sesión Android VR");SetWindowText(g_loginButton,L"Iniciar sesión");SetWindowText(g_status,L"Sesión eliminada.");addLog(L"Sesión OAuth Android VR y cookies locales borradas.");return 0;
        }
        if(LOWORD(w)==ID_CANCEL_SESSION){closeAuthWindow();return 0;}
        break;
    case WM_TIMER:
        if(w==1&&InterlockedCompareExchange(&g_oauthPollBusy,1,0)==0){
            std::thread([h]{auto* result=new OAuthPayload{ytm::pollOAuthDeviceFlow()};if(!PostMessage(h,WM_OAUTH_POLL_DONE,0,(LPARAM)result))delete result;}).detach();
        }return 0;
    case WM_OAUTH_BEGIN_DONE:{
        std::unique_ptr<OAuthPayload> result((OAuthPayload*)l);EnableWindow(GetDlgItem(h,ID_OAUTH_BEGIN),TRUE);
        if(result->info.state<0){SetWindowText(g_authStatus,utf8(result->info.message).c_str());addLog(L"OAuth Android VR: "+utf8(result->info.message));return 0;}
        g_oauthVerificationUrl=utf8(result->info.verificationUrl);SetWindowText(g_oauthCode,utf8(result->info.userCode).c_str());SetWindowText(g_authStatus,L"Abre la verificación y escribe el código. Esperando autorización…");EnableWindow(GetDlgItem(h,ID_OAUTH_VERIFY),TRUE);
        SetTimer(h,1,(UINT)std::max(2,result->info.pollSeconds)*1000,nullptr);addLog(L"Código de autorización Android VR recibido; el código no se copia al log.");return 0;
    }
    case WM_OAUTH_POLL_DONE:{
        InterlockedExchange(&g_oauthPollBusy,0);std::unique_ptr<OAuthPayload> result((OAuthPayload*)l);
        if(result->info.state==0){SetTimer(h,1,(UINT)std::max(2,result->info.pollSeconds)*1000,nullptr);return 0;}
        KillTimer(h,1);
        if(result->info.state==1){SetWindowText(g_authStatus,L"Android VR autorizado. Reproducción usará Bearer OAuth.");SetWindowText(g_loginButton,L"Android VR listo");SetWindowText(g_status,L"Sesión Android VR lista para reproducir.");addLog(L"Autorización Android VR completada; refresh token cifrado con DPAPI.");}
        else{SetWindowText(g_authStatus,utf8(result->info.message).c_str());addLog(L"OAuth Android VR: "+utf8(result->info.message));}
        return 0;
    }
    case WM_CLOSE:closeAuthWindow();return 0;
    }
    return DefWindowProc(h,m,w,l);
}
void showAuth() {
    if(!g_authWindow){g_authWindow=CreateWindowEx(WS_EX_DLGMODALFRAME,L"Ytm32Auth",L"Iniciar sesión en YouTube Music",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU, CW_USEDEFAULT,CW_USEDEFAULT,600,265,g_window,nullptr,(HINSTANCE)GetWindowLongPtr(g_window,GWLP_HINSTANCE),nullptr);if(!g_authWindow)return;EnableWindow(g_window,FALSE);}
    ShowWindow(g_authWindow,SW_SHOW);SetForegroundWindow(g_authWindow);
}
void showLog() {
    if (!g_logWindow) g_logWindow = CreateWindowEx(WS_EX_TOOLWINDOW, L"Ytm32Log", L"Ytm32 — Log de InnerTube", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,CW_USEDEFAULT,780,560,g_window,nullptr,(HINSTANCE)GetWindowLongPtr(g_window,GWLP_HINSTANCE),nullptr);
    if (g_logEdit && !g_lastLog.empty()) SetWindowText(g_logEdit,g_lastLog.c_str());
    ShowWindow(g_logWindow,SW_SHOW); SetForegroundWindow(g_logWindow);
}
void paint(HWND h) {
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT r; GetClientRect(h, &r);
    HBRUSH b = CreateSolidBrush(BG); FillRect(dc, &r, b); DeleteObject(b);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, TEXT); SelectObject(dc, g_heading);
    RECT title{32, 26, r.right - 32, 68}; DrawText(dc, L"YT MUSIC", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, g_font); SetTextColor(dc, MUTED);
    RECT sub{33, 65, r.right - 30, 92}; DrawText(dc, L"Busca, selecciona un resultado y pulsa Reproducir", -1, &sub, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(dc, TEXT); RECT searchLabel{32, 94, r.right - 32, 110}; DrawText(dc, L"BÚSQUEDA", -1, &searchLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT resultsLabel{32, 155, r.right - 32, 174}; DrawText(dc, L"RESULTADOS  ·  doble clic también reproduce", -1, &resultsLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    EndPaint(h, &ps);
}
LRESULT CALLBACK windowProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        g_font = CreateFont(16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        g_heading = CreateFont(25,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        g_loginButton=CreateWindowEx(0,L"BUTTON",L"Iniciar sesión",WS_CHILD|WS_VISIBLE|WS_TABSTOP,480,30,120,30,h,(HMENU)ID_LOGIN,nullptr,nullptr);
        g_query = CreateWindowEx(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,32,112,430,36,h,(HMENU)ID_QUERY,nullptr,nullptr);
        g_search = CreateWindowEx(0,L"BUTTON",L"Buscar",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,472,112,108,36,h,(HMENU)ID_SEARCH,nullptr,nullptr);
        g_results = CreateWindowEx(WS_EX_CLIENTEDGE,L"LISTBOX",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|WS_TABSTOP|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,32,180,548,294,h,(HMENU)ID_RESULTS,nullptr,nullptr);
        g_status = CreateWindowEx(0,L"STATIC",L"Preparado · busca una canción o artista",WS_CHILD|WS_VISIBLE,32,510,258,28,h,(HMENU)ID_STATUS,nullptr,nullptr);
        g_playButton = CreateWindowEx(0,L"BUTTON",L"Reproducir",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,290,486,112,34,h,(HMENU)ID_PLAY,nullptr,nullptr);
        g_pauseButton = CreateWindowEx(0,L"BUTTON",L"Pausa",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,410,486,92,34,h,(HMENU)ID_PAUSE,nullptr,nullptr);
        g_logButton = CreateWindowEx(0,L"BUTTON",L"Registro",WS_CHILD|WS_VISIBLE|WS_TABSTOP,510,486,90,34,h,(HMENU)ID_LOG,nullptr,nullptr);
        EnableWindow(g_playButton,FALSE); EnableWindow(g_pauseButton,FALSE);
        for (HWND c : {g_query,g_results,g_status,g_logButton,g_loginButton,g_playButton,g_pauseButton}) SendMessage(c,WM_SETFONT,(WPARAM)g_font,TRUE);
        SetWindowLongPtr(g_query,GWLP_USERDATA,GetWindowLongPtr(g_query,GWLP_WNDPROC)); SetWindowLongPtr(g_query,GWLP_WNDPROC,(LONG_PTR)EditProc);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w)==ID_SEARCH) { if(HIWORD(w)==BN_CLICKED)startSearch(); return 0; }
        if (LOWORD(w)==ID_LOGIN) { showAuth(); return 0; }
        if (LOWORD(w)==ID_LOG) { showLog(); return 0; }
        if (LOWORD(w)==ID_PLAY) { int index=(int)SendMessage(g_results,LB_GETCURSEL,0,0);if(index>=0&&(size_t)index<g_searchResults.size())startPlayback(g_searchResults[index]);else{SetWindowText(g_status,L"Selecciona primero una canción de la lista.");addLog(L"Reproducción solicitada sin un resultado seleccionado.");}return 0; }
        if (LOWORD(w)==ID_PAUSE&&g_mediaEngine) { if(!g_mediaEngine->IsPaused()){g_mediaEngine->Pause();SetWindowText(g_pauseButton,L"Reanudar");}else{g_mediaEngine->Play();SetWindowText(g_pauseButton,L"Pausa");}return 0; }
        if (LOWORD(w)==ID_RESULTS&&HIWORD(w)==LBN_SELCHANGE) { int index=(int)SendMessage(g_results,LB_GETCURSEL,0,0);EnableWindow(g_playButton,index!=LB_ERR);if(index>=0&&(size_t)index<g_searchResults.size())SetWindowText(g_status,(L"Seleccionado · "+g_searchResults[index].display+L" · pulsa Reproducir").c_str());return 0; }
        if (LOWORD(w)==ID_RESULTS&&HIWORD(w)==LBN_DBLCLK) { int index=(int)SendMessage(g_results,LB_GETCURSEL,0,0);if(index>=0&&(size_t)index<g_searchResults.size())startPlayback(g_searchResults[index]);return 0; }
        break;
    case WM_SEARCH_DONE: {
        std::unique_ptr<SearchResultPayload> p(reinterpret_cast<SearchResultPayload*>(l));
        g_searchVisitorData=p->visitorData;
        if(!g_searchVisitorData.empty())addLog(L"VisitorData recibido en la búsqueda.");
        EnableWindow(g_search, TRUE); EnableWindow(g_playButton,FALSE); SendMessage(g_results,LB_RESETCONTENT,0,0);
        g_searchResults.clear();
        std::wstring details = utf8(p->log);
        std::vector<YouTubeVideoResult> rows;
        bool jsonParsed = false;
        if (p->body.empty()) SetWindowText(g_status,L"No hubo respuesta. Abre el log para ver el error.");
        else {
            rows = parseVideoSearchResults(p->body,jsonParsed);
            g_searchResults=rows;
            for (const auto& row : rows) SendMessage(g_results,LB_ADDSTRING,0,(LPARAM)row.display.c_str());
            std::wstring status = !jsonParsed ? L"La respuesta no contiene JSON válido. Abre Registro." : rows.empty() ? L"YouTube respondió, pero no se encontraron videos." : std::to_wstring(rows.size())+L" videos · selecciona uno y pulsa Reproducir.";
            SetWindowText(g_status,status.c_str());
            details += L"\r\nParser recognized rows: " + std::to_wstring(rows.size()) + L"\r\n";
            details += jsonParsed ? L"JSON syntax: valid\r\n" : L"JSON syntax: INVALID\r\n";
            if (rows.empty()) details += L"No result titles were extracted. Raw response follows (limited to 60,000 bytes):\r\n\r\n";
        }
        if (rows.empty()) details += L"Parser recognized rows: 0\r\n";
        if (!p->body.empty() && rows.empty() && p->body.size() <= 60000) details += L"Raw response:\r\n\r\n" + utf8(p->body);
        else if (!p->body.empty() && rows.empty()) details += L"Raw response (first 60,000 bytes):\r\n\r\n" + utf8(p->body.substr(0,60000)) + L"\r\n\r\n[Response clipped]";
        g_lastLog += details;
        addLog(L"Búsqueda terminada: "+std::to_wstring(rows.size())+L" video(s) de YouTube.");
        return 0;
    }
    case WM_PLAYBACK_READY: {
        std::unique_ptr<PlaybackPayload> p(reinterpret_cast<PlaybackPayload*>(l));
        if(p->requestToken!=g_playRequestToken)return 0;
        g_lastLog+=L"\r\n"+utf8(p->log);refreshLog();
        if(p->formats.empty()){if(p->loginRequired){SetWindowText(g_status,L"YouTube requiere sesión · pulsa Iniciar sesión y vuelve a intentar.");addLog(L"YouTube exige autenticación para resolver este audio.");}else{SetWindowText(g_status,L"No se encontró audio reproducible. Abre Registro para ver el motivo.");addLog(L"No se encontró una URL directa 251/140 para este resultado.");}return 0;}
        g_activeFormats=std::move(p->formats);
        if(!initializeMediaPlayer()){SetWindowText(g_status,L"No se pudo iniciar el reproductor de Windows.");return 0;}
        EnableWindow(g_pauseButton,TRUE);SetWindowText(g_pauseButton,L"Pausa");loadFormat(0);return 0;
    }
    case WM_OPUS_DECODE_DONE: {
        std::unique_ptr<OpusDecodePayload> p(reinterpret_cast<OpusDecodePayload*>(l));
        if(p->requestToken!=g_playRequestToken||p->formatIndex!=g_activeFormat)return 0;
        if(!p->success){addLog(L"No se pudo decodificar itag 251: "+utf8(p->diagnostic));if(g_activeFormat+1<g_activeFormats.size()){++g_activeFormat;addLog(L"Usando formato alternativo itag "+std::to_wstring(g_activeFormats[g_activeFormat].itag));loadFormat(g_activeFormat);}else SetWindowText(g_status,L"No se pudo decodificar el audio. Abre Registro.");return 0;}
        const HRESULT hr=playPcmFromMemory(p->wav);
        if(FAILED(hr)){addLog(L"Media Engine no pudo abrir el audio en memoria. HRESULT: "+std::to_wstring((unsigned long)hr));SetWindowText(g_status,L"No se pudo abrir el audio convertido.");return 0;}
        addLog(L"Opus decodificado a WAV PCM en memoria ("+std::to_wstring(p->wav->size()/1024)+L" KiB); reproduciendo itag 251.");SetWindowText(g_status,L"Cargando audio itag 251 (Opus)…");return 0;
    }
    case WM_MEDIA_ENGINE_EVENT: {
        const DWORD event=(DWORD)w;
        std::unique_ptr<MediaEventPayload> eventInfo(reinterpret_cast<MediaEventPayload*>(l));
        if(event==MF_MEDIA_ENGINE_EVENT_PLAYING){
            if(g_activeFormat<g_activeFormats.size()){SetWindowText(g_status,(L"Reproduciendo · audio itag "+std::to_wstring(g_activeFormats[g_activeFormat].itag)).c_str());addLog(L"Reproducción iniciada correctamente.");}
        }else if(event==MF_MEDIA_ENGINE_EVENT_ERROR){
            const DWORD_PTR p1=eventInfo?eventInfo->param1:0;const DWORD p2=eventInfo?eventInfo->param2:0;
            addLog(L"Media Engine informó error. Parámetros: "+std::to_wstring((unsigned long)p1)+L", "+std::to_wstring(p2));
            if(g_activeFormat+1<g_activeFormats.size()){
                ++g_activeFormat;addLog(L"Probando formato alternativo itag "+std::to_wstring(g_activeFormats[g_activeFormat].itag));loadFormat(g_activeFormat);
            }else{
                SetWindowText(g_status,L"Error al reproducir. Abre Registro para ver los detalles.");
            }
        }else if(event==MF_MEDIA_ENGINE_EVENT_STALLED||event==MF_MEDIA_ENGINE_EVENT_WAITING){SetWindowText(g_status,L"Esperando datos de audio…");}
        else if(event==MF_MEDIA_ENGINE_EVENT_ABORT){addLog(L"La carga del audio fue cancelada.");}
        return 0;
    }
    case WM_DRAWITEM: {
        auto* d=(DRAWITEMSTRUCT*)l; HBRUSH br=CreateSolidBrush(d->itemState&ODS_SELECTED?RGB(58,40,48):PANEL); FillRect(d->hDC,&d->rcItem,br); DeleteObject(br);
        SetBkMode(d->hDC,TRANSPARENT); SetTextColor(d->hDC,TEXT); SelectObject(d->hDC,g_font);
        if(d->CtlID==ID_SEARCH) { RECT x=d->rcItem; DrawText(d->hDC,L"Buscar",-1,&x,DT_CENTER|DT_VCENTER|DT_SINGLELINE); }
        else if(d->CtlID==ID_PLAY||d->CtlID==ID_PAUSE) { wchar_t label[32]{};GetWindowText(d->hwndItem,label,32);RECT x=d->rcItem;DrawText(d->hDC,label,-1,&x,DT_CENTER|DT_VCENTER|DT_SINGLELINE); }
        else if(d->itemID!=UINT(-1)) { wchar_t buf[512]; SendMessage(d->hwndItem,LB_GETTEXT,d->itemID,(LPARAM)buf); RECT x=d->rcItem; x.left+=12; DrawText(d->hDC,buf,-1,&x,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS); }
        return TRUE;
    }
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORSTATIC: {
        HDC dc=(HDC)w; SetBkColor(dc,PANEL); SetTextColor(dc,m==WM_CTLCOLORSTATIC?MUTED:TEXT); static HBRUSH br=CreateSolidBrush(PANEL); return (LRESULT)br;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paint(h); return 0;
    case WM_SIZE: { int width=LOWORD(l); MoveWindow(g_loginButton,width-152,29,120,30,TRUE); MoveWindow(g_query,32,112,width-180,36,TRUE); MoveWindow(g_search,width-140,112,108,36,TRUE); MoveWindow(g_results,32,180,width-64,HIWORD(l)-230,TRUE); MoveWindow(g_status,32,HIWORD(l)-38,width-392,24,TRUE); MoveWindow(g_playButton,width-360,HIWORD(l)-42,112,34,TRUE); MoveWindow(g_pauseButton,width-240,HIWORD(l)-42,92,34,TRUE); MoveWindow(g_logButton,width-140,HIWORD(l)-42,108,34,TRUE); return 0; }
    case WM_DESTROY:
        if(g_mediaEngine){g_mediaEngine->Shutdown();g_mediaEngine->Release();g_mediaEngine=nullptr;}
        if(g_mediaCallback){g_mediaCallback->Release();g_mediaCallback=nullptr;}
        if(g_mfStarted){MFShutdown();g_mfStarted=false;}
        if(g_comInitialized){CoUninitialize();g_comInitialized=false;}
        PostQuitMessage(0);return 0;
    }
    return DefWindowProc(h,m,w,l);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show) {
    ytm::loadOAuthSession();
    loadSession();
    HRESULT comResult=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(SUCCEEDED(comResult))g_comInitialized=true;
    INITCOMMONCONTROLSEX ic{sizeof(ic),ICC_STANDARD_CLASSES}; InitCommonControlsEx(&ic);
    WNDCLASSEX logClass{sizeof(logClass)}; logClass.lpfnWndProc=logProc; logClass.hInstance=instance; logClass.hCursor=LoadCursor(nullptr,IDC_ARROW); logClass.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); logClass.lpszClassName=L"Ytm32Log"; RegisterClassEx(&logClass);
    WNDCLASSEX authClass{sizeof(authClass)}; authClass.lpfnWndProc=authProc; authClass.hInstance=instance; authClass.hCursor=LoadCursor(nullptr,IDC_ARROW); authClass.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); authClass.lpszClassName=L"Ytm32Auth"; RegisterClassEx(&authClass);
    WNDCLASSEX wc{sizeof(wc)}; wc.lpfnWndProc=windowProc; wc.hInstance=instance; wc.hCursor=LoadCursor(nullptr,IDC_ARROW); wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); wc.lpszClassName=L"Ytm32Main"; RegisterClassEx(&wc);
    g_window=CreateWindowEx(0,wc.lpszClassName,L"Ytm32 — YouTube Music",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,650,590,nullptr,nullptr,instance,nullptr);
    if(g_window&& ytm::hasOAuthSession())SetWindowText(g_loginButton,L"Android VR listo");
    else if(g_window&& ytm::hasSession())SetWindowText(g_loginButton,L"Cookies listas");
    ShowWindow(g_window,show); UpdateWindow(g_window);
    MSG msg; while(GetMessage(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessage(&msg);} return (int)msg.wParam;
}

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
#include <wincodec.h>
#include <string>
#include <vector>
#include <thread>
#include <memory>
#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <set>
#include <map>
#include <atomic>
#include <cmath>
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
constexpr UINT WM_THUMBNAIL_READY = WM_APP + 7;
constexpr UINT ID_TIMER_PLAYBACK = 2;
constexpr int ID_QUERY = 101, ID_SEARCH = 102, ID_RESULTS = 103, ID_STATUS = 104, ID_LOG = 105, ID_PLAY = 106, ID_PAUSE = 107, ID_LOGIN = 108;
constexpr int ID_HOME = 109, ID_BROWSE = 110, ID_LIBRARY = 111, ID_FAVORITE = 112, ID_PREVIOUS = 113, ID_NEXT = 114, ID_OPEN_LOCAL = 115;
constexpr int ID_QUICK_BASE = 120;
constexpr int ID_COOKIE = 201, ID_OPEN_BROWSER = 202, ID_SAVE_SESSION = 203, ID_CLEAR_SESSION = 204, ID_CANCEL_SESSION = 205, ID_IMPORT_COOKIES = 206;
constexpr int ID_OAUTH_BEGIN = 207, ID_OAUTH_VERIFY = 208;
HWND g_window, g_query, g_results, g_status, g_search, g_logButton, g_loginButton, g_logWindow, g_logEdit, g_authWindow, g_cookieEdit, g_playButton, g_pauseButton;
HWND g_homeButton, g_browseButton, g_libraryButton, g_localButton, g_favoriteButton, g_previousButton, g_nextButton, g_quickButtons[4];
HWND g_oauthCode, g_authStatus;
HFONT g_font, g_heading;
COLORREF const BG = RGB(17, 19, 25), PANEL = RGB(25, 28, 36), TEXT = RGB(238, 240, 245), MUTED = RGB(155, 163, 178), ACCENT = RGB(255, 75, 93);
COLORREF const SIDEBAR = RGB(21, 23, 30), SURFACE = RGB(31, 34, 43), SOFT_ACCENT = RGB(58, 42, 52);
std::wstring g_lastLog;
std::string g_searchVisitorData;
struct SearchResultPayload { std::string body, log, visitorData; };
struct PlaybackPayload { DWORD requestToken = 0; bool loginRequired = false; std::wstring title; std::vector<AudioFormat> formats; std::string log; };
struct MediaEventPayload { DWORD_PTR param1; DWORD param2; };
std::vector<YouTubeVideoResult> g_searchResults;
std::vector<YouTubeVideoResult> g_favorites;
std::vector<YouTubeVideoResult> g_queue;
std::wstring g_nowPlaying;
int g_view = 0; // 0 home, 1 search, 2 library
size_t g_queueIndex = 0;
std::vector<AudioFormat> g_activeFormats;
size_t g_activeFormat = 0;
DWORD g_playRequestToken = 0;
LONG g_oauthPollBusy = 0;
std::wstring g_oauthVerificationUrl;
struct OAuthPayload { ytm::OAuthDeviceInfo info; };
struct OpusDecodePayload { DWORD requestToken = 0; size_t formatIndex = 0; bool success = false; std::shared_ptr<std::vector<uint8_t>> wav = std::make_shared<std::vector<uint8_t>>(); std::string diagnostic; };
struct ThumbnailPayload { std::string videoId; std::vector<unsigned char> bytes; };
std::map<std::string,HBITMAP> g_thumbnailBitmaps;
std::map<std::string,COLORREF> g_thumbnailAccent;
std::set<std::string> g_thumbnailPending;
std::string g_currentVideoId;
double g_playbackPosition=0.0,g_playbackDuration=0.0;
COLORREF g_currentAccent=ACCENT;
bool g_mfStarted = false, g_comInitialized = false;
bool g_isLocalPlayback = false;
IMFMediaEngine* g_mediaEngine = nullptr;
class MediaEngineCallback;
MediaEngineCallback* g_mediaCallback = nullptr;
void startPlayback(const YouTubeVideoResult& item);
bool initializeMediaPlayer();
void addLog(const std::wstring& line);
void requestThumbnails(const std::vector<YouTubeVideoResult>& rows);

std::wstring favoritesPath() {
    wchar_t base[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, nullptr, SHGFP_TYPE_CURRENT, base))) return {};
    std::wstring dir = base; dir += L"\\Ytm32"; CreateDirectoryW(dir.c_str(), nullptr); return dir + L"\\favorites.txt";
}
std::string utf8Bytes(const std::wstring& value) {
    if (value.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), (int)value.size(), nullptr, 0, nullptr, nullptr);
    std::string result(count, '\0');
    if (count) WideCharToMultiByte(CP_UTF8, 0, value.c_str(), (int)value.size(), &result[0], count, nullptr, nullptr);
    return result;
}
void saveFavorites() {
    const std::wstring path = favoritesPath(); if (path.empty()) return;
    std::string data;
    for (const auto& item : g_favorites) data += item.videoId + "\t" + utf8Bytes(item.display) + "\t" + utf8Bytes(item.thumbnailUrl) + "\n";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) { DWORD written = 0; WriteFile(file, data.data(), (DWORD)data.size(), &written, nullptr); CloseHandle(file); }
}
void loadFavorites() {
    const std::wstring path = favoritesPath(); if (path.empty()) return;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(file, nullptr); if (!size || size > 1024 * 1024) { CloseHandle(file); return; }
    std::string data(size, '\0'); DWORD got = 0; bool ok = ReadFile(file, &data[0], size, &got, nullptr) && got == size; CloseHandle(file); if (!ok) return;
    size_t at = 0;
    while (at < data.size()) {
        size_t end = data.find('\n', at); if (end == std::string::npos) end = data.size();
        std::string line = data.substr(at, end - at); at = end + 1;
        size_t tab = line.find('\t'); if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size()) continue;
        size_t secondTab=line.find('\t',tab+1);
        std::string id = line.substr(0, tab); std::wstring title = utf8(line.substr(tab + 1,secondTab==std::string::npos?std::string::npos:secondTab-tab-1));
        std::wstring thumbnail=secondTab==std::string::npos?L"":utf8(line.substr(secondTab+1));
        if (title.empty()) continue;
        g_favorites.push_back({std::move(title), std::move(id),std::move(thumbnail)});
    }
}
const std::vector<YouTubeVideoResult>& visibleRows() { return g_view == 2 ? g_favorites : g_searchResults; }
void refreshRows() {
    if (!g_results) return;
    SendMessage(g_results, LB_RESETCONTENT, 0, 0);
    const auto& rows = visibleRows();
    for (const auto& row : rows) SendMessage(g_results, LB_ADDSTRING, 0, (LPARAM)row.display.c_str());
    ShowWindow(g_results, g_view == 2 || (g_view == 1 && !rows.empty()) ? SW_SHOW : SW_HIDE);
    EnableWindow(g_favoriteButton, g_view != 0 && SendMessage(g_results, LB_GETCOUNT, 0, 0) > 0);
    InvalidateRect(g_results,nullptr,FALSE);
    if(g_window)InvalidateRect(g_window, nullptr, FALSE);
}
void setView(int view) {
    g_view = view;
    ShowWindow(g_query, view == 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_search, view == 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_results, view == 2 || (view == 1 && !g_searchResults.empty()) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_favoriteButton, view == 2 || view == 1 ? SW_SHOW : SW_HIDE);
    for(HWND button:g_quickButtons)ShowWindow(button,view==0?SW_SHOW:SW_HIDE);
    ShowWindow(g_status,SW_HIDE);
    InvalidateRect(g_homeButton,nullptr,TRUE);InvalidateRect(g_browseButton,nullptr,TRUE);InvalidateRect(g_libraryButton,nullptr,TRUE);
    if (view == 2) SetWindowText(g_status, L"Tu música · favoritos guardados en este equipo");
    else if (view == 0) SetWindowText(g_status, L"Inicio · descubre algo para escuchar");
    else if (g_searchResults.empty()) SetWindowText(g_status, L"Busca canciones, artistas o álbumes");
    refreshRows();
    if(view==2)requestThumbnails(g_favorites);
}
void toggleFavorite(const YouTubeVideoResult& item) {
    const YouTubeVideoResult savedItem=item;
    auto found = std::find_if(g_favorites.begin(), g_favorites.end(), [&](const YouTubeVideoResult& saved) { return saved.videoId == savedItem.videoId; });
    if (found == g_favorites.end()) { g_favorites.push_back(savedItem); addLog(L"Añadido a Mi música: " + savedItem.display); }
    else { g_favorites.erase(found); addLog(L"Quitado de Mi música: " + savedItem.display); }
    saveFavorites(); refreshRows();
}
void playQueueOffset(int offset) {
    if (g_queue.empty()) return;
    long long next = (long long)g_queueIndex + offset;
    if (next < 0 || next >= (long long)g_queue.size()) return;
    g_queueIndex = (size_t)next;EnableWindow(g_previousButton,g_queueIndex>0);EnableWindow(g_nextButton,g_queueIndex+1<g_queue.size());startPlayback(g_queue[g_queueIndex]);
}
void playVisibleIndex(int index) {
    const auto& rows = visibleRows();
    if (index < 0 || (size_t)index >= rows.size()) return;
    g_queue = rows; g_queueIndex = (size_t)index;EnableWindow(g_previousButton,g_queueIndex>0);EnableWindow(g_nextButton,g_queueIndex+1<g_queue.size());startPlayback(g_queue[g_queueIndex]);
}
void openLocalAudio() {
    wchar_t path[32768]{};
    OPENFILENAMEW picker{};picker.lStructSize=sizeof(picker);picker.hwndOwner=g_window;picker.lpstrFilter=L"Archivos de audio\0Archivos compatibles (*.mp3;*.wav;*.wma;*.m4a;*.aac)\0*.mp3;*.wav;*.wma;*.m4a;*.aac\0Todos los archivos\0*.*\0\0";picker.lpstrFile=path;picker.nMaxFile=(DWORD)(sizeof(path)/sizeof(path[0]));picker.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_HIDEREADONLY;picker.lpstrTitle=L"Abrir archivo de audio";
    if(!GetOpenFileNameW(&picker))return;
    if(!initializeMediaPlayer()){SetWindowText(g_status,L"No se pudo iniciar el reproductor de Windows.");return;}
    wchar_t fileUrl[32768]{};DWORD capacity=(DWORD)(sizeof(fileUrl)/sizeof(fileUrl[0]));HRESULT hr=UrlCreateFromPathW(path,fileUrl,&capacity,0);
    if(FAILED(hr)){SetWindowText(g_status,L"No se pudo abrir el archivo seleccionado.");return;}
    BSTR source=SysAllocString(fileUrl);hr=source?g_mediaEngine->SetSource(source):E_OUTOFMEMORY;if(source)SysFreeString(source);if(SUCCEEDED(hr))hr=g_mediaEngine->Play();
    if(FAILED(hr)){SetWindowText(g_status,L"Windows no pudo reproducir este formato de audio.");addLog(L"Falló la reproducción del archivo local.");return;}
    g_isLocalPlayback=true;g_activeFormats.clear();g_queue.clear();g_currentVideoId.clear();g_currentAccent=ACCENT;g_playbackPosition=0;g_playbackDuration=0;g_nowPlaying=PathFindFileNameW(path);EnableWindow(g_playButton,TRUE);EnableWindow(g_previousButton,FALSE);EnableWindow(g_nextButton,FALSE);EnableWindow(g_pauseButton,TRUE);SetWindowText(g_status,L"Reproduciendo archivo local");InvalidateRect(g_window,nullptr,FALSE);addLog(L"Reproducción local: "+g_nowPlaying);
}

HBITMAP decodeThumbnailBitmap(const std::vector<unsigned char>& bytes,COLORREF& accent){
    if(bytes.empty()||bytes.size()>2*1024*1024)return nullptr;
    IWICImagingFactory* factory=nullptr;IWICStream* stream=nullptr;IWICBitmapDecoder* decoder=nullptr;IWICBitmapFrameDecode* frame=nullptr;IWICBitmapScaler* scaler=nullptr;IWICFormatConverter* converter=nullptr;
    HBITMAP bitmap=nullptr;
    HRESULT hr=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory));
    if(SUCCEEDED(hr))hr=factory->CreateStream(&stream);
    if(SUCCEEDED(hr))hr=stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()),(DWORD)bytes.size());
    if(SUCCEEDED(hr))hr=factory->CreateDecoderFromStream(stream,nullptr,WICDecodeMetadataCacheOnLoad,&decoder);
    if(SUCCEEDED(hr))hr=decoder->GetFrame(0,&frame);
    if(SUCCEEDED(hr))hr=factory->CreateBitmapScaler(&scaler);
    if(SUCCEEDED(hr))hr=scaler->Initialize(frame,96,96,WICBitmapInterpolationModeFant);
    if(SUCCEEDED(hr))hr=factory->CreateFormatConverter(&converter);
    if(SUCCEEDED(hr))hr=converter->Initialize(scaler,GUID_WICPixelFormat32bppBGR,WICBitmapDitherTypeNone,nullptr,0.0,WICBitmapPaletteTypeCustom);
    if(SUCCEEDED(hr)){
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=96;info.bmiHeader.biHeight=-96;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        void* pixels=nullptr;bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
        if(!bitmap||FAILED(converter->CopyPixels(nullptr,96*4,96*96*4,(BYTE*)pixels))){if(bitmap)DeleteObject(bitmap);bitmap=nullptr;}
        else{
            const BYTE* p=(const BYTE*)pixels;unsigned long long red=0,green=0,blue=0;for(size_t i=0;i<96u*96u;++i){blue+=p[i*4];green+=p[i*4+1];red+=p[i*4+2];}
            accent=RGB((BYTE)((red/(96*96)+255)/2),(BYTE)((green/(96*96)+255)/2),(BYTE)((blue/(96*96)+255)/2));
        }
    }
    if(converter)converter->Release();if(scaler)scaler->Release();if(frame)frame->Release();if(decoder)decoder->Release();if(stream)stream->Release();if(factory)factory->Release();return bitmap;
}
void requestThumbnails(const std::vector<YouTubeVideoResult>& rows){
    auto requests=std::make_shared<std::vector<std::pair<std::string,std::wstring>>>();
    for(const auto& row:rows){if(row.thumbnailUrl.empty()||g_thumbnailBitmaps.find(row.videoId)!=g_thumbnailBitmaps.end()||!g_thumbnailPending.insert(row.videoId).second)continue;requests->push_back({row.videoId,row.thumbnailUrl});if(requests->size()>=32)break;}
    if(requests->empty())return;
    auto cursor=std::make_shared<std::atomic<size_t>>(0);const size_t workers=std::min<size_t>(4,requests->size());
    for(size_t i=0;i<workers;++i)std::thread([requests,cursor]{for(;;){const size_t at=(*cursor)++;if(at>=requests->size())break;auto* payload=new(std::nothrow) ThumbnailPayload();if(!payload)continue;payload->videoId=(*requests)[at].first;payload->bytes=downloadThumbnail((*requests)[at].second);if(!PostMessage(g_window,WM_THUMBNAIL_READY,0,(LPARAM)payload))delete payload;}}).detach();
}

void refreshLog() {
    if (g_logEdit) { SetWindowText(g_logEdit, g_lastLog.c_str()); SendMessage(g_logEdit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1); SendMessage(g_logEdit, EM_SCROLLCARET, 0, 0); }
    if (g_logButton) SetWindowText(g_logButton, g_lastLog.empty() ? L"Registro" : L"Registro •");
}
void addLog(const std::wstring& line) {
    SYSTEMTIME now{}; GetLocalTime(&now);
    wchar_t stamp[24]{}; wsprintf(stamp, L"[%02u:%02u:%02u] ", now.wHour, now.wMinute, now.wSecond);
    g_lastLog += stamp; g_lastLog += line; g_lastLog += L"\r\n";
    refreshLog();
    if(g_window)InvalidateRect(g_window,nullptr,FALSE);
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
    g_isLocalPlayback=false;g_nowPlaying=item.display;g_currentVideoId=item.videoId;g_currentAccent=ACCENT;auto cover=g_thumbnailBitmaps.find(item.videoId);if(cover!=g_thumbnailBitmaps.end())g_currentAccent=g_thumbnailAccent[item.videoId];g_playbackPosition=0;g_playbackDuration=0;EnableWindow(g_playButton,TRUE);InvalidateRect(g_window,nullptr,FALSE);
    SetWindowText(g_status,(L"Resolviendo: "+item.display).c_str());
    addLog(L"Resolviendo reproducción: "+item.display);
    const std::string videoId=item.videoId;const std::wstring title=item.display;const DWORD requestToken=++g_playRequestToken;const std::string visitorData=g_searchVisitorData;
    std::thread([videoId,title,requestToken,visitorData]{auto* payload=new PlaybackPayload();payload->requestToken=requestToken;payload->title=title;payload->formats=resolveAudio(videoId,visitorData,payload->log,payload->loginRequired);if(!PostMessage(g_window,WM_PLAYBACK_READY,(WPARAM)requestToken,(LPARAM)payload))delete payload;}).detach();
}
void startSearch() {
    wchar_t q[512]{}; GetWindowText(g_query, q, 512);
    if (!*q) { SetWindowText(g_status,L"Escribe un título, artista o consulta para buscar."); SetFocus(g_query); addLog(L"Búsqueda cancelada: el campo está vacío."); return; }
    setView(1);
    EnableWindow(g_search, FALSE); EnableWindow(g_playButton,FALSE); SetWindowText(g_status, L"Buscando en YouTube…");
    g_lastLog.clear(); addLog(L"Buscando videos de YouTube: "+std::wstring(q));
    std::wstring query(q);
    const std::string visitorData=g_searchVisitorData;
    std::thread([query,visitorData] {
        std::string log;
        auto body = searchInnerTube(query, log, visitorData);
        std::string responseVisitorData=visitorDataFromJson(body);
        auto* payload = new SearchResultPayload{std::move(body), std::move(log), std::move(responseVisitorData)};
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
    RECT side{0,0,176,r.bottom}; HBRUSH sb=CreateSolidBrush(SIDEBAR); FillRect(dc,&side,sb); DeleteObject(sb);
    HPEN pen=CreatePen(PS_SOLID,1,RGB(46,48,58)); HGDIOBJ oldPen=SelectObject(dc,pen); MoveToEx(dc,176,0,nullptr); LineTo(dc,176,r.bottom); SelectObject(dc,oldPen); DeleteObject(pen);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, ACCENT); SelectObject(dc, g_heading);
    RECT brand{22,25,158,64}; DrawText(dc,L"♪  YTM32",-1,&brand,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    SelectObject(dc,g_font);SetTextColor(dc,MUTED);RECT brandSub{23,59,158,80};DrawText(dc,L"MUSIC PLAYER",-1,&brandSub,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    SetTextColor(dc,TEXT);SelectObject(dc,g_heading);
    RECT title{202, 34, r.right - 190, 72};
    const wchar_t* pageTitle=g_view==0?L"Buenas tardes":g_view==1?L"Explorar música":L"Mi música";
    DrawText(dc,pageTitle,-1,&title,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    SelectObject(dc,g_font);SetTextColor(dc,MUTED);
    RECT sub{204,72,r.right-190,98};
    const wchar_t* subtitle=g_view==0?L"Tu rincón para descubrir y escuchar":g_view==1?L"Busca canciones, artistas o álbumes":L"Tus favoritos, guardados en este equipo";
    DrawText(dc,subtitle,-1,&sub,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    if(g_view==0){
        SetTextColor(dc,TEXT);RECT section{204,178,r.right-36,208};DrawText(dc,L"QUICK PICKS",-1,&section,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        SetTextColor(dc,MUTED);RECT hint{204,208,r.right-36,232};DrawText(dc,L"Un toque para encontrar algo nuevo",-1,&hint,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    }else if(g_view==1){
        SetTextColor(dc,MUTED);RECT section{204,154,r.right-36,178};DrawText(dc,L"RESULTADOS  ·  doble clic para reproducir",-1,&section,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    }else{
        SetTextColor(dc,MUTED);RECT section{204,154,r.right-36,178};DrawText(dc,L"TUS CANCIONES GUARDADAS",-1,&section,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        if(g_favorites.empty()){SetTextColor(dc,TEXT);RECT empty{204,220,r.right-36,280};DrawText(dc,L"Aún no has guardado canciones. Explora y pulsa ♡ para añadir una.",-1,&empty,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);}
    }
    RECT player{176,r.bottom-94,r.right,r.bottom};HBRUSH pb=CreateSolidBrush(SURFACE);FillRect(dc,&player,pb);DeleteObject(pb);
    HPEN ppen=CreatePen(PS_SOLID,1,RGB(50,52,62));oldPen=SelectObject(dc,ppen);MoveToEx(dc,176,r.bottom-94,nullptr);LineTo(dc,r.right,r.bottom-94);SelectObject(dc,oldPen);DeleteObject(ppen);
    const int progressY=r.bottom-84,progressLeft=278,progressRight=std::max(progressLeft+40,r.right-224);
    RECT progressTrack{progressLeft,progressY,progressRight,progressY+3};HBRUSH tr=CreateSolidBrush(RGB(67,69,78));FillRect(dc,&progressTrack,tr);DeleteObject(tr);
    if(g_playbackDuration>0.0){double ratio=std::max(0.0,std::min(1.0,g_playbackPosition/g_playbackDuration));RECT filled=progressTrack;filled.right=progressLeft+(int)((progressRight-progressLeft)*ratio);HBRUSH fill=CreateSolidBrush(g_currentAccent);FillRect(dc,&filled,fill);DeleteObject(fill);}
    auto clockText=[](double value){int sec=std::isfinite(value)&&value>0?(int)value:0;wchar_t text[16]{};wsprintf(text,L"%d:%02d",sec/60,sec%60);return std::wstring(text);};
    SetTextColor(dc,MUTED);RECT elapsed{198,r.bottom-91,260,r.bottom-76};std::wstring elapsedText=clockText(g_playbackPosition);DrawText(dc,elapsedText.c_str(),-1,&elapsed,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    RECT duration{progressRight+8,r.bottom-91,r.right-190,r.bottom-76};std::wstring durationText=clockText(g_playbackDuration);DrawText(dc,durationText.c_str(),-1,&duration,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
    RECT art{196,r.bottom-67,246,r.bottom-17};auto currentArt=g_thumbnailBitmaps.find(g_currentVideoId);
    if(currentArt!=g_thumbnailBitmaps.end()){HDC mem=CreateCompatibleDC(dc);HGDIOBJ old=SelectObject(mem,currentArt->second);StretchBlt(dc,art.left,art.top,art.right-art.left,art.bottom-art.top,mem,0,0,96,96,SRCCOPY);SelectObject(mem,old);DeleteDC(mem);}
    else{HBRUSH ab=CreateSolidBrush(SOFT_ACCENT);FillRect(dc,&art,ab);DeleteObject(ab);SetTextColor(dc,g_currentAccent);RECT note=art;DrawText(dc,L"♪",-1,&note,DT_CENTER|DT_VCENTER|DT_SINGLELINE);}
    const int metadataRight=std::max(340,176+(r.right-176)/2-120);
    SetTextColor(dc,TEXT);RECT track{258,r.bottom-62,metadataRight,r.bottom-39};DrawText(dc,g_nowPlaying.empty()?L"Elige algo para reproducir":g_nowPlaying.c_str(),-1,&track,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
    SetTextColor(dc,MUTED);RECT artist{258,r.bottom-39,metadataRight,r.bottom-19};wchar_t statusText[256]{};GetWindowText(g_status,statusText,256);DrawText(dc,statusText[0]?statusText:L"YouTube Music · Reproductor ligero",-1,&artist,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
    EndPaint(h, &ps);
}
LRESULT CALLBACK windowProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        g_font = CreateFont(16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        g_heading = CreateFont(28,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        g_homeButton=CreateWindowEx(0,L"BUTTON",L"⌂   Inicio",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,12,112,152,42,h,(HMENU)ID_HOME,nullptr,nullptr);
        g_browseButton=CreateWindowEx(0,L"BUTTON",L"⌕   Explorar",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,12,158,152,42,h,(HMENU)ID_BROWSE,nullptr,nullptr);
        g_libraryButton=CreateWindowEx(0,L"BUTTON",L"♫   Mi música",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,12,204,152,42,h,(HMENU)ID_LIBRARY,nullptr,nullptr);
        g_localButton=CreateWindowEx(0,L"BUTTON",L"＋   Abrir archivo",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,12,250,152,42,h,(HMENU)ID_OPEN_LOCAL,nullptr,nullptr);
        g_loginButton=CreateWindowEx(0,L"BUTTON",L"Iniciar sesión",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,16,310,144,34,h,(HMENU)ID_LOGIN,nullptr,nullptr);
        g_logButton=CreateWindowEx(0,L"BUTTON",L"Registro",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,16,350,144,34,h,(HMENU)ID_LOG,nullptr,nullptr);
        g_query = CreateWindowEx(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_TABSTOP|ES_AUTOHSCROLL,204,112,560,36,h,(HMENU)ID_QUERY,nullptr,nullptr);
        g_search = CreateWindowEx(0,L"BUTTON",L"Buscar",WS_CHILD|WS_TABSTOP|BS_OWNERDRAW,774,112,108,36,h,(HMENU)ID_SEARCH,nullptr,nullptr);
        g_results = CreateWindowEx(0,L"LISTBOX",L"",WS_CHILD|WS_VSCROLL|WS_TABSTOP|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS,204,190,678,420,h,(HMENU)ID_RESULTS,nullptr,nullptr);
        SendMessage(g_results,LB_SETITEMHEIGHT,0,58);
        g_status = CreateWindowEx(0,L"STATIC",L"Inicio · descubre algo para escuchar",WS_CHILD|WS_VISIBLE,530,0,300,28,h,(HMENU)ID_STATUS,nullptr,nullptr);
        g_favoriteButton=CreateWindowEx(0,L"BUTTON",L"♡ Guardar favorito",WS_CHILD|WS_TABSTOP|BS_OWNERDRAW,204,146,170,34,h,(HMENU)ID_FAVORITE,nullptr,nullptr);
        g_previousButton=CreateWindowEx(0,L"BUTTON",L"|◀",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,610,0,48,42,h,(HMENU)ID_PREVIOUS,nullptr,nullptr);
        g_playButton = CreateWindowEx(0,L"BUTTON",L"▶",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,666,0,48,42,h,(HMENU)ID_PLAY,nullptr,nullptr);
        g_pauseButton = CreateWindowEx(0,L"BUTTON",L"Ⅱ",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,722,0,48,42,h,(HMENU)ID_PAUSE,nullptr,nullptr);
        g_nextButton=CreateWindowEx(0,L"BUTTON",L"▶|",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,778,0,48,42,h,(HMENU)ID_NEXT,nullptr,nullptr);
        const wchar_t* quickLabels[4]={L"Para descubrir",L"Novedades",L"Rock clásico",L"Instrumental"};
        for(int i=0;i<4;++i)g_quickButtons[i]=CreateWindowEx(0,L"BUTTON",quickLabels[i],WS_CHILD|WS_TABSTOP|BS_OWNERDRAW,204+i*170,250,158,116,h,(HMENU)(ID_QUICK_BASE+i),nullptr,nullptr);
        EnableWindow(g_playButton,FALSE); EnableWindow(g_pauseButton,FALSE);
        EnableWindow(g_previousButton,FALSE);EnableWindow(g_nextButton,FALSE);
        for (HWND c : {g_query,g_results,g_status,g_logButton,g_loginButton,g_playButton,g_pauseButton,g_homeButton,g_browseButton,g_libraryButton,g_localButton,g_favoriteButton,g_previousButton,g_nextButton}) SendMessage(c,WM_SETFONT,(WPARAM)g_font,TRUE);
        for(HWND c:g_quickButtons)SendMessage(c,WM_SETFONT,(WPARAM)g_font,TRUE);
        SetWindowLongPtr(g_query,GWLP_USERDATA,GetWindowLongPtr(g_query,GWLP_WNDPROC)); SetWindowLongPtr(g_query,GWLP_WNDPROC,(LONG_PTR)EditProc);
        loadFavorites(); setView(0);
        SetTimer(h,ID_TIMER_PLAYBACK,500,nullptr);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w)==ID_SEARCH) { if(HIWORD(w)==BN_CLICKED)startSearch(); return 0; }
        if (LOWORD(w)==ID_HOME) { setView(0); return 0; }
        if (LOWORD(w)==ID_BROWSE) { setView(1); SetFocus(g_query); return 0; }
        if (LOWORD(w)==ID_LIBRARY) { setView(2); return 0; }
        if (LOWORD(w)==ID_OPEN_LOCAL) { openLocalAudio(); return 0; }
        if (LOWORD(w)>=ID_QUICK_BASE&&LOWORD(w)<ID_QUICK_BASE+4) {
            const wchar_t* queries[4]={L"canciones para descubrir",L"nuevas canciones",L"rock clásico",L"música instrumental"};
            SetWindowText(g_query,queries[LOWORD(w)-ID_QUICK_BASE]);setView(1);startSearch();return 0;
        }
        if (LOWORD(w)==ID_LOGIN) { showAuth(); return 0; }
        if (LOWORD(w)==ID_LOG) { showLog(); return 0; }
        if (LOWORD(w)==ID_FAVORITE) { int index=(int)SendMessage(g_results,LB_GETCURSEL,0,0);const auto& rows=visibleRows();if(index>=0&&(size_t)index<rows.size())toggleFavorite(rows[index]);return 0; }
        if (LOWORD(w)==ID_PLAY) { if(!g_queue.empty())startPlayback(g_queue[g_queueIndex]);else playVisibleIndex((int)SendMessage(g_results,LB_GETCURSEL,0,0));return 0; }
        if (LOWORD(w)==ID_PAUSE&&g_mediaEngine) { if(!g_mediaEngine->IsPaused()){g_mediaEngine->Pause();SetWindowText(g_pauseButton,L"Reanudar");}else{g_mediaEngine->Play();SetWindowText(g_pauseButton,L"Pausa");}return 0; }
        if (LOWORD(w)==ID_PREVIOUS) { playQueueOffset(-1);return 0; }
        if (LOWORD(w)==ID_NEXT) { playQueueOffset(1);return 0; }
        if (LOWORD(w)==ID_RESULTS&&HIWORD(w)==LBN_SELCHANGE) {
            int index=(int)SendMessage(g_results,LB_GETCURSEL,0,0);const auto& rows=visibleRows();EnableWindow(g_playButton,index!=LB_ERR);
            if(index>=0&&(size_t)index<rows.size()){
                g_queue=rows;g_queueIndex=(size_t)index;SetWindowText(g_status,(L"Seleccionado · "+rows[index].display).c_str());
                auto fav=std::find_if(g_favorites.begin(),g_favorites.end(),[&](const YouTubeVideoResult& r){return r.videoId==rows[index].videoId;});
                SetWindowText(g_favoriteButton,fav==g_favorites.end()?L"♡ Guardar favorito":L"♥ Quitar favorito");
                EnableWindow(g_previousButton,g_queueIndex>0);EnableWindow(g_nextButton,g_queueIndex+1<g_queue.size());
            }return 0;
        }
        if (LOWORD(w)==ID_RESULTS&&HIWORD(w)==LBN_DBLCLK) { playVisibleIndex((int)SendMessage(g_results,LB_GETCURSEL,0,0));return 0; }
        break;
    case WM_TIMER:
        if(w==ID_TIMER_PLAYBACK&&g_mediaEngine){double position=g_mediaEngine->GetCurrentTime(),duration=g_mediaEngine->GetDuration();if(std::isfinite(position)&&position>=0)g_playbackPosition=position;if(std::isfinite(duration)&&duration>0)g_playbackDuration=duration;InvalidateRect(h,nullptr,FALSE);return 0;}break;
    case WM_LBUTTONDOWN: {
        const int x=LOWORD(l),y=HIWORD(l);
        RECT client{};GetClientRect(h,&client);const int playerTop=client.bottom-94,left=278,right=std::max(left+40,client.right-224);
        if(y>=playerTop&&y<=playerTop+18&&x>=left&&x<=right&&g_mediaEngine&&g_playbackDuration>0){double target=g_playbackDuration*(double)(x-left)/(double)(right-left);if(SUCCEEDED(g_mediaEngine->SetCurrentTime(target))){g_playbackPosition=target;InvalidateRect(h,nullptr,FALSE);}return 0;}break;
    }
    case WM_THUMBNAIL_READY: {
        std::unique_ptr<ThumbnailPayload> payload((ThumbnailPayload*)l);g_thumbnailPending.erase(payload->videoId);
        if(!payload->bytes.empty()){COLORREF accent=ACCENT;HBITMAP bitmap=decodeThumbnailBitmap(payload->bytes,accent);if(bitmap){auto old=g_thumbnailBitmaps.find(payload->videoId);if(old!=g_thumbnailBitmaps.end())DeleteObject(old->second);g_thumbnailBitmaps[payload->videoId]=bitmap;g_thumbnailAccent[payload->videoId]=accent;if(payload->videoId==g_currentVideoId)g_currentAccent=accent;InvalidateRect(g_results,nullptr,FALSE);InvalidateRect(h,nullptr,FALSE);}}
        return 0;
    }
    case WM_SEARCH_DONE: {
        std::unique_ptr<SearchResultPayload> p(reinterpret_cast<SearchResultPayload*>(l));
        g_searchVisitorData=p->visitorData;
        if(!g_searchVisitorData.empty())addLog(L"VisitorData recibido en la búsqueda.");
        EnableWindow(g_search, TRUE); EnableWindow(g_playButton,FALSE); EnableWindow(g_previousButton,FALSE); EnableWindow(g_nextButton,FALSE);
        g_searchResults.clear();
        refreshRows();
        std::wstring details = utf8(p->log);
        std::vector<YouTubeVideoResult> rows;
        bool jsonParsed = false;
        if (p->body.empty()) SetWindowText(g_status,L"No hubo respuesta. Abre el log para ver el error.");
        else {
            rows = parseVideoSearchResults(p->body,jsonParsed);
            g_searchResults=rows;
            refreshRows();
            requestThumbnails(rows);
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
            if(g_isLocalPlayback)SetWindowText(g_status,L"Reproduciendo archivo local");else if(g_activeFormat<g_activeFormats.size())SetWindowText(g_status,(L"Reproduciendo · audio itag "+std::to_wstring(g_activeFormats[g_activeFormat].itag)).c_str());
            InvalidateRect(g_window,nullptr,FALSE);addLog(L"Reproducción iniciada correctamente.");
        }else if(event==MF_MEDIA_ENGINE_EVENT_ERROR){
            const DWORD_PTR p1=eventInfo?eventInfo->param1:0;const DWORD p2=eventInfo?eventInfo->param2:0;
            addLog(L"Media Engine informó error. Parámetros: "+std::to_wstring((unsigned long)p1)+L", "+std::to_wstring(p2));
            if(!g_isLocalPlayback&&g_activeFormat+1<g_activeFormats.size()){
                ++g_activeFormat;addLog(L"Probando formato alternativo itag "+std::to_wstring(g_activeFormats[g_activeFormat].itag));loadFormat(g_activeFormat);
            }else{
                SetWindowText(g_status,L"Error al reproducir. Abre Registro para ver los detalles.");
            }
        }else if(event==MF_MEDIA_ENGINE_EVENT_STALLED||event==MF_MEDIA_ENGINE_EVENT_WAITING){SetWindowText(g_status,L"Esperando datos de audio…");InvalidateRect(g_window,nullptr,FALSE);}
        else if(event==MF_MEDIA_ENGINE_EVENT_ENDED){playQueueOffset(1);}
        else if(event==MF_MEDIA_ENGINE_EVENT_ABORT){addLog(L"La carga del audio fue cancelada.");}
        return 0;
    }
    case WM_DRAWITEM: {
        auto* d=(DRAWITEMSTRUCT*)l;const bool selected=(d->itemState&ODS_SELECTED)!=0;COLORREF color=selected?SOFT_ACCENT:PANEL;
        if(d->CtlID==ID_HOME&&g_view==0||d->CtlID==ID_BROWSE&&g_view==1||d->CtlID==ID_LIBRARY&&g_view==2)color=SOFT_ACCENT;
        if(d->CtlID>=ID_QUICK_BASE&&d->CtlID<ID_QUICK_BASE+4)color=selected?RGB(67,49,61):SURFACE;
        if(d->CtlID==ID_SEARCH||d->CtlID==ID_PLAY)color=ACCENT;
        HBRUSH br=CreateSolidBrush(color);FillRect(d->hDC,&d->rcItem,br);DeleteObject(br);
        SetBkMode(d->hDC,TRANSPARENT);SetTextColor(d->hDC,(d->CtlID==ID_HOME&&g_view==0||d->CtlID==ID_BROWSE&&g_view==1||d->CtlID==ID_LIBRARY&&g_view==2)?RGB(255,190,198):TEXT);SelectObject(d->hDC,g_font);
        if(d->CtlType==ODT_LISTBOX&&d->itemID!=UINT(-1)){
            wchar_t buf[512]{};SendMessage(d->hwndItem,LB_GETTEXT,d->itemID,(LPARAM)buf);RECT row=d->rcItem;row.left+=16;row.right-=12;
            const auto& rows=visibleRows();if(d->itemID<rows.size()){
                auto thumb=g_thumbnailBitmaps.find(rows[d->itemID].videoId);RECT cover{row.left-8,row.top+5,row.left+42,row.top+55};
                if(thumb!=g_thumbnailBitmaps.end()){HDC mem=CreateCompatibleDC(d->hDC);HGDIOBJ old=SelectObject(mem,thumb->second);StretchBlt(d->hDC,cover.left,cover.top,50,50,mem,0,0,96,96,SRCCOPY);SelectObject(mem,old);DeleteDC(mem);}
                else{HBRUSH placeholder=CreateSolidBrush(SOFT_ACCENT);FillRect(d->hDC,&cover,placeholder);DeleteObject(placeholder);SetTextColor(d->hDC,ACCENT);DrawText(d->hDC,L"♪",-1,&cover,DT_CENTER|DT_VCENTER|DT_SINGLELINE);}
                row.left+=58;
            }
            std::wstring line=buf;size_t split=line.find(L"  —  ");std::wstring title=split==std::wstring::npos?line:line.substr(0,split);std::wstring artist=split==std::wstring::npos?L"YouTube Music":line.substr(split+5);
            SetTextColor(d->hDC,TEXT);RECT primary=row;primary.top+=5;primary.bottom=row.top+31;DrawText(d->hDC,title.c_str(),-1,&primary,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
            SetTextColor(d->hDC,MUTED);RECT secondary=row;secondary.top+=30;DrawText(d->hDC,artist.c_str(),-1,&secondary,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
            HPEN divider=CreatePen(PS_SOLID,1,RGB(48,50,59));HGDIOBJ old=SelectObject(d->hDC,divider);MoveToEx(d->hDC,row.left,row.bottom-1,nullptr);LineTo(d->hDC,row.right,row.bottom-1);SelectObject(d->hDC,old);DeleteObject(divider);return TRUE;
        }
        if(d->CtlID>=ID_QUICK_BASE&&d->CtlID<ID_QUICK_BASE+4){
            const wchar_t* titles[4]={L"Para descubrir",L"Novedades",L"Rock clásico",L"Instrumental"};const wchar_t* icons[4]={L"✦",L"♫",L"◈",L"☼"};int i=d->CtlID-ID_QUICK_BASE;
            SetTextColor(d->hDC,ACCENT);RECT icon=d->rcItem;icon.left+=14;icon.top+=10;icon.bottom=icon.top+46;DrawText(d->hDC,icons[i],-1,&icon,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
            SetTextColor(d->hDC,TEXT);RECT title=d->rcItem;title.left+=14;title.top+=58;DrawText(d->hDC,titles[i],-1,&title,DT_LEFT|DT_VCENTER|DT_SINGLELINE);return TRUE;
        }
        wchar_t label[64]{};GetWindowText(d->hwndItem,label,64);RECT text=d->rcItem;
        if(d->CtlID==ID_HOME||d->CtlID==ID_BROWSE||d->CtlID==ID_LIBRARY||d->CtlID==ID_OPEN_LOCAL||d->CtlID==ID_LOGIN||d->CtlID==ID_LOG||d->CtlID==ID_FAVORITE){text.left+=12;DrawText(d->hDC,label,-1,&text,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);}
        else DrawText(d->hDC,label,-1,&text,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        return TRUE;
    }
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORSTATIC: {
        HDC dc=(HDC)w; SetBkColor(dc,PANEL); SetTextColor(dc,m==WM_CTLCOLORSTATIC?MUTED:TEXT); static HBRUSH br=CreateSolidBrush(PANEL); return (LRESULT)br;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paint(h); return 0;
    case WM_SIZE: {
        int width=LOWORD(l),height=HIWORD(l),content=width-220,playerTop=height-94;
        MoveWindow(g_query,204,112,std::max(180,content-128),38,TRUE);MoveWindow(g_search,width-112,112,88,38,TRUE);
        MoveWindow(g_favoriteButton,204,154,170,34,TRUE);MoveWindow(g_results,204,194,std::max(180,content),std::max(80,playerTop-210),TRUE);
        for(int i=0;i<4;++i){int gap=12,cardWidth=std::max(105,(content-gap*3)/4);MoveWindow(g_quickButtons[i],204+i*(cardWidth+gap),254,cardWidth,116,TRUE);}
        MoveWindow(g_status,204,playerTop-30,std::max(180,content-16),22,TRUE);
        int center=176+(width-176)/2;MoveWindow(g_previousButton,center-106,playerTop+26,42,42,TRUE);MoveWindow(g_playButton,center-54,playerTop+26,42,42,TRUE);MoveWindow(g_pauseButton,center+8,playerTop+26,42,42,TRUE);MoveWindow(g_nextButton,center+60,playerTop+26,42,42,TRUE);
        return 0;
    }
    case WM_GETMINMAXINFO: { auto* info=(MINMAXINFO*)l;info->ptMinTrackSize.x=780;info->ptMinTrackSize.y=560;return 0; }
    case WM_DESTROY:
        KillTimer(h,ID_TIMER_PLAYBACK);
        for(const auto& item:g_thumbnailBitmaps)DeleteObject(item.second);g_thumbnailBitmaps.clear();g_thumbnailAccent.clear();
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
    g_window=CreateWindowEx(0,wc.lpszClassName,L"Ytm32 — YouTube Music",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,1040,740,nullptr,nullptr,instance,nullptr);
    if(g_window&& ytm::hasOAuthSession())SetWindowText(g_loginButton,L"Android VR listo");
    else if(g_window&& ytm::hasSession())SetWindowText(g_loginButton,L"Cookies listas");
    ShowWindow(g_window,show); UpdateWindow(g_window);
    MSG msg; while(GetMessage(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessage(&msg);} return (int)msg.wParam;
}

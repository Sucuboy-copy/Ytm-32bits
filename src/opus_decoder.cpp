#include <windows.h>
#include <winhttp.h>
#include <opus.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <memory>
#include <new>
#include <vector>
#include "ytm/opus_decoder.hpp"

namespace ytm {
namespace {
struct Element { uint32_t id; size_t data; size_t size; size_t end; bool unknown; };
bool vint(const std::vector<uint8_t>& b,size_t& pos,uint64_t& value,unsigned& length,bool id){
    if(pos>=b.size())return false;uint8_t first=b[pos];uint8_t marker=0x80;length=1;
    while(length<=8&&(first&marker)==0){marker>>=1;++length;}if(length>8||marker==0||pos+length>b.size())return false;
    value=id?first:(uint64_t)(first&(marker-1));++pos;
    for(unsigned i=1;i<length;++i)value=(value<<8)|b[pos++];return true;
}
bool element(const std::vector<uint8_t>& b,size_t& pos,size_t parentEnd,Element& e){
    if(pos>=parentEnd||parentEnd>b.size())return false;uint64_t id=0,size=0;unsigned idLen=0,sizeLen=0;
    if(!vint(b,pos,id,idLen,true)||!vint(b,pos,size,sizeLen,false)||id>0xffffffffULL)return false;
    bool unknown=true;for(unsigned n=0;n<sizeLen*7;++n)if(((size>>(sizeLen*7-1-n))&1)==0){unknown=false;break;}
    if(unknown)size=parentEnd-pos;if(size>parentEnd-pos)return false;
    e={(uint32_t)id,pos,(size_t)size,pos+(size_t)size,unknown};return true;
}
uint64_t uintElement(const std::vector<uint8_t>& b,const Element& e){uint64_t v=0;for(size_t i=e.data;i<e.end;++i)v=(v<<8)|b[i];return v;}
double floatElement(const std::vector<uint8_t>& b,const Element& e){if(e.size==4){uint32_t v=0;for(size_t i=e.data;i<e.end;++i)v=(v<<8)|b[i];float f;std::memcpy(&f,&v,4);return f;}if(e.size==8){uint64_t v=0;for(size_t i=e.data;i<e.end;++i)v=(v<<8)|b[i];double d;std::memcpy(&d,&v,8);return d;}return 0;}
std::string stringElement(const std::vector<uint8_t>& b,const Element& e){return std::string((const char*)&b[e.data],e.size);}
struct OpusTrack { uint64_t number=0; unsigned channels=0; unsigned preSkip=0; bool found=false; };
bool parseTrack(const std::vector<uint8_t>& b,const Element& entry,OpusTrack& selected){
    uint64_t number=0,type=0;std::string codec;const uint8_t* privateData=nullptr;size_t privateSize=0;
    for(size_t p=entry.data;p<entry.end;){Element e{};if(!element(b,p,entry.end,e))return false;p=e.end;
        if(e.id==0xD7)number=uintElement(b,e);else if(e.id==0x83)type=uintElement(b,e);else if(e.id==0x86)codec=stringElement(b,e);else if(e.id==0x63A2){privateData=&b[e.data];privateSize=e.size;}}
    if(type!=2||codec!="A_OPUS"||!privateData||privateSize<19||std::memcmp(privateData,"OpusHead",8)!=0)return true;
    const unsigned channels=privateData[9];const unsigned mapping=privateData[18];if((channels!=1&&channels!=2)||mapping!=0)return true;
    selected.number=number;selected.channels=channels;selected.preSkip=(unsigned)privateData[10]|((unsigned)privateData[11]<<8);selected.found=number!=0;return true;
}
constexpr uint64_t kMaxPcmBytes=256ull*1024*1024;
bool appendBytes(std::vector<uint8_t>& out,const void* data,size_t size){const uint8_t* p=(const uint8_t*)data;out.insert(out.end(),p,p+size);return true;}
void put16(uint8_t* out,uint16_t x){out[0]=(uint8_t)x;out[1]=(uint8_t)(x>>8);}
void put32(uint8_t* out,uint32_t x){out[0]=(uint8_t)x;out[1]=(uint8_t)(x>>8);out[2]=(uint8_t)(x>>16);out[3]=(uint8_t)(x>>24);}
void initWave(uint8_t h[44],unsigned channels){std::memset(h,0,44);std::memcpy(h,"RIFF",4);std::memcpy(h+8,"WAVEfmt ",8);put32(h+16,16);put16(h+20,1);put16(h+22,(uint16_t)channels);put32(h+24,48000);put32(h+28,48000*channels*2);put16(h+32,(uint16_t)(channels*2));put16(h+34,16);std::memcpy(h+36,"data",4);}
bool readBlock(const std::vector<uint8_t>& b,size_t begin,size_t end,uint64_t wantedTrack,OpusDecoder* decoder,unsigned channels,unsigned& preSkip,std::vector<uint8_t>& wav,uint64_t& dataBytes,std::string& error){
    size_t p=begin;uint64_t track=0;unsigned trackLen=0;if(!vint(b,p,track,trackLen,false)||p+3>end)return false;
    p+=2;const uint8_t flags=b[p++];if(track!=wantedTrack)return true;
    const unsigned lace=(flags>>1)&3;std::vector<size_t> lengths;size_t payload=p;
    if(lace==0)lengths.push_back(end-p);
    else{
        if(p>=end)return false;const unsigned count=(unsigned)b[p++]+1;if(count>256)return false;
        if(lace==1){size_t sum=0;for(unsigned i=0;i+1<count;++i){size_t len=0;uint8_t x;do{if(p>=end)return false;x=b[p++];len+=x;}while(x==255);lengths.push_back(len);sum+=len;}if(sum>end-p)return false;lengths.push_back(end-p-sum);}
        else if(lace==2){if((end-p)%count)return false;lengths.assign(count,(end-p)/count);}
        else{
            uint64_t first=0;unsigned len=0;if(!vint(b,p,first,len,false))return false;lengths.push_back((size_t)first);int64_t previous=(int64_t)first;
            for(unsigned i=1;i+1<count;++i){uint64_t deltaValue=0;unsigned deltaLen=0;if(!vint(b,p,deltaValue,deltaLen,false))return false;int64_t bias=((int64_t)1<<(7*deltaLen-1))-1;int64_t current=previous+(int64_t)deltaValue-bias;if(current<0)return false;lengths.push_back((size_t)current);previous=current;}
            size_t sum=0;for(size_t n:lengths){if(sum>end-p||n>end-p-sum)return false;sum+=n;}lengths.push_back(end-p-sum);
        }
    }
    std::vector<opus_int16> pcm(5760*channels);
    for(size_t packetSize:lengths){if(packetSize>end-p)return false;int frames=opus_decode(decoder,&b[p],(opus_int32)packetSize,pcm.data(),5760,0);if(frames<0){error=opus_strerror(frames);return false;}p+=packetSize;
        const unsigned skip=std::min<unsigned>(preSkip,(unsigned)frames);preSkip-=skip;const unsigned remain=(unsigned)frames-skip;if(!remain)continue;
        const opus_int16* output=pcm.data()+(size_t)skip*channels;const size_t bytes=(size_t)remain*channels*sizeof(opus_int16);if(dataBytes+bytes>kMaxPcmBytes){error="Decoded PCM exceeds the in-memory limit.";return false;}if(!appendBytes(wav,output,bytes))return false;dataBytes+=bytes;
    }
    return p<=end;
}
constexpr uint64_t kMaxDownloadBytes=80ull*1024*1024;
constexpr uint64_t kChunkBytes=2ull*1024*1024;
uint64_t contentLengthFromUrl(const std::string& url){
    for(size_t p=url.find("clen=");p!=std::string::npos;p=url.find("clen=",p+1))if(p>0&&(url[p-1]=='?'||url[p-1]=='&'))return std::strtoull(url.c_str()+p+5,nullptr,10);
    return 0;
}
// Descarga un bloque (ya con &range=a-b en la ruta) y lo añade a data. Falla si el bloque llega incompleto.
bool fetchChunk(HINTERNET conn,const std::wstring& path,DWORD flags,std::vector<uint8_t>& data,DWORD& status,std::string& error){
    const size_t start=data.size();status=0;bool ok=false;
    HINTERNET req=WinHttpOpenRequest(conn,L"GET",path.c_str(),nullptr,L"https://www.youtube.com/",WINHTTP_DEFAULT_ACCEPT_TYPES,flags);
    if(!req){error="Opening audio request failed: "+std::to_string(GetLastError());return false;}
    if(WinHttpSendRequest(req,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)&&WinHttpReceiveResponse(req,nullptr)){
        DWORD size=sizeof(status);
        if(WinHttpQueryHeaders(req,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX)&&(status==200||status==206)){
            DWORD expected=0;size=sizeof(expected);
            const bool hasLength=WinHttpQueryHeaders(req,WINHTTP_QUERY_CONTENT_LENGTH|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&expected,&size,WINHTTP_NO_HEADER_INDEX)!=FALSE;
            ok=true;DWORD available=0;
            for(;;){
                if(!WinHttpQueryDataAvailable(req,&available)){error="Querying WebM audio data failed: "+std::to_string(GetLastError());ok=false;break;}
                if(!available)break;
                if(data.size()+available>kMaxDownloadBytes){error="WebM audio exceeds the 80 MiB safety limit.";ok=false;break;}
                const size_t old=data.size();data.resize(old+available);DWORD got=0;
                if(!WinHttpReadData(req,&data[old],available,&got)){data.resize(old);error="Reading WebM audio failed: "+std::to_string(GetLastError());ok=false;break;}
                data.resize(old+got);if(!got)break;
            }
            if(ok&&hasLength&&data.size()-start!=expected){error="Audio chunk truncated: received "+std::to_string(data.size()-start)+" of "+std::to_string(expected)+" bytes.";ok=false;}
        }else error=status?"Audio download HTTP status "+std::to_string(status):"Reading audio response headers failed: "+std::to_string(GetLastError());
    }else error="WebM audio request failed: "+std::to_string(GetLastError());
    WinHttpCloseHandle(req);if(!ok)data.resize(start);return ok;
}
// Descarga por bloques con el parámetro range de googlevideo (como yt-dlp): evita el límite de velocidad de las
// descargas de archivo completo y permite detectar cortes. Cada bloque se reintenta una vez.
bool download(const std::string& url,std::vector<uint8_t>& data,std::string& error){
    std::wstring wide(url.begin(),url.end());URL_COMPONENTS c{};c.dwStructSize=sizeof(c);wchar_t host[512]{},path[16384]{},extra[16384]{};c.lpszHostName=host;c.dwHostNameLength=512;c.lpszUrlPath=path;c.dwUrlPathLength=16384;c.lpszExtraInfo=extra;c.dwExtraInfoLength=16384;
    if(!WinHttpCrackUrl(wide.c_str(),(DWORD)wide.size(),0,&c)){error="Could not parse the signed audio URL.";return false;}
    const INTERNET_PORT port=c.nPort;const DWORD flags=c.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0;std::wstring requestPath(path,c.dwUrlPathLength);requestPath.append(extra,c.dwExtraInfoLength);
    HINTERNET session=WinHttpOpen(L"Ytm32 Opus/1.6.1",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);if(!session){error="Opus download session failed: "+std::to_string(GetLastError());return false;}WinHttpSetTimeouts(session,5000,10000,15000,15000);
    HINTERNET conn=WinHttpConnect(session,host,port,0);if(!conn){error="Opus download connection failed: "+std::to_string(GetLastError());WinHttpCloseHandle(session);return false;}
    const uint64_t total=contentLengthFromUrl(url);if(total>kMaxDownloadBytes){error="WebM audio exceeds the 80 MiB safety limit.";WinHttpCloseHandle(conn);WinHttpCloseHandle(session);return false;}
    if(total)data.reserve((size_t)total);
    bool ok=true;uint64_t offset=0;
    while(ok){
        uint64_t last=offset+kChunkBytes-1;if(total&&last>=total)last=total-1;
        const std::wstring chunkPath=requestPath+L"&range="+std::to_wstring(offset)+L"-"+std::to_wstring(last);
        DWORD status=0;const size_t before=data.size();bool got=fetchChunk(conn,chunkPath,flags,data,status,error);
        if(!got&&status!=416&&!(status>=400&&status<500))got=fetchChunk(conn,chunkPath,flags,data,status,error);
        if(!got){if(status==416&&offset>0&&!total){error.clear();break;}ok=false;break;}
        const uint64_t received=data.size()-before;offset+=received;
        if(total?offset>=total:received<kChunkBytes)break;
        if(!received){error="Audio server returned an empty chunk at byte "+std::to_string(offset)+".";ok=false;}
    }
    if(ok&&total&&offset!=total){error="WebM download incomplete: "+std::to_string(offset)+" of "+std::to_string(total)+" bytes.";ok=false;}
    WinHttpCloseHandle(conn);WinHttpCloseHandle(session);return ok;
}
bool decodeBlockGroup(const std::vector<uint8_t>& b,const Element& group,uint64_t track,OpusDecoder* decoder,unsigned channels,unsigned& preSkip,std::vector<uint8_t>& wav,uint64_t& bytes,std::string& error){
    for(size_t p=group.data;p<group.end;){Element e{};if(!element(b,p,group.end,e))return false;p=e.end;if(e.id==0xA1&&!readBlock(b,e.data,e.end,track,decoder,channels,preSkip,wav,bytes,error))return false;}return true;
}
}

bool decodeWebmOpusToWav(const std::string& url,std::vector<uint8_t>& wav,std::string& diagnostic){
    wav.clear();
    try{
        std::vector<uint8_t> bytes;if(!download(url,bytes,diagnostic))return false;
        Element segment{};size_t p=0;while(p<bytes.size()){Element e{};if(!element(bytes,p,bytes.size(),e)){diagnostic="Invalid EBML/WebM element tree.";return false;}p=e.end;if(e.id==0x18538067){segment=e;break;}}
        if(!segment.id){diagnostic="WebM Segment element is missing.";return false;}
        OpusTrack track{};uint64_t timecodeScale=1000000;double duration=0;
        for(size_t q=segment.data;q<segment.end;){Element e{};if(!element(bytes,q,segment.end,e)){diagnostic="Malformed WebM Segment.";return false;}q=e.end;
            if(e.id==0x1654AE6B){for(size_t r=e.data;r<e.end;){Element child{};if(!element(bytes,r,e.end,child)){diagnostic="Malformed WebM Tracks.";return false;}r=child.end;if(child.id==0xAE&&!parseTrack(bytes,child,track)){diagnostic="Malformed WebM audio track.";return false;}}}
            else if(e.id==0x1549A966){for(size_t r=e.data;r<e.end;){Element child{};if(!element(bytes,r,e.end,child))break;r=child.end;if(child.id==0x2AD7B1&&child.size)timecodeScale=uintElement(bytes,child);else if(child.id==0x4489)duration=floatElement(bytes,child);}}}
        if(!track.found){diagnostic="WebM has no supported mono/stereo A_OPUS track.";return false;}
        int opusError=OPUS_OK;std::unique_ptr<OpusDecoder,void(*)(OpusDecoder*)> decoder(opus_decoder_create(48000,(int)track.channels,&opusError),opus_decoder_destroy);
        if(!decoder){diagnostic="Opus decoder creation failed: "+std::string(opus_strerror(opusError));return false;}
        // Reserva el buffer final de una vez usando la duración del contenedor (o una estimación por tamaño) para evitar copias por crecimiento.
        const double seconds=duration*(double)timecodeScale/1e9;const uint64_t bytesPerSecond=48000ull*track.channels*2;
        uint64_t estimate=(seconds>0&&seconds<86400)?(uint64_t)(seconds*(double)bytesPerSecond):(uint64_t)bytes.size()*12ull;
        estimate=std::min<uint64_t>(estimate+bytesPerSecond,kMaxPcmBytes);wav.reserve((size_t)estimate+44);
        uint8_t header[44]{};initWave(header,track.channels);appendBytes(wav,header,sizeof(header));
        bool ok=true;uint64_t dataBytes=0;unsigned preSkip=track.preSkip;
        for(size_t q=segment.data;ok&&q<segment.end;){Element e{};if(!element(bytes,q,segment.end,e)){diagnostic="Malformed WebM Segment while reading audio.";ok=false;break;}q=e.end;if(e.id!=0x1F43B675)continue;
            for(size_t r=e.data;ok&&r<e.end;){Element child{};if(!element(bytes,r,e.end,child)){diagnostic="Malformed WebM Cluster.";ok=false;break;}r=child.end;
                if(child.id==0xA3)ok=readBlock(bytes,child.data,child.end,track.number,decoder.get(),track.channels,preSkip,wav,dataBytes,diagnostic);
                else if(child.id==0xA0)ok=decodeBlockGroup(bytes,child,track.number,decoder.get(),track.channels,preSkip,wav,dataBytes,diagnostic);
            }
        }
        if(!ok||!dataBytes){if(diagnostic.empty())diagnostic="WebM contained no decodable Opus packets.";wav.clear();wav.shrink_to_fit();return false;}
        put32(wav.data()+4,(uint32_t)(36+dataBytes));put32(wav.data()+40,(uint32_t)dataBytes);
        diagnostic="Decoded "+std::to_string(dataBytes)+" PCM bytes with libopus into memory.";return true;
    }catch(const std::bad_alloc&){
        diagnostic="Out of memory while decoding Opus audio.";std::vector<uint8_t>().swap(wav);return false;
    }
}
}

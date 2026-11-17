#include <windows.h>
#include <winhttp.h>
#include <opus.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
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
bool writeAll(HANDLE file,const void* data,DWORD size){const BYTE* p=(const BYTE*)data;while(size){DWORD wrote=0;if(!WriteFile(file,p,size,&wrote,nullptr)||!wrote)return false;p+=wrote;size-=wrote;}return true;}
void put16(uint8_t* out,uint16_t x){out[0]=(uint8_t)x;out[1]=(uint8_t)(x>>8);}
void put32(uint8_t* out,uint32_t x){out[0]=(uint8_t)x;out[1]=(uint8_t)(x>>8);out[2]=(uint8_t)(x>>16);out[3]=(uint8_t)(x>>24);}
void initWave(uint8_t h[44],unsigned channels){std::memset(h,0,44);std::memcpy(h,"RIFF",4);std::memcpy(h+8,"WAVEfmt ",8);put32(h+16,16);put16(h+20,1);put16(h+22,(uint16_t)channels);put32(h+24,48000);put32(h+28,48000*channels*2);put16(h+32,(uint16_t)(channels*2));put16(h+34,16);std::memcpy(h+36,"data",4);}
bool readBlock(const std::vector<uint8_t>& b,size_t begin,size_t end,uint64_t wantedTrack,OpusDecoder* decoder,unsigned channels,unsigned& preSkip,HANDLE wav,uint64_t& dataBytes,std::string& error){
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
        const opus_int16* output=pcm.data()+(size_t)skip*channels;const DWORD bytes=remain*channels*sizeof(opus_int16);if(dataBytes+bytes>0xffffffffULL-36){error="PCM output is larger than the RIFF/WAVE limit.";return false;}if(!writeAll(wav,output,bytes))return false;dataBytes+=bytes;
    }
    return p<=end;
}
bool download(const std::string& url,std::vector<uint8_t>& data,std::string& error){
    std::wstring wide(url.begin(),url.end());URL_COMPONENTS c{};c.dwStructSize=sizeof(c);wchar_t host[512]{},path[16384]{},extra[16384]{};c.lpszHostName=host;c.dwHostNameLength=512;c.lpszUrlPath=path;c.dwUrlPathLength=16384;c.lpszExtraInfo=extra;c.dwExtraInfoLength=16384;
    if(!WinHttpCrackUrl(wide.c_str(),(DWORD)wide.size(),0,&c)){error="Could not parse the signed audio URL.";return false;}
    const INTERNET_PORT port=c.nPort;const DWORD flags=c.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0;std::wstring requestPath(path,c.dwUrlPathLength);requestPath.append(extra,c.dwExtraInfoLength);
    HINTERNET session=WinHttpOpen(L"Ytm32 Opus/1.6.1",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);if(!session){error="Opus download session failed: "+std::to_string(GetLastError());return false;}WinHttpSetTimeouts(session,5000,10000,15000,15000);
    HINTERNET conn=WinHttpConnect(session,host,port,0);HINTERNET req=conn?WinHttpOpenRequest(conn,L"GET",requestPath.c_str(),nullptr,L"https://www.youtube.com/",WINHTTP_DEFAULT_ACCEPT_TYPES,flags):nullptr;
    bool ok=false;if(req&&WinHttpSendRequest(req,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)&&WinHttpReceiveResponse(req,nullptr)){
        DWORD status=0,size=sizeof(status);if(WinHttpQueryHeaders(req,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX)&&status==200){DWORD available=0;ok=true;while(WinHttpQueryDataAvailable(req,&available)&&available){if(data.size()+available>80u*1024u*1024u){error="WebM audio exceeds the 80 MiB safety limit.";ok=false;break;}size_t old=data.size();data.resize(old+available);DWORD got=0;if(!WinHttpReadData(req,&data[old],available,&got)){data.resize(old);error="Reading WebM audio failed: "+std::to_string(GetLastError());ok=false;break;}data.resize(old+got);if(!got)break;}}
        else error="Audio download HTTP status "+std::to_string(status);
    }else error="WebM audio request failed: "+std::to_string(GetLastError());
    if(req)WinHttpCloseHandle(req);if(conn)WinHttpCloseHandle(conn);WinHttpCloseHandle(session);return ok;
}
std::wstring newWavePath(){wchar_t directory[MAX_PATH]{},file[MAX_PATH]{};if(!GetTempPathW(MAX_PATH,directory)||!GetTempFileNameW(directory,L"ytm",0,file))return {};DeleteFileW(file);std::wstring p=file;size_t dot=p.rfind(L'.');if(dot!=std::wstring::npos)p.resize(dot);p+=L".wav";return p;}
bool decodeBlockGroup(const std::vector<uint8_t>& b,const Element& group,uint64_t track,OpusDecoder* decoder,unsigned channels,unsigned& preSkip,HANDLE wav,uint64_t& bytes,std::string& error){
    for(size_t p=group.data;p<group.end;){Element e{};if(!element(b,p,group.end,e))return false;p=e.end;if(e.id==0xA1&&!readBlock(b,e.data,e.end,track,decoder,channels,preSkip,wav,bytes,error))return false;}return true;
}
}

bool decodeWebmOpusToWav(const std::string& url,std::wstring& wavPath,std::string& diagnostic){
    std::vector<uint8_t> bytes;if(!download(url,bytes,diagnostic))return false;
    Element segment{};size_t p=0;while(p<bytes.size()){Element e{};if(!element(bytes,p,bytes.size(),e)){diagnostic="Invalid EBML/WebM element tree.";return false;}p=e.end;if(e.id==0x18538067){segment=e;break;}}
    if(!segment.id){diagnostic="WebM Segment element is missing.";return false;}
    OpusTrack track{};size_t cursor=segment.data;
    for(size_t q=cursor;q<segment.end;){Element e{};if(!element(bytes,q,segment.end,e)){diagnostic="Malformed WebM Segment.";return false;}q=e.end;if(e.id==0x1654AE6B){for(size_t r=e.data;r<e.end;){Element child{};if(!element(bytes,r,e.end,child)){diagnostic="Malformed WebM Tracks.";return false;}r=child.end;if(child.id==0xAE&&!parseTrack(bytes,child,track)){diagnostic="Malformed WebM audio track.";return false;}}}}
    if(!track.found){diagnostic="WebM has no supported mono/stereo A_OPUS track.";return false;}
    int opusError=OPUS_OK;OpusDecoder* decoder=opus_decoder_create(48000,(int)track.channels,&opusError);if(!decoder){diagnostic="Opus decoder creation failed: "+std::string(opus_strerror(opusError));return false;}
    wavPath=newWavePath();HANDLE output=wavPath.empty()?INVALID_HANDLE_VALUE:CreateFileW(wavPath.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY,nullptr);
    if(output==INVALID_HANDLE_VALUE){diagnostic="Could not create temporary WAV.";opus_decoder_destroy(decoder);return false;}
    uint8_t header[44]{};initWave(header,track.channels);bool ok=writeAll(output,header,sizeof(header));uint64_t dataBytes=0;unsigned preSkip=track.preSkip;cursor=segment.data;
    for(size_t q=cursor;ok&&q<segment.end;){Element e{};if(!element(bytes,q,segment.end,e)){diagnostic="Malformed WebM Segment while reading audio.";ok=false;break;}q=e.end;if(e.id!=0x1F43B675)continue;
        for(size_t r=e.data;ok&&r<e.end;){Element child{};if(!element(bytes,r,e.end,child)){diagnostic="Malformed WebM Cluster.";ok=false;break;}r=child.end;
            if(child.id==0xA3)ok=readBlock(bytes,child.data,child.end,track.number,decoder,track.channels,preSkip,output,dataBytes,diagnostic);
            else if(child.id==0xA0)ok=decodeBlockGroup(bytes,child,track.number,decoder,track.channels,preSkip,output,dataBytes,diagnostic);
        }
    }
    if(ok&&dataBytes){put32(header+4,(uint32_t)(36+dataBytes));put32(header+40,(uint32_t)dataBytes);DWORD written=0;LARGE_INTEGER zero{};if(!SetFilePointerEx(output,zero,nullptr,FILE_BEGIN)||!WriteFile(output,header,sizeof(header),&written,nullptr)||written!=sizeof(header))ok=false;}
    CloseHandle(output);opus_decoder_destroy(decoder);if(!ok||!dataBytes){if(diagnostic.empty())diagnostic="WebM contained no decodable Opus packets.";DeleteFileW(wavPath.c_str());wavPath.clear();return false;}
    diagnostic="Decoded "+std::to_string(dataBytes)+" PCM bytes with libopus.";return true;
}
}

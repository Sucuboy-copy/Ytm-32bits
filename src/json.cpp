#include "ytm/json.hpp"
#include <windows.h>
#include <algorithm>
#include <cstdlib>

namespace ytm {
std::wstring utf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
    return out;
}
std::string jsonEscape(const std::wstring& in) {
    int n = WideCharToMultiByte(CP_UTF8, 0, in.data(), (int)in.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, in.data(), (int)in.size(), &s[0], n, nullptr, nullptr);
    std::string out;
    for (char c : s) { if (c == '"' || c == '\\') out.push_back('\\'); if ((unsigned char)c >= 0x20) out.push_back(c); }
    return out;
}
namespace {
void appendUtf8(std::string& out, unsigned int cp) {
    if (cp <= 0x7f) out.push_back((char)cp);
    else if (cp <= 0x7ff) { out.push_back((char)(0xc0 | (cp >> 6))); out.push_back((char)(0x80 | (cp & 0x3f))); }
    else if (cp <= 0xffff) { out.push_back((char)(0xe0 | (cp >> 12))); out.push_back((char)(0x80 | ((cp >> 6) & 0x3f))); out.push_back((char)(0x80 | (cp & 0x3f))); }
    else { out.push_back((char)(0xf0 | (cp >> 18))); out.push_back((char)(0x80 | ((cp >> 12) & 0x3f))); out.push_back((char)(0x80 | ((cp >> 6) & 0x3f))); out.push_back((char)(0x80 | (cp & 0x3f))); }
}
std::string runsText(const JsonValue* text){
    if(!text)return {};
    if(const auto* simple=member(*text,"simpleText"))return simple->scalar;
    const auto* runs=member(*text,"runs");
    if(!runs||runs->kind!=JsonValue::Kind::Array)return {};
    std::string out;
    for(const auto& run:runs->array)if(const auto* t=member(run,"text"))out+=t->scalar;
    return out;
}
std::string videoIdFrom(const JsonValue& item){
    if(const auto* id=member(item,"videoId"))return id->scalar;
    if(const auto* p=member(item,"playlistItemData"))if(const auto* id=member(*p,"videoId"))return id->scalar;
    if(const auto* p=member(item,"navigationEndpoint"))if(const auto* w=member(*p,"watchEndpoint"))if(const auto* id=member(*w,"videoId"))return id->scalar;
    return {};
}
std::string thumbnailUrlFrom(const JsonValue& item){
    const JsonValue* thumbnail=member(item,"thumbnail");
    if(thumbnail){
        const JsonValue* renderer=member(*thumbnail,"musicThumbnailRenderer");if(!renderer)renderer=member(*thumbnail,"croppedSquareThumbnailRenderer");
        if(renderer)thumbnail=member(*renderer,"thumbnail");
    }
    if(!thumbnail)if(const auto* renderer=member(item,"thumbnailRenderer")){
        thumbnail=member(*renderer,"musicThumbnailRenderer");if(thumbnail)thumbnail=member(*thumbnail,"thumbnail");
        if(!thumbnail){thumbnail=member(*renderer,"croppedSquareThumbnailRenderer");if(thumbnail)thumbnail=member(*thumbnail,"thumbnail");}
    }
    const auto* images=thumbnail?member(*thumbnail,"thumbnails"):nullptr;
    if(!images||images->kind!=JsonValue::Kind::Array)return {};
    std::string best;
    for(const auto& image:images->array)if(const auto* url=member(image,"url"))if(url->kind==JsonValue::Kind::Scalar&&!url->scalar.empty())best=url->scalar;
    return best;
}
void extractVideoItems(const JsonValue& node,std::vector<YouTubeVideoResult>& rows){
    if(node.kind==JsonValue::Kind::Object){
        for(const auto& field:node.object){
            if(field.first=="musicResponsiveListItemRenderer"&&field.second.kind==JsonValue::Kind::Object){
                const auto* cols=member(field.second,"flexColumns");
                if(!cols||cols->kind!=JsonValue::Kind::Array||cols->array.empty())continue;
                const auto* first=member(cols->array[0],"musicResponsiveListItemFlexColumnRenderer");
                const std::string title=runsText(first?member(*first,"text"):nullptr);
                const std::string videoId=videoIdFrom(field.second);
                if(title.empty()||videoId.empty())continue;
                std::string subtitle;
                if(cols->array.size()>1){const auto* second=member(cols->array[1],"musicResponsiveListItemFlexColumnRenderer");subtitle=runsText(second?member(*second,"text"):nullptr);}
                std::wstring line=utf8(title);if(!subtitle.empty())line+=L"  —  "+utf8(subtitle);
                if(line.size()>180)line.resize(180);
                auto duplicate=std::find_if(rows.begin(),rows.end(),[&](const YouTubeVideoResult& r){return r.videoId==videoId;});
                const std::string thumbnail=thumbnailUrlFrom(field.second);
                if(duplicate==rows.end()&&rows.size()<80)rows.push_back({std::move(line),videoId,utf8(thumbnail)});
                continue;
            }
            const bool videoRenderer=field.first=="videoRenderer"||field.first=="compactVideoRenderer"||field.first=="videoWithContextRenderer"||field.first=="playlistVideoRenderer";
            if(videoRenderer&&field.second.kind==JsonValue::Kind::Object){
                const auto* titleNode=member(field.second,"title");
                const std::string title=runsText(titleNode);
                if(title.empty())continue;
                const std::string videoId=videoIdFrom(field.second);
                if(videoId.empty())continue;
                std::string subtitle=runsText(member(field.second,"ownerText"));
                if(subtitle.empty())subtitle=runsText(member(field.second,"shortBylineText"));
                if(subtitle.empty())subtitle=runsText(member(field.second,"longBylineText"));
                std::wstring line=utf8(title);if(!subtitle.empty())line+=L"  —  "+utf8(subtitle);
                if(line.size()>180)line.resize(180);
                auto duplicate=std::find_if(rows.begin(),rows.end(),[&](const YouTubeVideoResult& r){return r.videoId==videoId;});
                const std::string thumbnail=thumbnailUrlFrom(field.second);
                if(duplicate==rows.end()&&rows.size()<80)rows.push_back({std::move(line),videoId,utf8(thumbnail)});
            }else extractVideoItems(field.second,rows);
        }
    }else if(node.kind==JsonValue::Kind::Array){for(const auto& item:node.array)extractVideoItems(item,rows);}
}
}
JsonParser::JsonParser(const std::string& input):s(input){}
void JsonParser::ws(){while(pos<s.size()&&(s[pos]==' '||s[pos]=='\r'||s[pos]=='\n'||s[pos]=='\t'))++pos;}
bool JsonParser::hex4(unsigned int& cp){
    if(pos+4>s.size())return false;cp=0;
    for(int n=0;n<4;++n){char c=s[pos++];unsigned int v=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:16;if(v>15)return false;cp=(cp<<4)|v;}
    return true;
}
bool JsonParser::string(std::string& out){
    if(pos>=s.size()||s[pos++]!='"')return false;
    while(pos<s.size()){
        unsigned char c=(unsigned char)s[pos++];if(c=='"')return true;if(c<0x20)return false;
        if(c!='\\'){out.push_back((char)c);continue;}
        if(pos>=s.size())return false;char e=s[pos++];
        switch(e){case '"':out.push_back('"');break;case '\\':out.push_back('\\');break;case '/':out.push_back('/');break;case 'b':out.push_back('\b');break;case 'f':out.push_back('\f');break;case 'n':out.push_back('\n');break;case 'r':out.push_back('\r');break;case 't':out.push_back('\t');break;
        case 'u':{unsigned int cp;if(!hex4(cp))return false;if(cp>=0xd800&&cp<=0xdbff){if(pos+2>s.size()||s[pos++]!='\\'||s[pos++]!='u')return false;unsigned int low;if(!hex4(low)||low<0xdc00||low>0xdfff)return false;cp=0x10000+((cp-0xd800)<<10)+(low-0xdc00);}else if(cp>=0xdc00&&cp<=0xdfff)return false;appendUtf8(out,cp);break;}default:return false;}
    }return false;
}
bool JsonParser::value(JsonValue& out,unsigned depth){
    if(depth>128)return false;ws();if(pos>=s.size())return false;
    if(s[pos]=='{'){++pos;out.kind=JsonValue::Kind::Object;ws();if(pos<s.size()&&s[pos]=='}'){++pos;return true;}for(;;){ws();std::string key;if(!string(key))return false;ws();if(pos>=s.size()||s[pos++]!=':')return false;JsonValue v;if(!value(v,depth+1))return false;out.object.emplace_back(std::move(key),std::move(v));ws();if(pos>=s.size())return false;char c=s[pos++];if(c=='}')return true;if(c!=',')return false;}}
    if(s[pos]=='['){++pos;out.kind=JsonValue::Kind::Array;ws();if(pos<s.size()&&s[pos]==']'){++pos;return true;}for(;;){JsonValue v;if(!value(v,depth+1))return false;out.array.push_back(std::move(v));ws();if(pos>=s.size())return false;char c=s[pos++];if(c==']')return true;if(c!=',')return false;}}
    if(s[pos]=='"'){out.kind=JsonValue::Kind::Scalar;return string(out.scalar);}
    size_t start=pos;while(pos<s.size()&&s[pos]!=','&&s[pos]!=']'&&s[pos]!='}'&&s[pos]!=' '&&s[pos]!='\r'&&s[pos]!='\n'&&s[pos]!='\t')++pos;if(start==pos)return false;out.scalar=s.substr(start,pos-start);return true;
}
bool JsonParser::parse(JsonValue& out){bool ok=value(out,0);ws();return ok&&pos==s.size();}
const JsonValue* member(const JsonValue& v,const char* key){if(v.kind!=JsonValue::Kind::Object)return nullptr;for(const auto& p:v.object)if(p.first==key)return &p.second;return nullptr;}
const JsonValue* arrayItem(const JsonValue* v,size_t index){return v&&v->kind==JsonValue::Kind::Array&&index<v->array.size()?&v->array[index]:nullptr;}
std::string visitorDataFromJson(const std::string& json){JsonValue root;JsonParser parser(json);if(!parser.parse(root))return {};const auto* context=member(root,"responseContext");const auto* visitor=context?member(*context,"visitorData"):nullptr;return visitor?visitor->scalar:std::string();}
std::vector<YouTubeVideoResult> parseVideoSearchResults(const std::string& json,bool& parsed){JsonValue root;JsonParser parser(json);parsed=parser.parse(root);std::vector<YouTubeVideoResult> rows;if(parsed)extractVideoItems(root,rows);return rows;}
std::vector<AudioFormat> audioFormatsFromPlayerResponse(const std::string& json,std::string& log,bool& loginRequired){
    JsonValue root;JsonParser parser(json);
    if(!parser.parse(root)){log+="Player response JSON invalid.\r\n";return {};}
    if(const auto* error=member(root,"error")){
        const auto* code=member(*error,"code");const auto* message=member(*error,"message");const auto* state=member(*error,"status");
        log+="Player API error";if(code)log+=" code="+code->scalar;if(state)log+=" status="+state->scalar;if(message)log+=" message="+message->scalar;log+="\r\n";
    }
    if(const auto* playability=member(root,"playabilityStatus")){
        const auto* state=member(*playability,"status");const auto* reason=member(*playability,"reason");
        log+="Playability: "+(state?state->scalar:std::string("unknown"))+"\r\n";
        if(state&&state->scalar=="LOGIN_REQUIRED")loginRequired=true;
        if(reason)log+="Reason: "+reason->scalar+"\r\n";
    }
    std::vector<AudioFormat> found;
    const auto* streaming=member(root,"streamingData");const auto* formats=streaming?member(*streaming,"adaptiveFormats"):nullptr;
    if(formats&&formats->kind==JsonValue::Kind::Array){
        log+="adaptiveFormats entries: "+std::to_string(formats->array.size())+"\r\n";size_t inspected=0;
        for(const auto& format:formats->array){
            const auto* itag=member(format,"itag");const auto* url=member(format,"url");const auto* cipher=member(format,"signatureCipher");if(!cipher)cipher=member(format,"cipher");const auto* mime=member(format,"mimeType");
            const int code=itag?atoi(itag->scalar.c_str()):0;const bool direct=url&&!url->scalar.empty();
            if(mime&&mime->scalar.find("audio/")==0&&inspected<40){log+="Audio stream itag "+std::to_string(code)+", mime="+mime->scalar.substr(0,mime->scalar.find(';'))+", direct URL="+(direct?"yes":"no")+", cipher="+(cipher?"yes":"no")+"\r\n";++inspected;}
            if((code==251||code==140)&&direct)found.push_back({code,url->scalar});
        }
    }else log+="streamingData.adaptiveFormats: missing\r\n";
    std::sort(found.begin(),found.end(),[](const AudioFormat&a,const AudioFormat&b){return a.itag==251&&b.itag==140;});
    return found;
}
}

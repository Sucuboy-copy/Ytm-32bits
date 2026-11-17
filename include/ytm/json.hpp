#pragma once
#include <string>
#include <utility>
#include <vector>
#include <cstddef>
#include "ytm/models.hpp"

namespace ytm {
struct JsonValue {
    enum class Kind { Scalar, Object, Array } kind = Kind::Scalar;
    std::string scalar;
    std::vector<std::pair<std::string, JsonValue>> object;
    std::vector<JsonValue> array;
};

class JsonParser {
public:
    explicit JsonParser(const std::string& input);
    bool parse(JsonValue& out);
private:
    const std::string& s;
    size_t pos = 0;
    void ws();
    bool hex4(unsigned int& cp);
    bool string(std::string& out);
    bool value(JsonValue& out, unsigned depth);
};

const JsonValue* member(const JsonValue& value, const char* key);
const JsonValue* arrayItem(const JsonValue* value, size_t index);
std::string visitorDataFromJson(const std::string& json);
std::vector<YouTubeVideoResult> parseVideoSearchResults(const std::string& json, bool& parsed);
std::vector<AudioFormat> audioFormatsFromPlayerResponse(const std::string& json, std::string& log, bool& loginRequired);
std::wstring utf8(const std::string& value);
std::string jsonEscape(const std::wstring& value);
}

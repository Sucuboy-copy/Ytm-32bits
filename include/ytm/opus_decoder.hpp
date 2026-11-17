#pragma once
#include <string>

namespace ytm {
bool decodeWebmOpusToWav(const std::string& url, std::wstring& wavPath, std::string& diagnostic);
}

#pragma once
#include <string>
#include <vector>
#include "ytm/models.hpp"
#include "ytm/json.hpp"

namespace ytm {
std::string searchInnerTube(const std::wstring& query, std::string& log);
std::vector<AudioFormat> resolveAudio(const std::string& videoId, std::string visitorData, std::string& log, bool& loginRequired);
}

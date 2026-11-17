#pragma once
#include <string>
#include <vector>

namespace ytm {
struct YouTubeVideoResult { std::wstring display; std::string videoId; std::wstring thumbnailUrl; };
struct AudioFormat { int itag = 0; std::string url; };
}

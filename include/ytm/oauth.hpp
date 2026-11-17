#pragma once
#include <string>

namespace ytm {
struct OAuthDeviceInfo {
    int state = -1; // -1 error, 0 waiting, 1 authorized
    int pollSeconds = 5;
    std::string userCode;
    std::string verificationUrl;
    std::string message;
};

OAuthDeviceInfo beginOAuthDeviceFlow();
OAuthDeviceInfo pollOAuthDeviceFlow();
std::wstring oauthBearerToken(std::string& diagnostic);
bool hasOAuthSession();
void loadOAuthSession();
void clearOAuthSession();
}

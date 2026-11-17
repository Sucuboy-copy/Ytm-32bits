#pragma once
#include <windows.h>
#include <string>

namespace ytm {
bool importNetscapeCookies(const std::wstring& path, std::wstring& cookieHeader, size_t& imported);
bool saveSession(const std::wstring& cookie);
void loadSession();
std::wstring sessionPath();
std::wstring cookieValue(const std::wstring& header, const wchar_t* key);
std::wstring authenticatedHeaders(const std::wstring& origin);
std::wstring sessionCookie();
bool hasSession();
void setSessionCookie(const std::wstring& cookie);
void clearSessionCookie();
}

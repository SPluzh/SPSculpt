#pragma once
#include <string>

#ifdef _WIN32
std::wstring utf8ToWide(const std::string& str);
std::string wideToUtf8(const std::wstring& wstr);
#endif

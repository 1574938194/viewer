#include "util/string_util.h"

#include <Windows.h>

namespace nexus {

std::wstring Utf8ToWide(const std::string& utf8)
{
    if (utf8.empty())
    {
        return {};
    }
    int count = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                    static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        wide.data(), count);
    return wide;
}

std::string WideToUtf8(const std::wstring& wide)
{
    if (wide.empty())
    {
        return {};
    }
    int count = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                    static_cast<int>(wide.size()), nullptr, 0,
                                    nullptr, nullptr);
    std::string utf8(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        utf8.data(), count, nullptr, nullptr);
    return utf8;
}

std::string AsciiLower(std::string s)
{
    for (char& c : s)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c += 'a' - 'A';
        }
    }
    return s;
}

}  // namespace nexus

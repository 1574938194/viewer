#ifndef NEXUS_SRC_UTIL_STRING_UTIL_H_
#define NEXUS_SRC_UTIL_STRING_UTIL_H_

#include <string>

namespace nexus {

std::wstring Utf8ToWide(const std::string& utf8);

std::string WideToUtf8(const std::wstring& wide);

std::string AsciiLower(std::string s);

}  // namespace nexus

#endif  // NEXUS_SRC_UTIL_STRING_UTIL_H_

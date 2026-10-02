#ifndef NEXUS_SRC_UTIL_TAG_VALUES_H_
#define NEXUS_SRC_UTIL_TAG_VALUES_H_

#include <cstddef>
#include <string>
#include <vector>

namespace nexus {

std::string TagKey(const std::string& value);
std::vector<std::string> UniqueTags(const std::vector<std::string>& values);
std::vector<std::string> ParseTags(const std::wstring& value);
// Returns the original text when the tag already exists.
std::wstring AppendTag(const std::wstring& text, const std::string& tag);
constexpr std::size_t kQuickTagsPerPage = 6;
std::size_t QuickTagPageCount(std::size_t count);
std::vector<std::string> QuickTagPage(const std::vector<std::string>& tags,
                                      std::size_t page);

}  // namespace nexus

#endif  // NEXUS_SRC_UTIL_TAG_VALUES_H_

#include "util/tag_values.h"

#include <algorithm>
#include <cwctype>
#include <unordered_set>

#include "util/string_util.h"

namespace nexus {
namespace {

std::wstring Trim(std::wstring value)
{
  const auto first = std::find_if_not(value.begin(), value.end(), iswspace);
  const auto last =
      std::find_if_not(value.rbegin(), value.rend(), iswspace).base();
  return first < last ? std::wstring(first, last) : std::wstring();
}

}  // namespace

std::string TagKey(const std::string& value)
{
  return AsciiLower(WideToUtf8(Trim(Utf8ToWide(value))));
}

std::vector<std::string> UniqueTags(const std::vector<std::string>& values)
{
  std::vector<std::string> result;
  std::unordered_set<std::string> seen;
  for (const std::string& value : values) {
    const std::string trimmed = WideToUtf8(Trim(Utf8ToWide(value)));
    const std::string key = TagKey(trimmed);
    if (!key.empty() && seen.insert(key).second) {
      result.push_back(trimmed);
    }
  }
  return result;
}

std::vector<std::string> ParseTags(const std::wstring& value)
{
  std::vector<std::string> values;
  std::wstring token;
  for (wchar_t character : value) {
    if (character == L',' || character == L';' || character == L'，') {
      values.push_back(WideToUtf8(token));
      token.clear();
    } else {
      token.push_back(character);
    }
  }
  values.push_back(WideToUtf8(token));
  return UniqueTags(values);
}

std::wstring AppendTag(const std::wstring& text, const std::string& tag)
{
  const std::string key = TagKey(tag);
  if (key.empty()) {
    return text;
  }
  for (const std::string& existing : ParseTags(text)) {
    if (TagKey(existing) == key) {
      return text;
    }
  }
  return text + (Trim(text).empty() ? L"" : L", ") + Trim(Utf8ToWide(tag));
}

std::size_t QuickTagPageCount(std::size_t count)
{
  return std::max<std::size_t>(
      1, (count + kQuickTagsPerPage - 1) / kQuickTagsPerPage);
}

std::vector<std::string> QuickTagPage(const std::vector<std::string>& tags,
                                      std::size_t page)
{
  if (page >= QuickTagPageCount(tags.size())) {
    return {};
  }
  const std::size_t first = page * kQuickTagsPerPage;
  const std::size_t last = std::min(tags.size(), first + kQuickTagsPerPage);
  return {tags.begin() + first, tags.begin() + last};
}

}  // namespace nexus

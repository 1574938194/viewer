#include "app/settings.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>
#include <vector>

#include "util/string_util.h"

namespace nexus::app {
namespace {

std::string Trim(std::string value)
{
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return {};
    }
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string Value(std::string value)
{
    value = Trim(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
    {
        value = value.substr(1, value.size() - 2);
        std::string unescaped;
        unescaped.reserve(value.size());
        bool escaped = false;
        for (char character : value)
        {
            if (escaped)
            {
                if (character == '\\' || character == '"')
                {
                    unescaped.push_back(character);
                }
                else
                {
                    unescaped.push_back('\\');
                    unescaped.push_back(character);
                }
                escaped = false;
            }
            else if (character == '\\')
            {
                escaped = true;
            }
            else
            {
                unescaped.push_back(character);
            }
        }
        if (escaped)
        {
            unescaped.push_back('\\');
        }
        return unescaped;
    }
    return value;
}

std::string Escape(std::string value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (char character : value)
    {
        if (character == '\\' || character == '"')
        {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    return escaped;
}

std::string TrimTag(std::string value)
{
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return {};
    }
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

void AppendUniqueTag(std::vector<std::string>* tags, std::string value)
{
    if (tags == nullptr)
    {
        return;
    }
    value = TrimTag(std::move(value));
    if (value.empty() ||
        std::find(tags->begin(), tags->end(), value) != tags->end())
    {
        return;
    }
    tags->push_back(std::move(value));
}

}  // namespace

void Settings::set_presetTags(std::vector<std::string> value)
{
    presetTags_.clear();
    for (std::string& tag : value)
    {
        AppendUniqueTag(&presetTags_, std::move(tag));
    }
}

bool Settings::Load(const std::filesystem::path& file)
{
    std::ifstream input(file, std::ios::binary);
    if (!input)
    {
        return false;
    }
    // Loading replaces the complete settings snapshot.  This matters when a
    // caller reuses a Settings instance after a previous customisation: an
    // omitted key must receive the same defaults as a first launch rather
    // than retaining stale values from the old snapshot.
    videoPlayer_.clear();
    imageViewer_.clear();
    managedLibrary_.clear();
    deleteSourceAfterImport_ = true;
    askMetadataOnDrop_ = true;
    presetTags_ = Settings().presetTags();
    bool presetTagsSeen = false;
    std::string line;
    while (std::getline(input, line))
    {
        line = Trim(line);
        if (line.empty() || line.front() == '#')
        {
            continue;
        }
        const size_t equals = line.find('=');
        if (equals == std::string::npos)
        {
            continue;
        }
        const std::string key = Trim(line.substr(0, equals));
        const std::string value = Value(line.substr(equals + 1));
        if (key == "video_player")
        {
            videoPlayer_ = Utf8ToWide(value);
        }
        else if (key == "image_viewer")
        {
            imageViewer_ = Utf8ToWide(value);
        }
        else if (key == "managed_library")
        {
            managedLibrary_ = Utf8ToWide(value);
        }
        else if (key == "delete_source_after_import")
        {
            deleteSourceAfterImport_ = value == "true" || value == "1";
        }
        else if (key == "ask_metadata_on_drop")
        {
            askMetadataOnDrop_ = value != "false" && value != "0";
        }
        else if (key == "preset_tags")
        {
            // The value is an empty TOML array marker. Individual tags are
            // stored on following preset_tag lines so arbitrary Unicode text
            // can use the same escaping rules as the other settings.
            if (!presetTagsSeen)
            {
                presetTags_.clear();
                presetTagsSeen = true;
            }
        }
        else if (key == "preset_tag")
        {
            if (!presetTagsSeen)
            {
                presetTags_.clear();
                presetTagsSeen = true;
            }
            AppendUniqueTag(&presetTags_, value);
        }
    }
    return true;
}

bool Settings::Save(const std::filesystem::path& file) const
{
    if (file.empty())
    {
        return false;
    }
    std::error_code error;
    const std::filesystem::path parent = file.parent_path();
    if (!parent.empty())
    {
        std::filesystem::create_directories(parent, error);
        if (error)
        {
            return false;
        }
    }
    const std::filesystem::path temporary =
        file.wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId());
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        return false;
    }
    output << "# Viewer settings\n";
    output << "video_player = \"" << Escape(WideToUtf8(videoPlayer_))
           << "\"\n";
    output << "image_viewer = \"" << Escape(WideToUtf8(imageViewer_))
           << "\"\n";
    output << "managed_library = \""
           << Escape(WideToUtf8(managedLibrary_)) << "\"\n";
    output << "delete_source_after_import = "
           << (deleteSourceAfterImport_ ? "true" : "false") << "\n";
    output << "ask_metadata_on_drop = "
           << (askMetadataOnDrop_ ? "true" : "false") << "\n";
    output << "preset_tags = []\n";
    for (const std::string& tag : presetTags_)
    {
        output << "preset_tag = \"" << Escape(tag) << "\"\n";
    }
    if (!output.good())
    {
        output.close();
        std::filesystem::remove(temporary, error);
        return false;
    }
    output.close();
    if (!MoveFileExW(temporary.c_str(), file.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

}  // namespace nexus::app

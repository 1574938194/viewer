#include "app/settings.h"

#include <Windows.h>

#include <fstream>
#include <string>

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

}  // namespace

bool Settings::Load(const std::filesystem::path& file)
{
    std::ifstream input(file, std::ios::binary);
    if (!input)
    {
        return false;
    }
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
        else if (key == "delete_source_after_import")
        {
            deleteSourceAfterImport_ = value == "true" || value == "1";
        }
    }
    return true;
}

bool Settings::Save(const std::filesystem::path& file) const
{
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    if (error)
    {
        return false;
    }
    const std::filesystem::path temporary =
        file.wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId());
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        return false;
    }
    output << "# Nexus settings\n";
    output << "video_player = \"" << Escape(WideToUtf8(videoPlayer_))
           << "\"\n";
    output << "image_viewer = \"" << Escape(WideToUtf8(imageViewer_))
           << "\"\n";
    output << "delete_source_after_import = "
           << (deleteSourceAfterImport_ ? "true" : "false") << "\n";
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

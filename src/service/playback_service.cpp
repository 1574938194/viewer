#include "service/playback_service.h"

#include <Windows.h>
#include <shellapi.h>

#include "app/settings.h"

namespace nexus::service {

namespace {

bool FileExists(const std::wstring& path)
{
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

}  // namespace

std::wstring PlaybackService::FindVideoPlayer() const
{
    wchar_t programFiles[MAX_PATH] = {};
    wchar_t programFilesX86[MAX_PATH] = {};
    GetEnvironmentVariableW(L"ProgramFiles", programFiles, MAX_PATH);
    GetEnvironmentVariableW(L"ProgramFiles(x86)", programFilesX86, MAX_PATH);

    const std::wstring candidates[] = {
        std::wstring(programFiles) + L"\\DAUM\\PotPlayer\\PotPlayerMini64.exe",
        std::wstring(programFiles) + L"\\PotPlayer\\PotPlayerMini64.exe",
        std::wstring(programFilesX86) + L"\\DAUM\\PotPlayer\\PotPlayerMini64.exe",
        std::wstring(programFilesX86) + L"\\DAUM\\PotPlayer\\PotPlayerMini.exe",
    };
    for (const std::wstring& candidate : candidates)
    {
        if (FileExists(candidate))
        {
            return candidate;
        }
    }
    return {};
}

bool PlaybackService::OpenVideo(HWND hwnd, const std::filesystem::path& path)
{
    std::wstring player = settings_ != nullptr ? settings_->videoPlayer()
                                               : std::wstring();
    if (!player.empty() && !FileExists(player))
    {
        player.clear();
    }
    if (player.empty())
    {
        player = FindVideoPlayer();
    }
    if (!player.empty())
    {
        std::wstring command =
            L"\"" + player + L"\" \"" + path.wstring() + L"\"";
        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process = {};
        if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
                           nullptr, nullptr, &startup, &process))
        {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            return true;
        }
    }
    return reinterpret_cast<INT_PTR>(ShellExecuteW(
               hwnd, L"open", path.c_str(), nullptr, nullptr,
               SW_SHOWNORMAL)) > 32;
}

bool PlaybackService::OpenImage(HWND hwnd, const std::filesystem::path& path)
{
    if (settings_ != nullptr && !settings_->imageViewer().empty() &&
        FileExists(settings_->imageViewer()))
    {
        std::wstring command = L"\"" + settings_->imageViewer() +
                               L"\" \"" + path.wstring() + L"\"";
        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process = {};
        if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
                           nullptr, nullptr, &startup, &process))
        {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            return true;
        }
    }
    return reinterpret_cast<INT_PTR>(ShellExecuteW(
               hwnd, L"open", path.c_str(), nullptr, nullptr,
               SW_SHOWNORMAL)) > 32;
}

}  // namespace nexus::service

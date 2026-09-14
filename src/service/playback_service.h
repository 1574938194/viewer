#ifndef NEXUS_SRC_SERVICE_PLAYBACK_SERVICE_H_
#define NEXUS_SRC_SERVICE_PLAYBACK_SERVICE_H_

#include <Windows.h>

#include <filesystem>
#include <string>

namespace nexus::app {
class Settings;
}

namespace nexus::service {

// Opens resources with external applications (design doc section 49/50).
// Videos prefer the configured player (PotPlayer when found) via
// CreateProcessW and fall back to the default file association.
class PlaybackService
{
public:
    explicit PlaybackService(const nexus::app::Settings* settings = nullptr)
        : settings_(settings) {}

    void SetSettings(const nexus::app::Settings* settings)
    {
        settings_ = settings;
    }
    bool OpenVideo(HWND hwnd, const std::filesystem::path& path);

    bool OpenImage(HWND hwnd, const std::filesystem::path& path);

private:
    std::wstring FindVideoPlayer() const;
    const nexus::app::Settings* settings_ = nullptr;
};

}  // namespace nexus::service

#endif  // NEXUS_SRC_SERVICE_PLAYBACK_SERVICE_H_

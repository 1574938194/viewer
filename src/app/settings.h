#ifndef NEXUS_SRC_APP_SETTINGS_H_
#define NEXUS_SRC_APP_SETTINGS_H_

#include <filesystem>
#include <string>
#include <utility>

namespace nexus::app {

class Settings {
public:
    bool Load(const std::filesystem::path& file);
    bool Save(const std::filesystem::path& file) const;

    const std::wstring& videoPlayer() const { return videoPlayer_; }
    const std::wstring& imageViewer() const { return imageViewer_; }
    void set_videoPlayer(std::wstring value) { videoPlayer_ = std::move(value); }
    void set_imageViewer(std::wstring value) { imageViewer_ = std::move(value); }
    bool deleteSourceAfterImport() const { return deleteSourceAfterImport_; }
    void set_deleteSourceAfterImport(bool value) { deleteSourceAfterImport_ = value; }

private:
    std::wstring videoPlayer_;
    std::wstring imageViewer_;
    bool deleteSourceAfterImport_ = false;
};

}  // namespace nexus::app

#endif  // NEXUS_SRC_APP_SETTINGS_H_

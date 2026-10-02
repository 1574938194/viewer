#ifndef NEXUS_SRC_APP_SETTINGS_H_
#define NEXUS_SRC_APP_SETTINGS_H_

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace nexus::app {

class Settings {
public:
    bool Load(const std::filesystem::path& file);
    bool Save(const std::filesystem::path& file) const;

    const std::wstring& videoPlayer() const { return videoPlayer_; }
    const std::wstring& imageViewer() const { return imageViewer_; }
    const std::wstring& managedLibrary() const { return managedLibrary_; }
    void set_videoPlayer(std::wstring value) { videoPlayer_ = std::move(value); }
    void set_imageViewer(std::wstring value) { imageViewer_ = std::move(value); }
    void set_managedLibrary(std::wstring value)
    {
        managedLibrary_ = std::move(value);
    }
    bool deleteSourceAfterImport() const { return deleteSourceAfterImport_; }
    void set_deleteSourceAfterImport(bool value) { deleteSourceAfterImport_ = value; }
    bool askMetadataOnDrop() const { return askMetadataOnDrop_; }
    void set_askMetadataOnDrop(bool value) { askMetadataOnDrop_ = value; }
    const std::vector<std::string>& presetTags() const { return presetTags_; }
    void set_presetTags(std::vector<std::string> value);

private:
    std::wstring videoPlayer_;
    std::wstring imageViewer_;
    // Empty means the portable default next to vr.exe.
    std::wstring managedLibrary_;
    // Managed imports remove the original after a verified archive by
    // default; users can disable this in Settings.
    bool deleteSourceAfterImport_ = true;
    bool askMetadataOnDrop_ = true;
    std::vector<std::string> presetTags_ = {
        "白丝", "美少女", "萝莉", "3p", "肛交", "内射"};
};

}  // namespace nexus::app

#endif  // NEXUS_SRC_APP_SETTINGS_H_

#ifndef NEXUS_SRC_UI_IMPORT_DIALOG_H_
#define NEXUS_SRC_UI_IMPORT_DIALOG_H_

#include <Windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "service/import_service.h"

namespace nexus::ui {

class ImportDialog {
public:
    static std::optional<service::ImportMetadata> Show(
        HWND owner, const std::vector<std::filesystem::path>& files,
        const std::vector<std::string>& categories,
        const std::vector<std::string>& recentTags,
        const service::ImportMetadata* initial = nullptr,
        bool editMode = false);
};

}  // namespace nexus::ui

#endif  // NEXUS_SRC_UI_IMPORT_DIALOG_H_

#ifndef NEXUS_SRC_UI_SETTINGS_DIALOG_H_
#define NEXUS_SRC_UI_SETTINGS_DIALOG_H_

#include <Windows.h>

#include "app/settings.h"

namespace nexus::ui {

class SettingsDialog {
public:
    // |clearCacheRequested| is set when the user chooses the safe cache
    // reset action. It never touches managed-library source files; the caller
    // owns the database/thumbnail rebuild operation.
    static bool Show(HWND owner, app::Settings* settings,
                     bool* clearCacheRequested = nullptr);
};

}  // namespace nexus::ui

#endif  // NEXUS_SRC_UI_SETTINGS_DIALOG_H_

#ifndef NEXUS_SRC_UI_SETTINGS_DIALOG_H_
#define NEXUS_SRC_UI_SETTINGS_DIALOG_H_

#include <Windows.h>

#include "app/settings.h"

namespace nexus::ui {

class SettingsDialog {
public:
    static bool Show(HWND owner, app::Settings* settings);
};

}  // namespace nexus::ui

#endif  // NEXUS_SRC_UI_SETTINGS_DIALOG_H_

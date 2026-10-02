#ifndef NEXUS_SRC_UI_DELETE_DIALOG_H_
#define NEXUS_SRC_UI_DELETE_DIALOG_H_

#include <Windows.h>

#include <string>

namespace nexus::ui {

class DeleteDialog {
 public:
  static bool Show(HWND owner, const std::wstring& summary, bool permanent);
};

}  // namespace nexus::ui

#endif  // NEXUS_SRC_UI_DELETE_DIALOG_H_

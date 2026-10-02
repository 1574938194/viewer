#include <Windows.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "app/application.h"
#include "app/icon_resource.h"
#include "database/migration.h"
#include "ui/dark_controls.h"
#include "ui/delete_dialog.h"
#include "ui/import_dialog.h"
#include "ui/resource_order.h"
#include "ui/selection_drag.h"
#include "ui/settings_dialog.h"
#include "util/tag_values.h"

namespace {

void Check(bool condition, const char* message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::vector<nexus::ResourceId> Ids(
    const std::vector<const nexus::Resource*>& items)
{
  std::vector<nexus::ResourceId> result;
  for (const auto* item : items) {
    result.push_back(item->id);
  }
  return result;
}

void TestTags()
{
  Check(nexus::ParseTags(L" Uniform, uniform ; 白丝， 白丝 ; , New ") ==
            std::vector<std::string>({"Uniform", "白丝", "New"}),
        "tag parsing deduplication");
  Check(nexus::AppendTag(L" Uniform ", "uniform") == L" Uniform ",
        "duplicate click changed text");
  Check(nexus::AppendTag(L"白丝", " Custom ") == L"白丝, Custom",
        "new tag append");
  Check(nexus::AppendTag(nexus::AppendTag(L"", "白丝"), "白丝") == L"白丝",
        "repeated tag click");
  const auto tags = nexus::UniqueTags(
      {" One ", "one", "二", "三", "四", "五", "六", "新增", " ", "二"});
  Check(tags.size() == 7 && tags.front() == "One", "preset/recent merge order");
  Check(nexus::QuickTagPageCount(tags.size()) == 2, "tag pagination count");
  Check(nexus::QuickTagPage(tags, 0).size() == 6 &&
            nexus::QuickTagPage(tags, 1) == std::vector<std::string>({"新增"}),
        "new preset page missing");
  Check(nexus::QuickTagPage({}, 0).empty() &&
            nexus::QuickTagPage(tags, 2).empty(),
        "tag page bounds");
}

void TestOrdering()
{
  nexus::Resource first, second, added, folder, parent;
  first.id = 1;
  first.hotScore = 10;
  second.id = 2;
  second.hotScore = 1;
  added.id = 3;
  added.hotScore = 100;
  std::vector<const nexus::Resource*> rows = {&second, &first};
  nexus::ui::SortResources(&rows, nexus::ui::ResourceSort::kHot, false);
  nexus::ui::ResourceOrder order;
  order.Reset(rows);
  second.hotScore = 50;
  rows = {&second, &first, &added};
  order.Reconcile(rows);
  order.Apply(&rows);
  Check(Ids(rows) == std::vector<nexus::ResourceId>({1, 2, 3}),
        "stats/refresh/new resource reordered snapshot");
  std::vector<const nexus::Resource*> subset = {&added, &first};
  order.Apply(&subset);
  Check(Ids(subset) == std::vector<nexus::ResourceId>({1, 3}),
        "filter changed snapshot order");
  nexus::ui::SortResources(&rows, nexus::ui::ResourceSort::kHot, false);
  order.Reset(rows);
  Check(Ids(rows) == std::vector<nexus::ResourceId>({3, 2, 1}),
        "explicit sort ignored latest stats");
  order.Reconcile({&first, &second});
  order.Reconcile({&first, &second, &added});
  order.Apply(&rows);
  Check(Ids(rows) == std::vector<nexus::ResourceId>({2, 1, 3}),
        "deleted rank not pruned");
  folder.id = 4;
  folder.type = nexus::ResourceType::kFolder;
  folder.originalFilename = "folder";
  parent.id = 5;
  parent.type = nexus::ResourceType::kFolder;
  parent.originalFilename = "..";
  rows = {&first, &folder, &second, &parent};
  nexus::ui::SortResources(&rows, nexus::ui::ResourceSort::kHot, true);
  Check(Ids(rows) == std::vector<nexus::ResourceId>({5, 4, 2, 1}),
        "browse grouping/sort");
}

void TestDragGeometry()
{
  std::vector<nexus::Resource> resources(30);
  std::vector<const nexus::Resource*> rows;
  for (std::size_t i = 0; i < resources.size(); ++i) {
    resources[i].id = i + 1;
    rows.push_back(&resources[i]);
  }
  for (float scale : {1.0f, 1.5f, 2.0f}) {
    nexus::ui::GridView view;
    view.items = &rows;
    view.scale = scale;
    view.content = {236 * scale, 100 * scale, 460 * scale, 500 * scale};
    nexus::ui::SelectionDrag drag;
    drag.Begin({static_cast<LONG>(240 * scale), static_cast<LONG>(104 * scale)},
               view.content, 0, true, {30}, 30);
    view.scrollY = 620 * scale;
    drag.Update(
        {static_cast<LONG>(450 * scale), static_cast<LONG>(490 * scale)},
        view.content, view.scrollY);
    const auto rect = drag.content_rect();
    Check(rect.top == 4 * scale && rect.bottom == 1010 * scale,
          "selection content anchor moved on scroll");
    const auto hits = nexus::ui::ResourceGrid::HitTestRect(view, rect);
    Check(hits.size() == 7 && hits.front() == 1 && hits.back() == 7,
          "cross-screen selection lost offscreen cards");
    Check(drag.Select(hits).contains(30), "Ctrl selection lost original items");
    const auto window_rect = drag.window_rect(view.content, view.scrollY);
    Check(window_rect.top == -516 * scale,
          "selection rendering not following scroll");
    drag.Update(
        {static_cast<LONG>(450 * scale), static_cast<LONG>(160 * scale)},
        view.content, 0);
    Check(drag.Select(nexus::ui::ResourceGrid::HitTestRect(view,
                                                           drag.content_rect()))
                  .size() == 2,
          "reverse drag did not shrink selection");
    drag.Update(
        {static_cast<LONG>(450 * scale), static_cast<LONG>(500 * scale)},
        view.content, 0);
    Check(drag.ScrollStep(view.content, scale) > 0,
          "bottom edge autoscroll missing");
    drag.Update(
        {static_cast<LONG>(450 * scale), static_cast<LONG>(100 * scale)},
        view.content, 0);
    Check(drag.ScrollStep(view.content, scale) < 0,
          "top edge autoscroll missing");
    drag.End();
    Check(drag.ScrollStep(view.content, scale) == 0,
          "ended drag still scrolls");
  }
}

enum class ModalAction {
  kCancel,
  kConfirm,
  kEscape,
  kEnter,
  kTabEnter,
  kClose,
  kTags
};
ModalAction modal_action;
bool modal_checked = false;
bool modal_ok = false;
std::string modal_error;

HWND FindTestWindow(const wchar_t* class_name)
{
  struct Search {
    const wchar_t* name;
    HWND window;
  } search = {class_name, nullptr};
  EnumThreadWindows(
      GetCurrentThreadId(),
      [](HWND window, LPARAM context) -> BOOL
      {
        auto* search = reinterpret_cast<Search*>(context);
        wchar_t name[128] = {};
        GetClassNameW(window, name, 128);
        if (std::wstring(name) == search->name) {
          search->window = window;
          return FALSE;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&search));
  return search.window;
}

void CALLBACK DriveModal(HWND, UINT, UINT_PTR timer, DWORD)
{
  HWND window =
      FindTestWindow(modal_action == ModalAction::kTags ? L"NexusImportDialog"
                                                        : L"NexusDeleteDialog");
  if (window == nullptr) {
    return;
  }
  KillTimer(nullptr, timer);
  modal_checked = true;
  try {
    Check((GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION) == 0,
          "modal has system title bar");
    if (modal_action == ModalAction::kTags) {
      wchar_t text[128] = {};
      SendMessageW(window, WM_COMMAND, MAKEWPARAM(300, BN_CLICKED), 0);
      SendMessageW(window, WM_COMMAND, MAKEWPARAM(300, BN_CLICKED), 0);
      GetWindowTextW(GetDlgItem(window, 103), text, 128);
      Check(std::wstring(text) == L"白丝", "actual shortcut duplicated tag");
      SendMessageW(window, WM_COMMAND, MAKEWPARAM(111, BN_CLICKED), 0);
      GetWindowTextW(GetDlgItem(window, 300), text, 128);
      Check(std::wstring(text) == L"新增", "actual new preset button missing");
      Check(!IsWindowVisible(GetDlgItem(window, 301)),
            "last tag page has stale buttons");
      SendMessageW(window, WM_COMMAND, MAKEWPARAM(300, BN_CLICKED), 0);
      SendMessageW(window, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
    } else {
      Check(GetFocus() == GetDlgItem(window, IDCANCEL),
            "delete initial focus is unsafe");
      Check(!IsWindowEnabled(GetWindow(window, GW_OWNER)),
            "modal owner not disabled");
      if (modal_action == ModalAction::kEscape ||
          modal_action == ModalAction::kEnter ||
          modal_action == ModalAction::kTabEnter) {
        if (modal_action == ModalAction::kTabEnter) {
          MSG tab = {};
          tab.hwnd = GetFocus();
          tab.message = WM_KEYDOWN;
          tab.wParam = VK_TAB;
          IsDialogMessageW(window, &tab);
          Check(GetFocus() == GetDlgItem(window, IDOK),
                "Tab cannot reach delete confirmation");
        }
        PostMessageW(
            GetFocus(), WM_KEYDOWN,
            modal_action == ModalAction::kEscape ? VK_ESCAPE : VK_RETURN, 0);
      } else if (modal_action == ModalAction::kClose) {
        PostMessageW(window, WM_CLOSE, 0, 0);
      } else {
        SendMessageW(
            window, WM_COMMAND,
            MAKEWPARAM(modal_action == ModalAction::kConfirm ? IDOK : IDCANCEL,
                       BN_CLICKED),
            0);
      }
    }
    modal_ok = true;
  } catch (const std::exception& error) {
    modal_error = error.what();
    PostMessageW(window, WM_CLOSE, 0, 0);
  }
}

void ArmModal(ModalAction action)
{
  modal_action = action;
  modal_checked = false;
  modal_ok = false;
  modal_error.clear();
  Check(SetTimer(nullptr, 0, 40, DriveModal) != 0, "modal test timer failed");
}

void CALLBACK ChooseCurrentSort(HWND, UINT, UINT_PTR timer, DWORD)
{
  HWND popup = FindTestWindow(L"NexusSortPopup");
  if (popup != nullptr) {
    KillTimer(nullptr, timer);
    PostMessageW(popup, WM_KEYDOWN, VK_RETURN, 0);
  }
}

void TestListBoxScrollbar(HWND owner, bool native_scrollbar)
{
  HWND list = CreateWindowExW(
      0, L"LISTBOX", nullptr,
      WS_CHILD | WS_VISIBLE | (native_scrollbar ? WS_VSCROLL : 0) |
          LBS_OWNERDRAWFIXED | LBS_HASSTRINGS |
          LBS_NOINTEGRALHEIGHT,
      0, 0, 300, 148, owner, nullptr, nullptr, nullptr);
  Check(list != nullptr, "scrollbar test list creation");
  SendMessageW(list, LB_SETITEMHEIGHT, 0, 44);
  for (int i = 0; i < 20; ++i) {
    SendMessageW(list, LB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"preset tag"));
  }
  nexus::ui::dark::StyleListBox(list);
  try {
    auto check_bar = [list]()
    {
      Check((GetWindowLongPtrW(list, GWL_STYLE) & WS_VSCROLL) == 0,
            "native list scrollbar reappeared");
      RECT client = {};
      GetClientRect(list, &client);
      Check(client.right == 300, "native scrollbar reduced list client width");
    };
    check_bar();
    SendMessageW(list, WM_MOUSEWHEEL,
                 MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
    Check(SendMessageW(list, LB_GETTOPINDEX, 0, 0) > 0,
          "list wheel did not scroll");
    check_bar();
    SendMessageW(list, WM_MOUSEWHEEL,
                 MAKEWPARAM(0, static_cast<WORD>(-40 * WHEEL_DELTA)), 0);
    Check(SendMessageW(list, LB_GETTOPINDEX, 0, 0) == 17,
          "list wheel did not stop at bottom");
    check_bar();
    SendMessageW(list, WM_MOUSEWHEEL,
                 MAKEWPARAM(0, static_cast<WORD>(40 * WHEEL_DELTA)), 0);
    Check(SendMessageW(list, LB_GETTOPINDEX, 0, 0) == 0,
          "list wheel did not stop at top");
    check_bar();
    SendMessageW(list, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(292, 12));
    SendMessageW(list, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(292, 140));
    SendMessageW(list, WM_LBUTTONUP, 0, MAKELPARAM(292, 140));
    Check(SendMessageW(list, LB_GETTOPINDEX, 0, 0) == 17,
          "custom list thumb did not scroll to bottom");
    Check(GetCapture() != list, "list thumb drag left capture active");
    check_bar();
    SendMessageW(list, LB_SETTOPINDEX, 10, 0);
    check_bar();
    SendMessageW(list, WM_KEYDOWN, VK_END, 0);
    check_bar();
    SendMessageW(list, LB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"new tag"));
    check_bar();
    SendMessageW(list, LB_DELETESTRING, 0, 0);
    check_bar();
    SetWindowPos(list, nullptr, 0, 0, 300, 88, SWP_NOMOVE | SWP_NOZORDER);
    check_bar();
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    check_bar();
  } catch (...) {
    DestroyWindow(list);
    throw;
  }
  DestroyWindow(list);
}

void TestModals(HWND owner)
{
  for (bool permanent : {false, true}) {
    for (auto action :
         {ModalAction::kCancel, ModalAction::kConfirm, ModalAction::kEscape,
          ModalAction::kEnter, ModalAction::kTabEnter, ModalAction::kClose}) {
      ArmModal(action);
      const bool result = nexus::ui::DeleteDialog::Show(
          owner, L"2 个资源\n1 个视频，1 张图片\n总大小：8 MB", permanent);
      Check(modal_checked && modal_ok, modal_error.empty()
                                           ? "delete modal test not driven"
                                           : modal_error.c_str());
      Check(result == (action == ModalAction::kConfirm ||
                       action == ModalAction::kTabEnter),
            "delete cancellation/confirmation result");
      Check(IsWindowEnabled(owner), "delete modal failed to restore owner");
    }
  }
  for (bool edit : {false, true}) {
    ArmModal(ModalAction::kTags);
    const auto result = nexus::ui::ImportDialog::Show(
        owner, {L"fixture.mp4"}, {},
        {"白丝", "二", "三", "四", "五", "六", "新增"}, nullptr, edit);
    Check(modal_checked && modal_ok,
          modal_error.empty() ? "tag modal not driven" : modal_error.c_str());
    Check(result && result->tags == std::vector<std::string>({"白丝", "新增"}),
          "actual Metadata tag result");
  }
}

}  // namespace

namespace nexus::app {

class ApplicationInteractionTest {
 public:
  // An isolated fixture for visual inspection; these dialogs never delete
  // files.
  static void Preview()
  {
    Application app;
    const auto root =
        std::filesystem::temp_directory_path() /
        (L"nexus-ui-preview-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    app.exeDir_ = root;
    app.libraryRoot_ = root;
    WNDCLASSW window_class = {};
    window_class.lpfnWndProc = PreviewProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(window_class.hInstance,
                                  MAKEINTRESOURCEW(IDI_VIEWER));
    window_class.lpszClassName = L"NexusUiPreview";
    RegisterClassW(&window_class);
    app.hwnd_ = CreateWindowExW(
        0, window_class.lpszClassName, L"Viewer UI preview",
        WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
        30, 30, 1280, 900, nullptr, nullptr, window_class.hInstance, nullptr);
    SetWindowLongPtrW(app.hwnd_, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(&app));
    ui::dark::ApplyRoundedWindow(app.hwnd_);
    SetWindowPos(app.hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE);
    Check(app.renderer_.Init(app.hwnd_, 1), "preview renderer initialization");
    Check(
        app.database_.Open(root / L"preview.db") && db::Migrate(app.database_),
        "preview database");
    for (int i = 0; i < 80; ++i) {
      Resource resource;
      resource.path = root / (L"fixture-" + std::to_wstring(i) + L".mp4");
      resource.originalFilename = "fixture-" + std::to_string(i) + ".mp4";
      resource.title = i % 2 == 0 ? "测试视频 " + std::to_string(i)
                                  : "测试图片 " + std::to_string(i);
      resource.type = i % 2 == 0 ? ResourceType::kVideo : ResourceType::kImage;
      resource.duration = 135;
      Check(app.resources_.Insert(resource), "preview resource insert");
      app.tags_.Attach(resource.id, "示例标签");
    }
    app.settings_.set_presetTags(
        {"白丝", "二", "三", "四", "五", "六", "新增"});
    app.ReloadFromDatabase();
    app.Refilter();
    app.searchBar_.set_on_text_changed([&app] { app.Refilter(); });
    app.statusMessage_ =
        L"独立预览：Delete 删除框，Shift+Delete 永久删除框，F2 编辑，F3 "
        L"导入，F4 切换 DPI，F5 设置";
    ShowWindow(app.hwnd_, SW_SHOW);
    SetWindowPos(app.hwnd_, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    app.Shutdown();
    std::filesystem::remove_all(root);
  }

  static void Run()
  {
    Application app;
    const auto root =
        std::filesystem::temp_directory_path() /
        (L"nexus-ui-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    app.exeDir_ = root;
    app.hwnd_ =
        CreateWindowExW(0, L"STATIC", L"Nexus interaction fixture", WS_POPUP,
                        20, 20, 1200, 800, nullptr, nullptr, nullptr, nullptr);
    Check(app.hwnd_ != nullptr, "test window creation");
    try {
      Check(app.database_.Open(root / L"test.db") && db::Migrate(app.database_),
            "test database creation");
      Resource first, second;
      first.title = "alpha";
      first.path = root / L"alpha.mp4";
      first.originalFilename = "alpha.mp4";
      first.type = ResourceType::kVideo;
      first.hotScore = 10;
      second.title = "beta";
      second.path = root / L"beta.jpg";
      second.originalFilename = "beta.jpg";
      second.type = ResourceType::kImage;
      second.hotScore = 1;
      Check(app.resources_.Insert(first) && app.resources_.Insert(second),
            "test resource insert");
      Check(app.database_.Exec("UPDATE resources SET hot_score=10 WHERE "
                               "original_filename='alpha.mp4';"),
            "initial heat fixture");
      app.ReloadFromDatabase();
      app.Refilter();
      Check(app.sortMode_ == ui::ResourceSort::kHot &&
                Ids(app.filtered_) ==
                    std::vector<ResourceId>({first.id, second.id}),
            "startup heat order");
      Check(app.resources_.NoteOpen(second.id),
            "playback stats fixture update");
      app.ReloadFromDatabase();
      app.Refilter();
      Check(
          Ids(app.filtered_) == std::vector<ResourceId>({first.id, second.id}),
          "background reload reordered played media");
      app.sidebarFilter_ = Application::SidebarFilter::kImage;
      app.Refilter();
      Check(Ids(app.filtered_) == std::vector<ResourceId>({second.id}),
            "sidebar filtering");
      app.sidebarFilter_ = Application::SidebarFilter::kAll;
      app.Refilter();
      Check(
          Ids(app.filtered_) == std::vector<ResourceId>({first.id, second.id}),
          "sidebar reset reordered list");
      Check(app.database_.Exec("UPDATE resources SET hot_score=100 WHERE "
                               "original_filename='beta.jpg';"),
            "latest stats update");
      app.ReloadFromDatabase();
      Check(SetTimer(nullptr, 0, 40, ChooseCurrentSort) != 0,
            "sort test timer");
      app.ChooseSortMode();
      Check(
          Ids(app.filtered_) == std::vector<ResourceId>({second.id, first.id}),
          "explicit application sort");
      app.sortMode_ = ui::ResourceSort::kRelevance;
      app.searchBar_.SetText(L"alpha");
      app.Refilter();
      Check(Ids(app.filtered_) == std::vector<ResourceId>({first.id}),
            "new composite query ignored");
      app.searchBar_.SetText(L"beta");
      app.Refilter();
      Check(Ids(app.filtered_) == std::vector<ResourceId>({second.id}),
            "changed composite query ignored");
      app.searchBar_.Clear();
      app.Refilter();
      Check(
          app.database_.Exec("UPDATE resources SET title=title || ' shared';"),
          "shared search fixture");
      app.ReloadFromDatabase();
      app.searchBar_.SetText(L"shared");
      app.Refilter();
      const auto relevance_order = Ids(app.filtered_);
      Check(app.database_.Exec("UPDATE resources SET hot_score=1000 WHERE "
                               "original_filename='alpha.mp4';"),
            "changed search stats fixture");
      app.ReloadFromDatabase();
      app.Refilter();
      Check(Ids(app.filtered_) == relevance_order,
            "same search reordered after playback/refresh");
      app.searchBar_.SetText(L"alpha shared");
      app.Refilter();
      Check(app.filtered_.size() == 2 && app.filtered_.front()->id == first.id,
            "changed search did not recompute matching relevance");
      app.searchBar_.Clear();

      app.sortMode_ = ui::ResourceSort::kHot;
      app.sidebarFilter_ = Application::SidebarFilter::kBrowse;
      Resource parent, folder;
      parent.id = 90;
      parent.type = ResourceType::kFolder;
      parent.originalFilename = "..";
      folder.id = 91;
      folder.type = ResourceType::kFolder;
      folder.originalFilename = "folder";
      first.hotScore = 10;
      second.hotScore = 100;
      app.browseItems_ = {first, folder, second, parent};
      app.browsePath_ = root / L"a";
      app.Refilter();
      Check(Ids(app.filtered_) ==
                std::vector<ResourceId>({90, 91, second.id, first.id}),
            "browse ignored selected sorting");
      app.browseItems_[0].hotScore = 200;
      app.Refilter();
      Check(app.filtered_[2]->id == second.id,
            "browse stats refresh reordered existing items");
      app.browsePath_ = root / L"b";
      app.Refilter();
      Check(app.filtered_[2]->id == first.id,
            "new browse directory did not initialize sorting");
      app.browsePath_ = root / L"a";
      app.Refilter();
      Check(app.filtered_[2]->id == second.id,
            "returning to directory lost order snapshot");
      app.sidebarFilter_ = Application::SidebarFilter::kAll;

      // Exercise real message handlers and timer cleanup without launching
      // players.
      app.items_.resize(80);
      for (std::size_t i = 0; i < app.items_.size(); ++i) {
        app.items_[i].id = i + 100;
      }
      app.filtered_.clear();
      for (const auto& item : app.items_) {
        app.filtered_.push_back(&item);
      }
      const auto content = app.ContentRect();
      const POINT start = {static_cast<LONG>(content.left + 2),
                           static_cast<LONG>(content.top + 2)};
      const POINT end = {static_cast<LONG>(content.right - 30),
                         static_cast<LONG>(content.bottom - 1)};
      app.selectedId_ = 179;
      app.selectedIds_ = {179};
      app.selectionDrag_.Begin(start, content, 0, true, app.selectedIds_,
                               app.selectedId_);
      SetCapture(app.hwnd_);
      app.UpdateDragSelection(end);
      Check(app.selectionTimerRunning_, "edge timer did not start");
      const float before = app.scrollY_;
      app.HandleMessage(WM_TIMER, Application::kSelectionTimer, 0);
      Check(app.scrollY_ > before && app.selectedIds_.contains(179),
            "edge tick/Ctrl selection");
      POINT wheel = end;
      ClientToScreen(app.hwnd_, &wheel);
      app.HandleMessage(WM_MOUSEWHEEL, MAKEWPARAM(MK_LBUTTON, -WHEEL_DELTA),
                        MAKELPARAM(wheel.x, wheel.y));
      Check(app.scrollY_ > before + 50, "held-button wheel did not scroll");
      app.HandleMessage(WM_KEYDOWN, VK_ESCAPE, 0);
      Check(!app.selectionDrag_.active() && !app.selectionTimerRunning_ &&
                app.selectedIds_ == std::unordered_set<ResourceId>({179}) &&
                GetCapture() != app.hwnd_,
            "Esc did not restore selection/stop capture/timer");
      app.selectionDrag_.Begin(start, content, app.scrollY_, false,
                               app.selectedIds_, app.selectedId_);
      app.UpdateDragSelection(end);
      app.HandleMessage(WM_CAPTURECHANGED, 0, 0);
      Check(!app.selectionDrag_.active() && !app.selectionTimerRunning_ &&
                app.selectedIds_.contains(179),
            "capture loss did not cancel drag");
      app.selectionDrag_.Begin(start, content, app.scrollY_, false,
                               app.selectedIds_, app.selectedId_);
      app.UpdateDragSelection(end);
      app.HandleMessage(WM_LBUTTONUP, 0, MAKELPARAM(end.x, end.y));
      const auto committed = app.selectedIds_;
      app.HandleMessage(WM_CAPTURECHANGED, 0, 0);
      Check(!app.selectionDrag_.active() && !app.selectionTimerRunning_ &&
                app.selectedIds_ == committed,
            "release capture undid committed selection");
      app.scrollY_ = 1e9f;
      app.ClampScroll();
      app.selectionDrag_.Begin(start, content, app.scrollY_, false,
                               app.selectedIds_, app.selectedId_);
      app.UpdateDragSelection(end);
      Check(!app.selectionTimerRunning_,
            "autoscroll timer active at lower bound");
      app.EndDragSelection(true);
      app.scrollY_ = 0;
      app.selectionDrag_.Begin(start, content, 0, false, app.selectedIds_,
                               app.selectedId_);
      app.UpdateDragSelection(start);
      Check(!app.selectionTimerRunning_,
            "autoscroll timer active at upper bound");
      app.ReloadFromDatabase();
      app.Refilter();
      Check(!app.selectionDrag_.active() && !app.selectionTimerRunning_,
            "refresh left drag active");
      TestListBoxScrollbar(app.hwnd_, false);
      TestListBoxScrollbar(app.hwnd_, true);
      TestModals(app.hwnd_);
    } catch (...) {
      app.Shutdown();
      std::filesystem::remove_all(root);
      throw;
    }
    app.Shutdown();
    std::filesystem::remove_all(root);
  }

 private:
  static LRESULT CALLBACK PreviewProc(HWND hwnd, UINT message, WPARAM wp,
                                      LPARAM lp)
  {
    auto* app =
        reinterpret_cast<Application*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (app == nullptr) {
      return DefWindowProcW(hwnd, message, wp, lp);
    }
    if (message == WM_KEYDOWN && wp == VK_DELETE) {
      const bool permanent = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
      ui::DeleteDialog::Show(
          hwnd,
          std::wstring(permanent ? L"永久删除 " : L"移到回收站：") +
              L"2 个资源？\n\n1 个视频，1 张图片\n总大小：8 MB" +
              (permanent ? L"\n\n此操作不可恢复。" : L""),
          permanent);
      return 0;
    }
    if (message == WM_KEYDOWN && (wp == VK_F2 || wp == VK_F3)) {
      ui::ImportDialog::Show(hwnd, {L"fixture.mp4"}, {},
                             app->settings_.presetTags(), nullptr, wp == VK_F2);
      return 0;
    }
    if (message == WM_KEYDOWN && wp == VK_F4) {
      app->EndDragSelection(true);
      app->scale_ = app->scale_ == 1 ? 1.5f : app->scale_ == 1.5f ? 2.0f : 1.0f;
      app->renderer_.SetScale(app->scale_);
      app->ClampScroll();
      app->Invalidate();
      return 0;
    }
    if (message == WM_KEYDOWN && wp == VK_F5) {
      ui::SettingsDialog::Show(hwnd, &app->settings_);
      return 0;
    }
    return app->HandleMessage(message, wp, lp);
  }
};

}  // namespace nexus::app

int main(int argc, char** argv)
{
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  try {
    if ((argc == 2 && std::string(argv[1]) == "--preview") ||
        std::filesystem::path(argv[0]).stem() == L"nexus_ui_preview") {
      nexus::app::ApplicationInteractionTest::Preview();
      CoUninitialize();
      return 0;
    }
    TestTags();
    TestOrdering();
    TestDragGeometry();
    nexus::app::ApplicationInteractionTest::Run();
    std::cout << "tags/order/selection/dialog regression tests passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    CoUninitialize();
    return 1;
  }
  CoUninitialize();
  return 0;
}

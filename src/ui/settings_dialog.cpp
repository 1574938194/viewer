#include "ui/settings_dialog.h"

#include <windowsx.h>

#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

#include "ui/dark_controls.h"
#include "util/string_util.h"

namespace nexus::ui {
namespace {

constexpr int kVideoId = 201;
constexpr int kImageId = 202;
constexpr int kBrowseVideoId = 203;
constexpr int kBrowseImageId = 204;
constexpr int kDefaultsId = 205;
constexpr int kDeleteSourceId = 206;
constexpr int kLibraryId = 207;
constexpr int kBrowseLibraryId = 208;
constexpr int kAskMetadataId = 209;
constexpr int kPresetTagListId = 210;
constexpr int kPresetTagEditId = 211;
constexpr int kAddPresetTagId = 212;
constexpr int kRemovePresetTagId = 213;
constexpr int kClearCacheId = 214;

// Doubled layout constants relative to the original 650x330 dialog.
constexpr int kWindowWidth = 1300;
constexpr int kWindowHeight = 900;
constexpr int kRowHeight = 52;

HMENU ControlId(int id)
{
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

struct DialogState {
    HWND owner = nullptr;
    app::Settings* settings = nullptr;
    bool* clearCacheRequested = nullptr;
    bool accepted = false;
    bool clearCache = false;
    bool done = false;
    bool closeHover = false;
    bool tracking = false;
};

std::wstring Text(HWND control)
{
    const int length = GetWindowTextLengthW(control);
    std::wstring result(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, result.data(), length + 1);
    result.resize(static_cast<size_t>(length));
    return result;
}

std::wstring TrimTag(std::wstring value)
{
    while (!value.empty() && iswspace(value.front()))
    {
        value.erase(value.begin());
    }
    while (!value.empty() && iswspace(value.back()))
    {
        value.pop_back();
    }
    return value;
}

void PopulatePresetTags(HWND list, const std::vector<std::string>& tags)
{
    if (list == nullptr)
    {
        return;
    }
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (const std::string& tag : tags)
    {
        const std::wstring wide = Utf8ToWide(tag);
        if (!wide.empty())
        {
            SendMessageW(list, LB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(wide.c_str()));
        }
    }
}

std::vector<std::string> ReadPresetTags(HWND list)
{
    std::vector<std::string> tags;
    if (list == nullptr)
    {
        return tags;
    }
    const LRESULT count = SendMessageW(list, LB_GETCOUNT, 0, 0);
    for (LRESULT i = 0; i < count; ++i)
    {
        const LRESULT length = SendMessageW(list, LB_GETTEXTLEN, i, 0);
        if (length <= 0)
        {
            continue;
        }
        std::wstring value(static_cast<size_t>(length) + 1, L'\0');
        SendMessageW(list, LB_GETTEXT, i,
                     reinterpret_cast<LPARAM>(value.data()));
        value.resize(static_cast<size_t>(length));
        value = TrimTag(std::move(value));
        if (!value.empty())
        {
            tags.push_back(WideToUtf8(value));
        }
    }
    return tags;
}

void CommitPresetTagEdit(HWND window)
{
    if (window == nullptr)
    {
        return;
    }
    HWND list = GetDlgItem(window, kPresetTagListId);
    HWND edit = GetDlgItem(window, kPresetTagEditId);
    if (list == nullptr || edit == nullptr)
    {
        return;
    }
    const LRESULT selected = SendMessageW(list, LB_GETCURSEL, 0, 0);
    if (selected < 0)
    {
        return;
    }
    const std::wstring value = TrimTag(Text(edit));
    if (value.empty())
    {
        return;
    }
    LRESULT existing = SendMessageW(
        list, LB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
        reinterpret_cast<LPARAM>(value.c_str()));
    if (existing >= 0 && existing != selected)
    {
        SendMessageW(list, LB_DELETESTRING, selected, 0);
        SendMessageW(list, LB_SETCURSEL, existing > selected ? existing - 1
                                                               : existing,
                     0);
        return;
    }
    SendMessageW(list, LB_DELETESTRING, selected, 0);
    const LRESULT replacement = SendMessageW(
        list, LB_INSERTSTRING, selected,
        reinterpret_cast<LPARAM>(value.c_str()));
    if (replacement >= 0)
    {
        SendMessageW(list, LB_SETCURSEL, replacement, 0);
    }
}

HWND AddLabel(HWND parent, const wchar_t* text, int x, int y, int width,
              int height, bool secondary = false)
{
    HWND control = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                                   x, y, width, height, parent, nullptr,
                                   nullptr, nullptr);
    if (secondary)
    {
        dark::MarkSecondary(control);
    }
    return control;
}

HWND AddEdit(HWND parent, const wchar_t* initial, int id, int x, int y,
             int width, int height)
{
    HWND control = CreateWindowExW(
        0, L"EDIT", initial,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, x, y, width,
        height, parent, ControlId(id), nullptr, nullptr);
    SendMessageW(control, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(12, 12));
    return control;
}

HWND AddButton(HWND parent, const wchar_t* text, int id, int x, int y,
               int width, int height, bool accent = false)
{
    HWND control = CreateWindowExW(
        0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, x, y, width,
        height, parent, ControlId(id), nullptr, nullptr);
    dark::StyleButton(control, false);
    if (accent)
    {
        dark::MarkAccent(control);
    }
    return control;
}

void Browse(HWND owner, HWND edit)
{
    // The modern COM picker is considerably larger than the legacy
    // GetOpenFileName dialog and keeps the same shell navigation used by the
    // managed-library folder picker.  Keep a legacy fallback for older
    // Windows shells or unusual COM registration failures.
    IFileOpenDialog* dialog = nullptr;
    const HRESULT created = CoCreateInstance(
        CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));
    if (SUCCEEDED(created) && dialog != nullptr)
    {
        COMDLG_FILTERSPEC filters[] = {
            {L"应用程序 (*.exe)", L"*.exe"},
            {L"所有文件", L"*.*"},
        };
        dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
        dialog->SetTitle(L"选择播放器或图片查看器");
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST |
                            FOS_PATHMUSTEXIST);
        const std::wstring current = Text(edit);
        if (!current.empty())
        {
            IShellItem* folder = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(
                    current.c_str(), nullptr, IID_PPV_ARGS(&folder))))
            {
                dialog->SetFolder(folder);
                folder->Release();
            }
        }
        if (SUCCEEDED(dialog->Show(owner)))
        {
            IShellItem* result = nullptr;
            if (SUCCEEDED(dialog->GetResult(&result)) && result != nullptr)
            {
                PWSTR path = nullptr;
                if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH,
                                                      &path)) &&
                    path != nullptr)
                {
                    SetWindowTextW(edit, path);
                    CoTaskMemFree(path);
                }
                result->Release();
            }
        }
        dialog->Release();
        return;
    }

    wchar_t path[MAX_PATH] = {};
    const std::wstring current = Text(edit);
    wcsncpy_s(path, current.c_str(), _TRUNCATE);
    OPENFILENAMEW legacy = {};
    legacy.lStructSize = sizeof(legacy);
    legacy.hwndOwner = owner;
    legacy.lpstrFilter = L"应用程序 (*.exe)\0*.exe\0所有文件\0*.*\0";
    legacy.lpstrFile = path;
    legacy.nMaxFile = MAX_PATH;
    legacy.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&legacy))
    {
        SetWindowTextW(edit, path);
    }
}

void BrowseFolder(HWND owner, HWND edit)
{
    BROWSEINFOW dialog = {};
    dialog.hwndOwner = owner;
    dialog.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    dialog.lpszTitle = L"选择 Viewer 托管库目录";
    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&dialog);
    if (item == nullptr)
    {
        return;
    }
    wchar_t path[MAX_PATH] = {};
    if (SHGetPathFromIDListW(item, path))
    {
        SetWindowTextW(edit, path);
    }
    CoTaskMemFree(item);
}

void CloseSettingsWindow(HWND window, DialogState* state)
{
    if (window == nullptr || state == nullptr)
    {
        return;
    }
    state->done = true;
    // Re-enable and foreground the owner before destroying the modal popup.
    // Destroying the last active popup while its owner is disabled lets
    // Windows activate the next top-level window (often Explorer) for one
    // frame, which is the flash users were seeing.
    if (state->owner != nullptr && IsWindow(state->owner))
    {
        EnableWindow(state->owner, TRUE);
        BringWindowToTop(state->owner);
        SetForegroundWindow(state->owner);
        SetActiveWindow(state->owner);
    }
    ShowWindow(window, SW_HIDE);
    DestroyWindow(window);
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
{
    auto* state = reinterpret_cast<DialogState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        state = static_cast<DialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr)
    {
        return DefWindowProcW(hwnd, message, wp, lp);
    }
    switch (message)
    {
    case WM_NCCALCSIZE:
        // Remove the system caption; the header is app-drawn.
        if (wp != FALSE)
        {
            return 0;
        }
        break;
    case WM_NCACTIVATE:
        // The frame is entirely owner-drawn.  Suppress the default non-client
        // repaint, which can briefly expose the window behind this popup when
        // focus moves between the owner and the dialog.
        return TRUE;
    case WM_NCHITTEST:
        return dark::BorderlessHitTest(hwnd, lp);
    case WM_MOUSEMOVE:
    {
        POINT point = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const bool hover = dark::CloseButtonHit(hwnd, point);
        if (hover != state->closeHover)
        {
            state->closeHover = hover;
            dark::InvalidateCloseButton(hwnd);
        }
        if (!state->tracking)
        {
            TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
            state->tracking = TrackMouseEvent(&track) != FALSE;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        state->tracking = false;
        if (state->closeHover)
        {
            state->closeHover = false;
            dark::InvalidateCloseButton(hwnd);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (dark::CloseButtonHit(hwnd, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}))
        {
            CloseSettingsWindow(hwnd, state);
            return 0;
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case kBrowseVideoId:
            Browse(hwnd, GetDlgItem(hwnd, kVideoId));
            return 0;
        case kBrowseImageId:
            Browse(hwnd, GetDlgItem(hwnd, kImageId));
            return 0;
        case kBrowseLibraryId:
            BrowseFolder(hwnd, GetDlgItem(hwnd, kLibraryId));
            return 0;
        case kAddPresetTagId:
        {
            HWND edit = GetDlgItem(hwnd, kPresetTagEditId);
            HWND list = GetDlgItem(hwnd, kPresetTagListId);
            if (list == nullptr || edit == nullptr)
            {
                return 0;
            }
            // Clicking 添加 always creates/selects an editable placeholder.
            // The selected row is mirrored into the edit control below the
            // list, so the user can immediately replace 新建标签 with the
            // desired name before saving.
            const wchar_t placeholder[] = L"新建标签";
            LRESULT index = SendMessageW(
                list, LB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                reinterpret_cast<LPARAM>(placeholder));
            if (index < 0)
            {
                index = SendMessageW(
                    list, LB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(placeholder));
            }
            if (index >= 0)
            {
                SendMessageW(list, LB_SETCURSEL, index, 0);
                SetWindowTextW(edit, placeholder);
                SetFocus(edit);
            }
            return 0;
        }
        case kRemovePresetTagId:
        {
            HWND list = GetDlgItem(hwnd, kPresetTagListId);
            const LRESULT selected = SendMessageW(list, LB_GETCURSEL, 0, 0);
            if (selected >= 0)
            {
                SendMessageW(list, LB_DELETESTRING, selected, 0);
            }
            return 0;
        }
        case kPresetTagListId:
            if (HIWORD(wp) == LBN_SELCHANGE)
            {
                HWND list = GetDlgItem(hwnd, kPresetTagListId);
                HWND edit = GetDlgItem(hwnd, kPresetTagEditId);
                const LRESULT selected =
                    SendMessageW(list, LB_GETCURSEL, 0, 0);
                if (selected >= 0 && edit != nullptr)
                {
                    const LRESULT length =
                        SendMessageW(list, LB_GETTEXTLEN, selected, 0);
                    if (length >= 0)
                    {
                        std::wstring value(static_cast<size_t>(length) + 1,
                                           L'\0');
                        SendMessageW(list, LB_GETTEXT, selected,
                                     reinterpret_cast<LPARAM>(value.data()));
                        value.resize(static_cast<size_t>(length));
                        SetWindowTextW(edit, value.c_str());
                    }
                }
            }
            return 0;
        case kDefaultsId:
            SetWindowTextW(GetDlgItem(hwnd, kVideoId), L"");
            SetWindowTextW(GetDlgItem(hwnd, kImageId), L"");
            SetWindowTextW(GetDlgItem(hwnd, kLibraryId), L"");
            dark::SetChecked(GetDlgItem(hwnd, kDeleteSourceId), true);
            dark::SetChecked(GetDlgItem(hwnd, kAskMetadataId), true);
            PopulatePresetTags(
                GetDlgItem(hwnd, kPresetTagListId),
                {"白丝", "美少女", "萝莉", "3p", "肛交", "内射"});
            return 0;
        case kClearCacheId:
            if (MessageBoxW(
                    hwnd,
                    L"将清空 SQLite 索引和缩略图缓存，并从托管库重新构建。\n"
                    L"托管库中的原文件不会被删除。\n\n继续吗？",
                    L"清空缓存并重建", MB_YESNO | MB_ICONWARNING) == IDYES)
            {
                state->clearCache = true;
                state->accepted = true;
                CloseSettingsWindow(hwnd, state);
            }
            return 0;
        case kDeleteSourceId:
            return 0;
        case IDOK:
            CommitPresetTagEdit(hwnd);
            state->settings->set_videoPlayer(Text(GetDlgItem(hwnd, kVideoId)));
            state->settings->set_imageViewer(Text(GetDlgItem(hwnd, kImageId)));
            state->settings->set_managedLibrary(
                Text(GetDlgItem(hwnd, kLibraryId)));
            state->settings->set_deleteSourceAfterImport(
                dark::IsChecked(GetDlgItem(hwnd, kDeleteSourceId)));
            state->settings->set_askMetadataOnDrop(
                dark::IsChecked(GetDlgItem(hwnd, kAskMetadataId)));
            state->settings->set_presetTags(
                ReadPresetTags(GetDlgItem(hwnd, kPresetTagListId)));
            state->accepted = true;
            CloseSettingsWindow(hwnd, state);
            return 0;
        case IDCANCEL:
            CloseSettingsWindow(hwnd, state);
            return 0;
        default:
            return 0;
        }
    case WM_ERASEBKGND:
    {
        HDC dc = reinterpret_cast<HDC>(wp);
        RECT client = {};
        GetClientRect(hwnd, &client);
        dark::PaintPanelBackground(hwnd, dc, client);
        return 1;
    }
    case WM_PAINT:
    {
        PAINTSTRUCT ps = {};
        HDC dc = BeginPaint(hwnd, &ps);
        dark::PaintBorderlessFrame(hwnd, dc, L"Viewer 设置",
                                   state->closeHover);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DRAWITEM:
        dark::HandleDrawItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
        return TRUE;
    case WM_MEASUREITEM:
        dark::HandleMeasureItem(reinterpret_cast<MEASUREITEMSTRUCT*>(lp));
        return TRUE;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
        return reinterpret_cast<LRESULT>(dark::HandleCtlColor(
            reinterpret_cast<HWND>(lp), reinterpret_cast<HDC>(wp), message));
    case WM_CLOSE:
        CloseSettingsWindow(hwnd, state);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wp, lp);
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

}  // namespace

bool SettingsDialog::Show(HWND owner, app::Settings* settings,
                           bool* clearCacheRequested)
{
    if (clearCacheRequested != nullptr)
    {
        *clearCacheRequested = false;
    }
    static bool registered = false;
    if (!registered)
    {
        WNDCLASSW windowClass = {};
        windowClass.lpfnWndProc = SettingsProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = nullptr;
        windowClass.lpszClassName = L"NexusSettingsDialog";
        registered = RegisterClassW(&windowClass) != 0 ||
                     GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    if (!registered || settings == nullptr)
    {
        return false;
    }
    DialogState state;
    state.owner = owner;
    state.settings = settings;
    state.clearCacheRequested = clearCacheRequested;
    RECT ownerRect = {};
    GetWindowRect(owner, &ownerRect);
    const int x = ownerRect.left + std::max(
        0, static_cast<int>((ownerRect.right - ownerRect.left) -
                            kWindowWidth) / 2);
    const int y = ownerRect.top + std::max(
        0, static_cast<int>((ownerRect.bottom - ownerRect.top) -
                            kWindowHeight) / 2);
    HWND window = CreateWindowExW(
        WS_EX_TOOLWINDOW, L"NexusSettingsDialog", L"Viewer 设置",
        WS_POPUP | WS_SYSMENU,
        x, y, kWindowWidth, kWindowHeight, owner,
        nullptr, GetModuleHandleW(nullptr), &state);
    if (window == nullptr)
    {
        return false;
    }
    dark::ApplyRoundedWindow(window);
    // WM_NCCALCSIZE already ran during CreateWindowExW with the default
    // handler; recalculate now that the dialog proc is wired up.
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE);
    AddLabel(window, L"视频播放器", 40, 132, 200, 44);
    AddEdit(window, settings->videoPlayer().c_str(), kVideoId, 250, 124, 800,
            kRowHeight);
    AddButton(window, L"浏览...", kBrowseVideoId, 1070, 124, 190, kRowHeight);
    AddLabel(window, L"图片查看器", 40, 224, 200, 44);
    AddEdit(window, settings->imageViewer().c_str(), kImageId, 250, 216, 800,
            kRowHeight);
    AddButton(window, L"浏览...", kBrowseImageId, 1070, 216, 190, kRowHeight);
    AddLabel(window, L"托管库目录", 40, 316, 200, 44);
    AddEdit(window, settings->managedLibrary().c_str(), kLibraryId, 250, 308,
            800, kRowHeight);
    AddButton(window, L"浏览...", kBrowseLibraryId, 1070, 308, 190,
              kRowHeight);
    AddLabel(window, L"播放器/查看器留空时使用 Windows 默认文件关联；托管库留空时使用程序目录。",
             250, 372, 1000, 40, true);
    HWND deleteSource = CreateWindowExW(
        0, L"BUTTON", L"导入成功后移入回收站删除原文件",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 250, 424, 620, 52,
        window, ControlId(kDeleteSourceId), nullptr, nullptr);
    dark::StyleButton(deleteSource, true);
    dark::MarkToggle(deleteSource);
    dark::SetChecked(deleteSource, settings->deleteSourceAfterImport());
    HWND askMetadata = CreateWindowExW(
        0, L"BUTTON", L"拖放文件/文件夹时询问 Metadata",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 250, 484, 620, 52,
        window, ControlId(kAskMetadataId), nullptr, nullptr);
    dark::StyleButton(askMetadata, true);
    dark::MarkToggle(askMetadata);
    dark::SetChecked(askMetadata, settings->askMetadataOnDrop());
    AddLabel(window, L"预设标签", 40, 564, 200, 44);
    HWND presetTagList = CreateWindowExW(
        0, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY |
            LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
        250, 548, 1010, 148, window, ControlId(kPresetTagListId), nullptr,
        nullptr);
    PopulatePresetTags(presetTagList, settings->presetTags());
    dark::StyleListBox(presetTagList);
    HWND presetTagEdit = AddEdit(window, L"", kPresetTagEditId, 250, 712, 500,
                                 kRowHeight);
    AddButton(window, L"添加", kAddPresetTagId, 770, 712, 190, kRowHeight);
    AddButton(window, L"删除选中", kRemovePresetTagId, 970, 712, 290,
              kRowHeight);
    AddLabel(window, L"拖放导入时可在 Metadata 窗口中快速选择这些标签。",
             250, 772, 1000, 36, true);
    (void)presetTagEdit;
    AddButton(window, L"清空缓存并重建", kClearCacheId, 40, 820, 360, 56);
    AddButton(window, L"恢复系统默认", kDefaultsId, 420, 820, 240, 56);
    AddButton(window, L"取消", IDCANCEL, 870, 820, 190, 56);
    AddButton(window, L"保存", IDOK, 1080, 820, 190, 56, true);
    dark::ApplyFontToChildren(window);

    EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
    SetWindowPos(window, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(window);
    SetActiveWindow(window);
    MSG message = {};
    while (!state.done && GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(window, &message))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(owner, TRUE);
    BringWindowToTop(owner);
    SetActiveWindow(owner);
    if (clearCacheRequested != nullptr)
    {
        *clearCacheRequested = state.clearCache;
    }
    return state.accepted;
}

}  // namespace nexus::ui

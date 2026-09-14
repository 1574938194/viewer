#include "ui/settings_dialog.h"

#include <windowsx.h>

#include <commdlg.h>

#include <algorithm>
#include <string>

#include "ui/dark_controls.h"

namespace nexus::ui {
namespace {

constexpr int kVideoId = 201;
constexpr int kImageId = 202;
constexpr int kBrowseVideoId = 203;
constexpr int kBrowseImageId = 204;
constexpr int kDefaultsId = 205;
constexpr int kDeleteSourceId = 206;

// Doubled layout constants relative to the original 650x330 dialog.
constexpr int kWindowWidth = 1300;
constexpr int kWindowHeight = 560;
constexpr int kRowHeight = 52;

HMENU ControlId(int id)
{
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

struct DialogState {
    HWND owner = nullptr;
    app::Settings* settings = nullptr;
    bool accepted = false;
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
    wchar_t path[MAX_PATH] = {};
    const std::wstring current = Text(edit);
    wcsncpy_s(path, current.c_str(), _TRUNCATE);
    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = L"应用程序 (*.exe)\0*.exe\0所有文件\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&dialog))
    {
        SetWindowTextW(edit, path);
    }
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
    case WM_NCHITTEST:
        return dark::BorderlessHitTest(hwnd, lp);
    case WM_MOUSEMOVE:
    {
        POINT point = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const bool hover = dark::CloseButtonHit(hwnd, point);
        if (hover != state->closeHover)
        {
            state->closeHover = hover;
            InvalidateRect(hwnd, nullptr, FALSE);
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
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (dark::CloseButtonHit(hwnd, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}))
        {
            state->done = true;
            DestroyWindow(hwnd);
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
        case kDefaultsId:
            SetWindowTextW(GetDlgItem(hwnd, kVideoId), L"");
            SetWindowTextW(GetDlgItem(hwnd, kImageId), L"");
            return 0;
        case kDeleteSourceId:
            return 0;
        case IDOK:
            state->settings->set_videoPlayer(Text(GetDlgItem(hwnd, kVideoId)));
            state->settings->set_imageViewer(Text(GetDlgItem(hwnd, kImageId)));
            state->settings->set_deleteSourceAfterImport(
                dark::IsChecked(GetDlgItem(hwnd, kDeleteSourceId)));
            state->accepted = true;
            state->done = true;
            DestroyWindow(hwnd);
            return 0;
        case IDCANCEL:
            state->done = true;
            DestroyWindow(hwnd);
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
        dark::PaintBorderlessFrame(hwnd, dc, L"Nexus 设置",
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
        state->done = true;
        DestroyWindow(hwnd);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wp, lp);
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

}  // namespace

bool SettingsDialog::Show(HWND owner, app::Settings* settings)
{
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
    RECT ownerRect = {};
    GetWindowRect(owner, &ownerRect);
    const int x = ownerRect.left + std::max(
        0, static_cast<int>((ownerRect.right - ownerRect.left) -
                            kWindowWidth) / 2);
    const int y = ownerRect.top + std::max(
        0, static_cast<int>((ownerRect.bottom - ownerRect.top) -
                            kWindowHeight) / 2);
    HWND window = CreateWindowExW(
        0, L"NexusSettingsDialog", L"Nexus 设置", WS_POPUP | WS_SYSMENU,
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
    AddLabel(window, L"路径留空时使用 Windows 默认文件关联。", 250, 288, 900,
             40, true);
    HWND deleteSource = CreateWindowExW(
        0, L"BUTTON", L"导入成功后移入回收站删除原文件",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 250, 344, 620, 52,
        window, ControlId(kDeleteSourceId), nullptr, nullptr);
    dark::StyleButton(deleteSource, true);
    dark::MarkToggle(deleteSource);
    dark::SetChecked(deleteSource, settings->deleteSourceAfterImport());
    AddButton(window, L"恢复系统默认", kDefaultsId, 40, 452, 240, 56);
    AddButton(window, L"取消", IDCANCEL, 870, 452, 190, 56);
    AddButton(window, L"保存", IDOK, 1080, 452, 190, 56, true);
    dark::ApplyFontToChildren(window);

    EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
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
    SetForegroundWindow(owner);
    return state.accepted;
}

}  // namespace nexus::ui

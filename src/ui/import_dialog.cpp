#include "ui/import_dialog.h"

#include <windowsx.h>

#include <algorithm>
#include <string>

#include "ui/dark_controls.h"
#include "util/string_util.h"
#include "util/tag_values.h"

namespace nexus::ui {
namespace {

constexpr int kTitleId = 101;
constexpr int kCategoryId = 102;
constexpr int kTagsId = 103;
constexpr int kActorsId = 104;
constexpr int kFavoriteId = 105;
constexpr int kRecentTagBaseId = 300;
constexpr int kPreviousTagsId = 110;
constexpr int kNextTagsId = 111;
constexpr int kTagPageId = 112;

// Doubled layout constants relative to the original 520x430 dialog.
constexpr int kWindowWidth = 1040;
constexpr int kWindowHeight = 780;
constexpr int kRowHeight = 52;

constexpr wchar_t kNoCategory[] = L"（无分类）";

HMENU ControlId(int id)
{
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

std::vector<std::string> SplitValues(const std::wstring& value)
{
    std::vector<std::string> result;
    std::wstring token;
    for (wchar_t character : value)
    {
        if (character == L',' || character == L';' || character == L'，')
        {
            while (!token.empty() && iswspace(token.front()))
            {
                token.erase(token.begin());
            }
            while (!token.empty() && iswspace(token.back()))
            {
                token.pop_back();
            }
            if (!token.empty())
            {
                result.push_back(WideToUtf8(token));
                token.clear();
            }
        }
        else
        {
            token.push_back(character);
        }
    }
    while (!token.empty() && iswspace(token.front()))
    {
        token.erase(token.begin());
    }
    while (!token.empty() && iswspace(token.back()))
    {
        token.pop_back();
    }
    if (!token.empty())
    {
        result.push_back(WideToUtf8(token));
    }
    return result;
}

std::wstring ControlText(HWND control)
{
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

class DialogState {
public:
    HWND owner = nullptr;
    HWND window = nullptr;
    const wchar_t* title = L"导入资源";
    const std::vector<std::filesystem::path>* files = nullptr;
    const std::vector<std::string>* categories = nullptr;
    const std::vector<std::string>* quickTags = nullptr;
    std::optional<service::ImportMetadata> result;
    bool done = false;
    bool closeHover = false;
    bool tracking = false;
    HWND tagEdit = nullptr;
    std::size_t tagPage = 0;
};

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

void UpdateQuickTagPage(DialogState* state)
{
    const auto page = QuickTagPage(*state->quickTags, state->tagPage);
    for (std::size_t slot = 0; slot < kQuickTagsPerPage; ++slot)
    {
        HWND button = GetDlgItem(state->window,
                                 kRecentTagBaseId + static_cast<int>(slot));
        const bool present = slot < page.size();
        SetWindowTextW(button, present ? Utf8ToWide(page[slot]).c_str() : L"");
        ShowWindow(button, present ? SW_SHOW : SW_HIDE);
    }
    const std::size_t pages = QuickTagPageCount(state->quickTags->size());
    const std::wstring label = std::to_wstring(state->tagPage + 1) + L" / " +
                               std::to_wstring(pages);
    SetWindowTextW(GetDlgItem(state->window, kTagPageId), label.c_str());
    EnableWindow(GetDlgItem(state->window, kPreviousTagsId), state->tagPage > 0);
    EnableWindow(GetDlgItem(state->window, kNextTagsId), state->tagPage + 1 < pages);
}

void CloseImportWindow(HWND window, DialogState* state)
{
    if (window == nullptr || state == nullptr)
    {
        return;
    }
    state->done = true;
    // Restore the owner before destroying the modal popup so activation never
    // falls through to an unrelated Explorer window for a frame.
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

LRESULT CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
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
        // Keep the custom rounded frame stable while controls receive focus;
        // the default non-client painter is not allowed to flash through.
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
            CloseImportWindow(hwnd, state);
            return 0;
        }
        break;
    case WM_COMMAND:
    {
        const int command = LOWORD(wp);
        if (HIWORD(wp) != BN_CLICKED)
        {
            return 0;
        }
        if (command == kPreviousTagsId || command == kNextTagsId)
        {
            const std::size_t pages = QuickTagPageCount(state->quickTags->size());
            if (command == kPreviousTagsId && state->tagPage > 0)
                --state->tagPage;
            if (command == kNextTagsId && state->tagPage + 1 < pages)
                ++state->tagPage;
            UpdateQuickTagPage(state);
            return 0;
        }
        if (command >= kRecentTagBaseId &&
            command < kRecentTagBaseId + static_cast<int>(kQuickTagsPerPage) &&
            state->tagEdit != nullptr &&
            state->quickTags != nullptr)
        {
            const size_t index = state->tagPage * kQuickTagsPerPage +
                static_cast<size_t>(command - kRecentTagBaseId);
            if (index < state->quickTags->size())
            {
                const std::wstring value = AppendTag(
                    ControlText(state->tagEdit), (*state->quickTags)[index]);
                SetWindowTextW(state->tagEdit, value.c_str());
                SetFocus(state->tagEdit);
                SendMessageW(state->tagEdit, EM_SETSEL,
                             static_cast<WPARAM>(-1),
                             static_cast<LPARAM>(-1));
            }
            return 0;
        }
        if (command == IDOK)
        {
            service::ImportMetadata metadata;
            metadata.overrideTitle = state->files->size() == 1;
            metadata.title = WideToUtf8(
                ControlText(GetDlgItem(hwnd, kTitleId)));
            std::wstring category = ControlText(GetDlgItem(hwnd, kCategoryId));
            if (category == kNoCategory)
            {
                category.clear();
            }
            metadata.category = WideToUtf8(category);
            metadata.tags = ParseTags(
                ControlText(GetDlgItem(hwnd, kTagsId)));
            metadata.actors = SplitValues(
                ControlText(GetDlgItem(hwnd, kActorsId)));
            metadata.favorite = dark::IsChecked(
                GetDlgItem(hwnd, kFavoriteId));
            state->result = std::move(metadata);
            CloseImportWindow(hwnd, state);
            return 0;
        }
        if (command == IDCANCEL)
        {
            CloseImportWindow(hwnd, state);
            return 0;
        }
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
        dark::PaintBorderlessFrame(hwnd, dc, state->title,
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
        CloseImportWindow(hwnd, state);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wp, lp);
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

}  // namespace

std::optional<service::ImportMetadata> ImportDialog::Show(
    HWND owner, const std::vector<std::filesystem::path>& files,
    const std::vector<std::string>& categories,
    const std::vector<std::string>& quickTags,
    const service::ImportMetadata* initial, bool editMode)
{
    static bool registered = false;
    if (!registered)
    {
        WNDCLASSW windowClass = {};
        windowClass.lpfnWndProc = DialogProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = nullptr;
        windowClass.lpszClassName = L"NexusImportDialog";
        registered = RegisterClassW(&windowClass) != 0 ||
                     GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    if (!registered)
    {
        return std::nullopt;
    }

    DialogState state;
    state.owner = owner;
    state.title = editMode ? L"编辑资源" : L"导入资源";
    state.files = &files;
    state.categories = &categories;
    const std::vector<std::string> uniqueQuickTags = UniqueTags(quickTags);
    state.quickTags = &uniqueQuickTags;
    RECT ownerRect = {};
    GetWindowRect(owner, &ownerRect);
    const int x = ownerRect.left + std::max(
        0, static_cast<int>((ownerRect.right - ownerRect.left) -
                            kWindowWidth) / 2);
    const int y = ownerRect.top + std::max(
        0, static_cast<int>((ownerRect.bottom - ownerRect.top) -
                            kWindowHeight) / 2);
    state.window = CreateWindowExW(
        WS_EX_TOOLWINDOW, L"NexusImportDialog",
        editMode ? L"编辑资源" : L"导入资源",
        WS_POPUP | WS_SYSMENU, x, y, kWindowWidth, kWindowHeight, owner,
        nullptr, GetModuleHandleW(nullptr), &state);
    if (state.window == nullptr)
    {
        return std::nullopt;
    }
    dark::ApplyRoundedWindow(state.window);
    // WM_NCCALCSIZE already ran during CreateWindowExW with the default
    // handler; recalculate now that the dialog proc is wired up.
    SetWindowPos(state.window, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE);

    const std::wstring summary = editMode
        ? L"编辑资源 Metadata"
        : std::to_wstring(files.size()) + L" 个资源将归档到 Viewer";
    AddLabel(state.window, summary.c_str(), 40, 108, 960, 44);
    AddLabel(state.window, L"标题", 40, 180, 180, 44);
    HWND title = AddEdit(state.window, L"", kTitleId, 240, 172, 760,
                         kRowHeight);
    if (initial != nullptr && !initial->title.empty())
    {
        SetWindowTextW(title, Utf8ToWide(initial->title).c_str());
    }
    else if (files.size() == 1)
    {
        SetWindowTextW(title, files.front().stem().c_str());
    }
    else
    {
        EnableWindow(title, FALSE);
    }
    AddLabel(state.window, L"分类", 40, 256, 180, 44);
    HWND category = CreateWindowExW(
        0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST |
            CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
        240, 248, 760, kRowHeight + 340, state.window, ControlId(kCategoryId),
        nullptr, nullptr);
    SendMessageW(category, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(kNoCategory));
    for (const std::string& item : categories)
    {
        const std::wstring wide = Utf8ToWide(item);
        SendMessageW(category, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(wide.c_str()));
    }
    int categorySelection = 0;
    if (initial != nullptr && !initial->category.empty())
    {
        const std::wstring wide = Utf8ToWide(initial->category);
        const LRESULT found = SendMessageW(category, CB_FINDSTRINGEXACT,
                                           static_cast<WPARAM>(-1),
                                           reinterpret_cast<LPARAM>(
                                               wide.c_str()));
        if (found >= 0)
        {
            categorySelection = static_cast<int>(found);
        }
    }
    SendMessageW(category, CB_SETCURSEL, categorySelection, 0);
    dark::StyleCombo(category);
    AddLabel(state.window, L"标签", 40, 332, 180, 44);
    HWND tagEdit = AddEdit(state.window, L"", kTagsId, 240, 324, 760,
                           kRowHeight);
    state.tagEdit = tagEdit;
    if (initial != nullptr)
    {
        std::wstring value;
        for (size_t i = 0; i < initial->tags.size(); ++i)
        {
            if (i != 0)
            {
                value += L", ";
            }
            value += Utf8ToWide(initial->tags[i]);
        }
        SetWindowTextW(tagEdit, value.c_str());
    }
    AddLabel(state.window, L"快捷标签", 40, 384, 180, 40, true);
    for (size_t i = 0; i < kQuickTagsPerPage; ++i)
    {
        const int buttonX = 240 + static_cast<int>(i) * 126;
        AddButton(state.window, L"",
                  kRecentTagBaseId + static_cast<int>(i), buttonX, 382, 114,
                  40);
    }
    AddButton(state.window, L"上一页", kPreviousTagsId, 240, 432, 140, 40);
    HWND tagPage = AddLabel(state.window, L"", 400, 432, 220, 40, true);
    SetWindowLongPtrW(tagPage, GWLP_ID, kTagPageId);
    AddButton(state.window, L"下一页", kNextTagsId, 640, 432, 140, 40);
    UpdateQuickTagPage(&state);
    AddLabel(state.window, L"演员", 40, 496, 180, 44);
    HWND actorEdit = AddEdit(state.window, L"", kActorsId, 240, 488, 760,
                             kRowHeight);
    if (initial != nullptr)
    {
        std::wstring value;
        for (size_t i = 0; i < initial->actors.size(); ++i)
        {
            if (i != 0)
            {
                value += L", ";
            }
            value += Utf8ToWide(initial->actors[i]);
        }
        SetWindowTextW(actorEdit, value.c_str());
    }
    HWND favorite = CreateWindowExW(
        0, L"BUTTON", L"收藏",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 240, 556, 760, 44,
        state.window, ControlId(kFavoriteId), nullptr, nullptr);
    dark::StyleButton(favorite, true);
    dark::MarkToggle(favorite);
    if (initial != nullptr && initial->favorite)
    {
        dark::SetChecked(favorite, true);
    }
    AddLabel(state.window,
             editMode ? L"保存将原子更新资源 Metadata。"
                      : L"确认后将在后台安全归档；成功前不会删除来源文件。",
             40, 620, 960, 44, true);
    AddButton(state.window, L"取消", IDCANCEL, 620, 688, 180, 56);
    AddButton(state.window, editMode ? L"保存" : L"导入", IDOK, 820, 688, 180,
              56, true);
    dark::ApplyFontToChildren(state.window);

    EnableWindow(owner, FALSE);
    ShowWindow(state.window, SW_SHOW);
    SetWindowPos(state.window, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(state.window);
    SetActiveWindow(state.window);
    MSG message = {};
    while (!state.done && GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(state.window, &message))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(owner, TRUE);
    BringWindowToTop(owner);
    SetActiveWindow(owner);
    return state.result;
}

}  // namespace nexus::ui

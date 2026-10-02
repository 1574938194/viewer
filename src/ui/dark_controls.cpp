#include "ui/dark_controls.h"

#include <windowsx.h>
#include <dwmapi.h>

#include <algorithm>

namespace nexus::ui::dark {
namespace {

constexpr wchar_t kOrigProcProp[] = L"NexusOrigProc";
constexpr wchar_t kHoverProp[] = L"NexusHover";
constexpr wchar_t kCheckboxProp[] = L"NexusCheckbox";
constexpr wchar_t kCheckedProp[] = L"NexusChecked";
constexpr wchar_t kSecondaryProp[] = L"NexusSecondary";
constexpr wchar_t kAccentProp[] = L"NexusAccent";
constexpr wchar_t kToggleProp[] = L"NexusToggle";
constexpr wchar_t kComboOrigProcProp[] = L"NexusComboOrigProc";
constexpr wchar_t kListOrigProcProp[] = L"NexusListOrigProc";
constexpr wchar_t kListDraggingProp[] = L"NexusListDragging";
constexpr wchar_t kListDragOffsetProp[] = L"NexusListDragOffset";
constexpr wchar_t kListHidingScrollProp[] = L"NexusListHidingScroll";

// The list-box scrollbar is drawn inside the client area after the native
// scrollbar has been disabled. Keeping a fixed strip makes the dark bar
// visible even when the list currently has fewer rows than fit on screen.
constexpr int kListScrollBarWidth = 24;
constexpr int kListScrollTrackWidth = 12;
constexpr int kListScrollMinThumb = 22;

HFONT g_font = nullptr;
HFONT g_headerFont = nullptr;
HBRUSH g_panelBrush = nullptr;
HBRUSH g_inputBrush = nullptr;

HBRUSH PanelBrush()
{
    if (g_panelBrush == nullptr)
    {
        g_panelBrush = CreateSolidBrush(kPanel);
    }
    return g_panelBrush;
}

HBRUSH InputBrush()
{
    if (g_inputBrush == nullptr)
    {
        g_inputBrush = CreateSolidBrush(kInput);
    }
    return g_inputBrush;
}

void FillColorRect(HDC dc, const RECT& rect, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void FrameColorRect(HDC dc, const RECT& rect, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    FrameRect(dc, &rect, brush);
    DeleteObject(brush);
}

void FillRoundColorRect(HDC dc, const RECT& rect, COLORREF color, int radius)
{
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2,
              radius * 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(brush);
}

void FrameRoundColorRect(HDC dc, const RECT& rect, COLORREF color, int radius)
{
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2,
              radius * 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void DrawLabel(HDC dc, const RECT& rect, const wchar_t* text, COLORREF color,
               UINT format)
{
    RECT textRect = rect;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    HFONT oldFont =
        reinterpret_cast<HFONT>(SelectObject(dc, UiFont()));
    DrawTextW(dc, text, -1, &textRect, format | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);
}

BOOL CALLBACK SetChildFont(HWND child, LPARAM font)
{
    SendMessageW(child, WM_SETFONT, font, MAKELPARAM(TRUE, 0));
    return TRUE;
}

LRESULT CALLBACK ButtonSubclassProc(HWND hwnd, UINT message, WPARAM wp,
                                    LPARAM lp)
{
    const WNDPROC original = reinterpret_cast<WNDPROC>(
        reinterpret_cast<LONG_PTR>(GetPropW(hwnd, kOrigProcProp)));
    switch (message)
    {
    case WM_MOUSEMOVE:
        if (GetPropW(hwnd, kHoverProp) == nullptr)
        {
            SetPropW(hwnd, kHoverProp, reinterpret_cast<HANDLE>(TRUE));
            InvalidateRect(hwnd, nullptr, TRUE);
            TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
        }
        break;
    case WM_MOUSELEAVE:
        SetPropW(hwnd, kHoverProp, nullptr);
        InvalidateRect(hwnd, nullptr, TRUE);
        break;
    case WM_LBUTTONUP:
    {
        RECT client = {};
        GetClientRect(hwnd, &client);
        const POINT point = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (PtInRect(&client, point))
        {
            if (GetPropW(hwnd, kCheckboxProp) != nullptr)
            {
                const bool checked = GetPropW(hwnd, kCheckedProp) != nullptr;
                SetPropW(hwnd, kCheckedProp,
                         checked ? nullptr : reinterpret_cast<HANDLE>(TRUE));
                InvalidateRect(hwnd, nullptr, TRUE);
            }
        }
        break;
    }
    case WM_DESTROY:
        RemovePropW(hwnd, kOrigProcProp);
        RemovePropW(hwnd, kHoverProp);
        RemovePropW(hwnd, kCheckboxProp);
        RemovePropW(hwnd, kCheckedProp);
        RemovePropW(hwnd, kSecondaryProp);
        RemovePropW(hwnd, kAccentProp);
        RemovePropW(hwnd, kToggleProp);
        break;
    default:
        break;
    }
    return original != nullptr
               ? CallWindowProcW(original, hwnd, message, wp, lp)
               : DefWindowProcW(hwnd, message, wp, lp);
}

LRESULT CALLBACK ComboSubclassProc(HWND hwnd, UINT message, WPARAM wp,
                                   LPARAM lp)
{
    const WNDPROC original = reinterpret_cast<WNDPROC>(
        reinterpret_cast<LONG_PTR>(GetPropW(hwnd, kComboOrigProcProp)));
    if (message == WM_PAINT)
    {
        PAINTSTRUCT ps = {};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rect = {};
        GetClientRect(hwnd, &rect);
        FillRoundColorRect(dc, rect, kInput, 10);
        FrameRoundColorRect(dc, rect, kBorder, 10);
        wchar_t text[256] = {};
        const LRESULT selection = SendMessageW(hwnd, CB_GETCURSEL, 0, 0);
        if (selection >= 0)
        {
            SendMessageW(hwnd, CB_GETLBTEXT, selection,
                         reinterpret_cast<LPARAM>(text));
        }
        RECT label = rect;
        label.left += 14;
        label.right -= 42;
        DrawLabel(dc, label, text, kText,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT arrow = rect;
        arrow.left = arrow.right - 38;
        DrawLabel(dc, arrow, L"▾", kTextSecondary,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (message == WM_DESTROY)
    {
        RemovePropW(hwnd, kComboOrigProcProp);
    }
    return original != nullptr
               ? CallWindowProcW(original, hwnd, message, wp, lp)
               : DefWindowProcW(hwnd, message, wp, lp);
}

struct ListScrollMetrics
{
    RECT bar = {};
    RECT track = {};
    RECT thumb = {};
    int contentRange = 1;
    int page = 1;
    int position = 0;
};

ListScrollMetrics GetListScrollMetrics(HWND hwnd)
{
    if (hwnd == nullptr)
    {
        return {};
    }

    RECT client = {};
    GetClientRect(hwnd, &client);
    const int width = std::max(
        0, static_cast<int>(client.right - client.left));
    const int height = std::max(
        0, static_cast<int>(client.bottom - client.top));
    if (width <= 0 || height <= 0)
    {
        return {};
    }

    ListScrollMetrics metrics;
    metrics.bar = {std::max(0, width - kListScrollBarWidth), 0, width,
                   height};
    const int trackTop = std::min(4, height);
    const int trackBottom = std::max(trackTop + 1, height - 4);
    const int barLeft = static_cast<int>(metrics.bar.left);
    const int barRight = static_cast<int>(metrics.bar.right);
    metrics.track = {
        std::max(barLeft + 2, barRight - kListScrollTrackWidth - 5),
        trackTop,
        std::min(barRight - 3, std::max(barLeft + 3, barRight - 5)),
        trackBottom};

    // Native SCROLLINFO can be stale when its scrollbar is disabled. The
    // custom bar always follows the actual list content and top item.
    const int count = std::max(
        0, static_cast<int>(SendMessageW(hwnd, LB_GETCOUNT, 0, 0)));
    const int itemHeight = std::max(
        1, static_cast<int>(SendMessageW(hwnd, LB_GETITEMHEIGHT, 0, 0)));
    metrics.contentRange = std::max(1, count);
    metrics.page = std::max(1, height / itemHeight);
    const int maxPosition =
        std::max(0, metrics.contentRange - metrics.page);
    metrics.position = std::clamp(
        static_cast<int>(SendMessageW(hwnd, LB_GETTOPINDEX, 0, 0)), 0,
        maxPosition);

    const int trackHeight = std::max(
        1, static_cast<int>(metrics.track.bottom - metrics.track.top));
    const int minThumb = std::min(kListScrollMinThumb, trackHeight);
    const int thumbHeight = metrics.contentRange <= metrics.page
        ? trackHeight
        : std::clamp(trackHeight * metrics.page /
                         std::max(1, metrics.contentRange),
                     minThumb, trackHeight);
    const int travel = std::max(0, trackHeight - thumbHeight);
    const int positionRange =
        std::max(1, metrics.contentRange - metrics.page);
    const int thumbTop = metrics.track.top +
        travel * metrics.position / positionRange;
    metrics.thumb = {metrics.track.left, thumbTop, metrics.track.right,
                     thumbTop + thumbHeight};
    return metrics;
}

void HideListBoxNativeScrollBar(HWND hwnd)
{
    if (hwnd != nullptr &&
        GetPropW(hwnd, kListHidingScrollProp) == nullptr)
    {
        // A list created with WS_VSCROLL can restore it when updating its
        // top index. Recheck after native messages; guard the frame update
        // because it synchronously sends WM_SIZE back to this subclass.
        LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        if ((style & static_cast<LONG_PTR>(WS_VSCROLL)) != 0)
        {
            SetPropW(hwnd, kListHidingScrollProp,
                     reinterpret_cast<HANDLE>(TRUE));
            ShowScrollBar(hwnd, SB_VERT, FALSE);
            style = GetWindowLongPtrW(hwnd, GWL_STYLE);
            style &= ~static_cast<LONG_PTR>(WS_VSCROLL);
            SetWindowLongPtrW(hwnd, GWL_STYLE, style);
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                         SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                             SWP_NOZORDER | SWP_NOACTIVATE);
            RemovePropW(hwnd, kListHidingScrollProp);
        }
    }
}

void PaintListBoxScrollBar(HWND hwnd)
{
    const ListScrollMetrics metrics = GetListScrollMetrics(hwnd);
    if (metrics.bar.right <= metrics.bar.left ||
        metrics.track.right <= metrics.track.left)
    {
        return;
    }

    HDC dc = GetDC(hwnd);
    if (dc == nullptr)
    {
        return;
    }

    // The native bar is disabled, so this strip is ours. Filling it first
    // also masks list text that reaches the right edge.
    FillColorRect(dc, metrics.bar, kInput);
    FillRoundColorRect(dc, metrics.track, kPanel,
                       (metrics.track.right - metrics.track.left) / 2);
    FillRoundColorRect(dc, metrics.thumb, kBorder,
                       (metrics.thumb.right - metrics.thumb.left) / 2);
    ReleaseDC(hwnd, dc);
}

void InvalidateListBoxScrollBar(HWND hwnd)
{
    if (hwnd == nullptr)
    {
        return;
    }
    // The themed scrollbar lives in the client area.  Invalidate only that
    // strip and let the normal paint cycle redraw it; forcing RDW_FRAME and
    // RDW_UPDATENOW causes a visible white/non-client flash while dragging.
    const ListScrollMetrics metrics = GetListScrollMetrics(hwnd);
    InvalidateRect(hwnd, &metrics.bar, FALSE);
}

LRESULT CALLBACK ListBoxSubclassProc(HWND hwnd, UINT message, WPARAM wp,
                                     LPARAM lp)
{
    const WNDPROC original = reinterpret_cast<WNDPROC>(
        reinterpret_cast<LONG_PTR>(GetPropW(hwnd, kListOrigProcProp)));
    if (message == WM_STYLECHANGING && wp == static_cast<WPARAM>(GWL_STYLE))
    {
        auto* style = reinterpret_cast<STYLESTRUCT*>(lp);
        style->styleNew &= ~WS_VSCROLL;
        return 0;
    }
    if (message == WM_PAINT)
    {
        const LRESULT result = original != nullptr
            ? CallWindowProcW(original, hwnd, message, wp, lp)
            : DefWindowProcW(hwnd, message, wp, lp);
        PaintListBoxScrollBar(hwnd);
        return result;
    }
    if (message == WM_NCPAINT)
    {
        // WS_VSCROLL is removed by StyleListBox. Do not let the list-box
        // window procedure repaint a legacy non-client scrollbar over the
        // themed client-area strip.
        return 0;
    }
    if (message == WM_ERASEBKGND)
    {
        return 1;
    }
    if (message == WM_NCDESTROY)
    {
        const LRESULT result = original != nullptr
            ? CallWindowProcW(original, hwnd, message, wp, lp)
            : DefWindowProcW(hwnd, message, wp, lp);
        RemovePropW(hwnd, kListOrigProcProp);
        RemovePropW(hwnd, kListDraggingProp);
        RemovePropW(hwnd, kListDragOffsetProp);
        RemovePropW(hwnd, kListHidingScrollProp);
        return result;
    }

    if (message == WM_MOUSEWHEEL)
    {
        // A list box without WS_VSCROLL no longer lets its default procedure
        // translate wheel messages. Move the top index directly so hiding
        // the native bar does not change scrolling behavior.
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (delta != 0)
        {
            const int magnitude = delta > 0 ? delta : -delta;
            const int steps = std::max(1, magnitude / WHEEL_DELTA);
            const int count = std::max(
                0, static_cast<int>(SendMessageW(hwnd, LB_GETCOUNT, 0, 0)));
            const int itemHeight = std::max(
                1, static_cast<int>(SendMessageW(hwnd, LB_GETITEMHEIGHT, 0, 0)));
            RECT client = {};
            GetClientRect(hwnd, &client);
            const int page = std::max(
                1, static_cast<int>(client.bottom - client.top) / itemHeight);
            const int maxTop = std::max(0, count - page);
            int top = static_cast<int>(SendMessageW(hwnd, LB_GETTOPINDEX, 0, 0));
            for (int i = 0; i < steps; ++i)
            {
                top += delta > 0 ? -1 : 1;
            }
            SendMessageW(hwnd, LB_SETTOPINDEX,
                         std::clamp(top, 0, maxTop), 0);
        }
        InvalidateListBoxScrollBar(hwnd);
        return 0;
    }

    if (message == WM_LBUTTONDOWN)
    {
        const ListScrollMetrics metrics = GetListScrollMetrics(hwnd);
        const POINT point = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (PtInRect(&metrics.bar, point))
        {
            if (PtInRect(&metrics.thumb, point))
            {
                SetPropW(hwnd, kListDraggingProp,
                         reinterpret_cast<HANDLE>(TRUE));
                SetPropW(hwnd, kListDragOffsetProp,
                         reinterpret_cast<HANDLE>(static_cast<INT_PTR>(
                             point.y - metrics.thumb.top)));
                SetCapture(hwnd);
            }
            else
            {
                const int trackHeight = std::max(
                    1, static_cast<int>(metrics.track.bottom -
                                        metrics.track.top));
                const int thumbHeight = std::max(
                    1, static_cast<int>(metrics.thumb.bottom -
                                        metrics.thumb.top));
                const int travel = std::max(1, trackHeight - thumbHeight);
                const int maxPosition =
                    std::max(0, metrics.contentRange - metrics.page);
                const int target = std::clamp(
                    static_cast<int>(point.y - metrics.track.top) -
                        thumbHeight / 2,
                    0, travel);
                const int targetPosition = std::clamp(
                    target * maxPosition / travel,
                    0, maxPosition);
                SendMessageW(hwnd, LB_SETTOPINDEX, targetPosition, 0);
                InvalidateListBoxScrollBar(hwnd);
            }
            return 0;
        }
    }
    if (message == WM_MOUSEMOVE &&
        GetPropW(hwnd, kListDraggingProp) != nullptr)
    {
        const ListScrollMetrics metrics = GetListScrollMetrics(hwnd);
        const int offset = static_cast<int>(reinterpret_cast<INT_PTR>(
            GetPropW(hwnd, kListDragOffsetProp)));
        const int trackHeight = std::max(
            1, static_cast<int>(metrics.track.bottom - metrics.track.top));
        const int thumbHeight = std::max(
            1, static_cast<int>(metrics.thumb.bottom - metrics.thumb.top));
        const int travel = std::max(1, trackHeight - thumbHeight);
        const int maxPosition =
            std::max(0, metrics.contentRange - metrics.page);
        const int thumbPosition = std::clamp(
            static_cast<int>(GET_Y_LPARAM(lp) - offset -
                             metrics.track.top),
            0, travel);
        const int target = thumbPosition * maxPosition / travel;
        SendMessageW(hwnd, LB_SETTOPINDEX, target, 0);
        InvalidateListBoxScrollBar(hwnd);
        return 0;
    }
    if (message == WM_LBUTTONUP || message == WM_CAPTURECHANGED)
    {
        if (GetPropW(hwnd, kListDraggingProp) != nullptr)
        {
            RemovePropW(hwnd, kListDraggingProp);
            RemovePropW(hwnd, kListDragOffsetProp);
            if (GetCapture() == hwnd)
            {
                ReleaseCapture();
            }
            InvalidateListBoxScrollBar(hwnd);
            return 0;
        }
    }

    const LRESULT result = original != nullptr
        ? CallWindowProcW(original, hwnd, message, wp, lp)
        : DefWindowProcW(hwnd, message, wp, lp);
    switch (message)
    {
    case WM_VSCROLL:
    case WM_KEYDOWN:
    case WM_LBUTTONUP:
    case WM_NCLBUTTONUP:
    case WM_SIZE:
    case LB_ADDSTRING:
    case LB_DELETESTRING:
    case LB_RESETCONTENT:
    case LB_SETTOPINDEX:
        HideListBoxNativeScrollBar(hwnd);
        InvalidateListBoxScrollBar(hwnd);
        break;
    default:
        break;
    }
    return result;
}

}  // namespace

HFONT UiFont()
{
    if (g_font == nullptr)
    {
        LOGFONTW description = {};
        description.lfHeight = -28;
        description.lfWeight = FW_NORMAL;
        description.lfCharSet = DEFAULT_CHARSET;
        description.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(description.lfFaceName, L"Segoe UI");
        g_font = CreateFontIndirectW(&description);
    }
    return g_font;
}

HFONT HeaderFont()
{
    if (g_headerFont == nullptr)
    {
        LOGFONTW description = {};
        description.lfHeight = -34;
        description.lfWeight = FW_SEMIBOLD;
        description.lfCharSet = DEFAULT_CHARSET;
        description.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(description.lfFaceName, L"Segoe UI");
        g_headerFont = CreateFontIndirectW(&description);
    }
    return g_headerFont;
}

void ApplyFontToChildren(HWND parent)
{
    EnumChildWindows(parent, SetChildFont,
                     reinterpret_cast<LPARAM>(UiFont()));
}

void MarkSecondary(HWND control)
{
    SetPropW(control, kSecondaryProp, reinterpret_cast<HANDLE>(TRUE));
}

void MarkAccent(HWND button)
{
    SetPropW(button, kAccentProp, reinterpret_cast<HANDLE>(TRUE));
}

void StyleButton(HWND button, bool checkbox)
{
    SetPropW(button, kOrigProcProp, reinterpret_cast<HANDLE>(
        GetWindowLongPtrW(button, GWLP_WNDPROC)));
    if (checkbox)
    {
        SetPropW(button, kCheckboxProp, reinterpret_cast<HANDLE>(TRUE));
        SetPropW(button, kCheckedProp, nullptr);
    }
    SetWindowLongPtrW(button, GWLP_WNDPROC,
                      reinterpret_cast<LONG_PTR>(ButtonSubclassProc));
}

void MarkToggle(HWND checkbox)
{
    SetPropW(checkbox, kToggleProp, reinterpret_cast<HANDLE>(TRUE));
}

void StyleCombo(HWND combo)
{
    SetPropW(combo, kComboOrigProcProp,
             reinterpret_cast<HANDLE>(GetWindowLongPtrW(combo, GWLP_WNDPROC)));
    SetWindowLongPtrW(combo, GWLP_WNDPROC,
                      reinterpret_cast<LONG_PTR>(ComboSubclassProc));
}

void StyleListBox(HWND list)
{
    if (list == nullptr || GetPropW(list, kListOrigProcProp) != nullptr)
    {
        return;
    }
    SetPropW(list, kListOrigProcProp,
             reinterpret_cast<HANDLE>(GetWindowLongPtrW(list, GWLP_WNDPROC)));
    SetWindowLongPtrW(list, GWLP_WNDPROC,
                      reinterpret_cast<LONG_PTR>(ListBoxSubclassProc));
    HideListBoxNativeScrollBar(list);
    InvalidateListBoxScrollBar(list);
}

void SetChecked(HWND checkbox, bool checked)
{
    SetPropW(checkbox, kCheckedProp,
             checked ? reinterpret_cast<HANDLE>(TRUE) : nullptr);
}

bool IsChecked(HWND checkbox)
{
    return GetPropW(checkbox, kCheckedProp) != nullptr;
}

HBRUSH HandleCtlColor(HWND control, HDC dc, UINT message)
{
    SetBkMode(dc, TRANSPARENT);
    if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX)
    {
        SetTextColor(dc, kText);
        SetBkColor(dc, kInput);
        return InputBrush();
    }
    SetTextColor(dc, GetPropW(control, kSecondaryProp) != nullptr
                         ? kTextSecondary
                         : kText);
    SetBkColor(dc, kPanel);
    return PanelBrush();
}

void HandleMeasureItem(MEASUREITEMSTRUCT* item)
{
    if (item != nullptr && item->CtlType == ODT_COMBOBOX)
    {
        item->itemHeight = 56;
    }
    else if (item != nullptr && item->CtlType == ODT_LISTBOX)
    {
        // Keep each preset-tag row tall enough for the dialog font while
        // allowing the list to remain compact and keyboard friendly.
        item->itemHeight = 44;
    }
}

void HandleDrawItem(const DRAWITEMSTRUCT* item)
{
    if (item == nullptr)
    {
        return;
    }
    const HDC dc = item->hDC;
    const RECT rect = item->rcItem;
    const bool hover = GetPropW(item->hwndItem, kHoverProp) != nullptr;
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    if (item->CtlType == ODT_BUTTON)
    {
        wchar_t text[256] = {};
        GetWindowTextW(item->hwndItem, text, 256);
        if (GetPropW(item->hwndItem, kCheckboxProp) != nullptr)
        {
            if (GetPropW(item->hwndItem, kToggleProp) != nullptr)
            {
                const int trackWidth = 52;
                const int trackHeight = 28;
                const int trackY = rect.top +
                    ((rect.bottom - rect.top) - trackHeight) / 2;
                RECT track = {rect.right - trackWidth - 2, trackY,
                              rect.right - 2, trackY + trackHeight};
                const bool checked =
                    GetPropW(item->hwndItem, kCheckedProp) != nullptr;
                const COLORREF trackColor = checked ? RGB(55, 104, 78)
                                                     : RGB(60, 64, 64);
                FillRoundColorRect(dc, track, trackColor, trackHeight / 2);
                const int knobSize = 20;
                const int knobX = checked
                    ? track.right - knobSize - 4 : track.left + 4;
                RECT knob = {knobX, track.top + 4, knobX + knobSize,
                             track.bottom - 4};
                FillRoundColorRect(dc, knob, RGB(225, 235, 228), knobSize / 2);
                RECT labelRect = rect;
                labelRect.right = track.left - 14;
                DrawLabel(dc, labelRect, text, kText,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                return;
            }
            const int boxSize = 26;
            const int boxY =
                rect.top + ((rect.bottom - rect.top) - boxSize) / 2;
            RECT box = {rect.left + 2, boxY, rect.left + 2 + boxSize,
                        boxY + boxSize};
            FillRoundColorRect(dc, box, kInput, 7);
            FrameRoundColorRect(dc, box, hover ? kAccent : kBorder, 7);
            if (GetPropW(item->hwndItem, kCheckedProp) != nullptr)
            {
                RECT inner = box;
                InflateRect(&inner, -4, -4);
                FillRoundColorRect(dc, inner, kAccent, 3);
                DrawLabel(dc, inner, L"✓", kText,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            RECT labelRect = rect;
            labelRect.left = rect.left + 2 + boxSize + 14;
            DrawLabel(dc, labelRect, text, kText,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        else
        {
            const COLORREF face =
                pressed ? kBorder : hover ? kCardHover : kCard;
            const COLORREF edge =
                GetPropW(item->hwndItem, kAccentProp) != nullptr ? kAccent
                                                                 : kBorder;
            FillRoundColorRect(dc, rect, face, 12);
            FrameRoundColorRect(dc, rect, edge, 12);
            DrawLabel(dc, rect, text, kText,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        return;
    }
    if (item->CtlType == ODT_COMBOBOX)
    {
        wchar_t text[256] = {};
        const bool listRow = item->itemID != static_cast<UINT>(-1);
        if (!listRow)
        {
            const LRESULT selection =
                SendMessageW(item->hwndItem, CB_GETCURSEL, 0, 0);
            if (selection >= 0)
            {
                SendMessageW(item->hwndItem, CB_GETLBTEXT, selection,
                             reinterpret_cast<LPARAM>(text));
            }
        }
        else if (item->itemData != 0)
        {
            wcsncpy_s(text, reinterpret_cast<const wchar_t*>(item->itemData),
                      _TRUNCATE);
        }
        const COLORREF face =
            listRow && (item->itemState & ODS_SELECTED) ? kCardHover
                                                        : kInput;
        if (listRow)
        {
            FillColorRect(dc, rect, face);
        }
        else
        {
            FillRoundColorRect(dc, rect, face, 12);
        }
        RECT textRect = rect;
        textRect.left += 14;
        textRect.right -= 36;
        DrawLabel(dc, textRect, text, kText,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (!listRow)
        {
            RECT arrowRect = rect;
            arrowRect.left = arrowRect.right - 40;
            DrawLabel(dc, arrowRect, L"▾", kTextSecondary,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
    if (item->CtlType == ODT_LISTBOX)
    {
        if (item->itemID == static_cast<UINT>(-1))
        {
            return;
        }
        wchar_t text[512] = {};
        SendMessageW(item->hwndItem, LB_GETTEXT, item->itemID,
                     reinterpret_cast<LPARAM>(text));
        const bool selected = (item->itemState & ODS_SELECTED) != 0;
        const COLORREF background = selected ? RGB(4, 57, 94) : kInput;
        const COLORREF foreground = selected ? RGB(255, 255, 255) : kText;
        // The native scrollbar stays disabled, so owner-draw
        // receives the entire client width. Reserve the themed scrollbar
        // strip explicitly; otherwise every item paints underneath it and
        // the later scrollbar pass appears to flicker at the right edge.
        RECT itemRect = rect;
        itemRect.right = std::max(itemRect.left,
                                  itemRect.right - kListScrollBarWidth);
        FillColorRect(dc, itemRect, background);
        RECT textRect = itemRect;
        textRect.left += 12;
        textRect.right -= 12;
        DrawLabel(dc, textRect, text, foreground,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if ((item->itemState & ODS_FOCUS) != 0)
        {
            FrameColorRect(dc, itemRect, kAccent);
        }
        return;
    }
}

void PaintPanelBackground(HWND hwnd, HDC dc, const RECT& client)
{
    (void)hwnd;
    FillRect(dc, &client, PanelBrush());
}

LRESULT BorderlessHitTest(HWND hwnd, LPARAM lp)
{
    const LRESULT hit = DefWindowProcW(hwnd, WM_NCHITTEST, 0, lp);
    if (hit != HTCLIENT)
    {
        return hit;
    }
    POINT point = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ScreenToClient(hwnd, &point);
    if (point.y >= 0 && point.y < kHeaderHeight && !CloseButtonHit(hwnd, point))
    {
        return HTCAPTION;
    }
    return HTCLIENT;
}

bool CloseButtonHit(HWND hwnd, POINT point)
{
    RECT client = {};
    GetClientRect(hwnd, &client);
    RECT button = {client.right - kCloseSize - 16,
                   (kHeaderHeight - kCloseSize) / 2,
                   client.right - 16, (kHeaderHeight + kCloseSize) / 2};
    return PtInRect(&button, point) != FALSE;
}

void InvalidateCloseButton(HWND hwnd)
{
    RECT client = {};
    GetClientRect(hwnd, &client);
    RECT button = {client.right - kCloseSize - 16,
                   (kHeaderHeight - kCloseSize) / 2,
                   client.right - 16, (kHeaderHeight + kCloseSize) / 2};
    InvalidateRect(hwnd, &button, FALSE);
}

void PaintBorderlessFrame(HWND hwnd, HDC dc, const wchar_t* title,
                          bool closeHover)
{
    RECT client = {};
    GetClientRect(hwnd, &client);
    FillRect(dc, &client, PanelBrush());
    RECT separator = {0, kHeaderHeight - 1, client.right, kHeaderHeight};
    FillColorRect(dc, separator, kBorder);

    RECT titleRect = {44, 0, client.right - kCloseSize - 40, kHeaderHeight};
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kText);
    HFONT oldFont =
        reinterpret_cast<HFONT>(SelectObject(dc, HeaderFont()));
    DrawTextW(dc, title, -1, &titleRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);

    RECT button = {client.right - kCloseSize - 16,
                   (kHeaderHeight - kCloseSize) / 2,
                   client.right - 16, (kHeaderHeight + kCloseSize) / 2};
    FillRoundColorRect(dc, button, closeHover ? kDanger : kCard, 12);
    FrameRoundColorRect(dc, button, closeHover ? kDanger : kBorder, 12);
    DrawLabel(dc, button, L"✕", closeHover ? kText : kTextSecondary,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void ApplyRoundedWindow(HWND hwnd)
{
    if (hwnd == nullptr)
    {
        return;
    }
    // DWMWA_WINDOW_CORNER_PREFERENCE is available on Windows 11 and keeps
    // the rounded edge correct across resize, DPI changes and maximize.
    constexpr DWORD kCornerPreference = 33;
    constexpr DWORD kRound = 2;
    DwmSetWindowAttribute(hwnd, kCornerPreference, &kRound, sizeof(kRound));
}

}  // namespace nexus::ui::dark

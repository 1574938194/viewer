#include "ui/dark_controls.h"

#include <windowsx.h>
#include <dwmapi.h>

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
            SendMessageW(GetParent(hwnd), WM_COMMAND,
                         MAKEWPARAM(GetDlgCtrlID(hwnd), BN_CLICKED),
                         reinterpret_cast<LPARAM>(hwnd));
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
                RECT track = {rect.left + 2, trackY,
                              rect.left + 2 + trackWidth,
                              trackY + trackHeight};
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
                labelRect.left = track.right + 14;
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

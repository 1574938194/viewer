#ifndef NEXUS_SRC_UI_DARK_CONTROLS_H_
#define NEXUS_SRC_UI_DARK_CONTROLS_H_

#include <Windows.h>

namespace nexus::ui::dark {

// GDI-side mirror of render/theme.h so the Win32 dialogs share the main
// window's dark palette.
constexpr COLORREF kPanel = RGB(32, 32, 32);
constexpr COLORREF kCard = RGB(37, 37, 37);
constexpr COLORREF kCardHover = RGB(48, 48, 48);
constexpr COLORREF kInput = RGB(29, 29, 29);
constexpr COLORREF kBorder = RGB(53, 53, 53);
constexpr COLORREF kText = RGB(229, 229, 229);
constexpr COLORREF kTextSecondary = RGB(153, 153, 153);
constexpr COLORREF kAccent = RGB(91, 141, 190);
constexpr COLORREF kDanger = RGB(184, 92, 92);

// Doubled UI font shared by every dialog control.
HFONT UiFont();

// Larger semibold font for the borderless dialog header title.
HFONT HeaderFont();

void ApplyFontToChildren(HWND parent);

// Borderless dialog chrome (matches the main window header): custom header
// height in px, rounded hover close button, caption drag via WM_NCHITTEST.
constexpr int kHeaderHeight = 72;
constexpr int kCloseSize = 44;

// WM_NCHITTEST for the header: HTCAPTION except over the close button.
LRESULT BorderlessHitTest(HWND hwnd, LPARAM lp);

// True when the client point is over the header close button.
bool CloseButtonHit(HWND hwnd, POINT point);

// Invalidates only the custom close button, avoiding child-control redraw
// flicker when the pointer crosses the header.
void InvalidateCloseButton(HWND hwnd);

// Paints the header (title + rounded close button) and the panel body.
void PaintBorderlessFrame(HWND hwnd, HDC dc, const wchar_t* title,
                          bool closeHover);

// Applies the same rounded outer window treatment to borderless dialogs.
void ApplyRoundedWindow(HWND hwnd);

// Marks a STATIC label to draw with the secondary text color.
void MarkSecondary(HWND control);

// Marks a push button to draw with the accent border (primary action).
void MarkAccent(HWND button);

// Subclasses an owner-draw button/checkbox for hover tracking, checkbox
// toggling and BN_CLICKED notifications to the parent.
void StyleButton(HWND button, bool checkbox);

// Styles a checkbox as a compact horizontal toggle switch.
void MarkToggle(HWND checkbox);

void StyleCombo(HWND combo);

// Styles a list box with a single, always-visible Nexus dark scrollbar while
// preserving the native list-box keyboard and mouse-wheel behavior.
void StyleListBox(HWND list);

void SetChecked(HWND checkbox, bool checked);

bool IsChecked(HWND checkbox);

// WM_CTLCOLOR* dispatch; returns the matching background brush.
HBRUSH HandleCtlColor(HWND control, HDC dc, UINT message);

// WM_MEASUREITEM dispatch (combobox row height).
void HandleMeasureItem(MEASUREITEMSTRUCT* item);

// WM_DRAWITEM dispatch for owner-draw buttons, checkboxes and comboboxes.
void HandleDrawItem(const DRAWITEMSTRUCT* item);

// Fills the dialog client area with the panel color.
void PaintPanelBackground(HWND hwnd, HDC dc, const RECT& client);

}  // namespace nexus::ui::dark

#endif  // NEXUS_SRC_UI_DARK_CONTROLS_H_

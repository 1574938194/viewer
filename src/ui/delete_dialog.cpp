#include "ui/delete_dialog.h"

#include <windowsx.h>

#include <algorithm>

#include "ui/dark_controls.h"

namespace nexus::ui {
namespace {

struct DialogState {
  HWND owner = nullptr;
  bool done = false;
  bool confirmed = false;
  bool close_hover = false;
  const wchar_t* title = nullptr;
};

void Close(HWND window, DialogState* state, bool confirmed)
{
  state->confirmed = confirmed;
  state->done = true;
  EnableWindow(state->owner, TRUE);
  SetForegroundWindow(state->owner);
  SetActiveWindow(state->owner);
  DestroyWindow(window);
}

LRESULT CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
{
  auto* state =
      reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    state = static_cast<DialogState*>(
        reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  }
  if (state == nullptr) {
    return DefWindowProcW(hwnd, message, wp, lp);
  }
  switch (message) {
    case WM_NCCALCSIZE:
      if (wp != FALSE) {
        return 0;
      }
      break;
    case WM_NCACTIVATE:
      return TRUE;
    case WM_NCHITTEST:
      return dark::BorderlessHitTest(hwnd, lp);
    case WM_MOUSEMOVE: {
      const bool hover =
          dark::CloseButtonHit(hwnd, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
      if (hover != state->close_hover) {
        state->close_hover = hover;
        dark::InvalidateCloseButton(hwnd);
      }
      TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
      TrackMouseEvent(&track);
      return 0;
    }
    case WM_MOUSELEAVE:
      state->close_hover = false;
      dark::InvalidateCloseButton(hwnd);
      return 0;
    case WM_LBUTTONDOWN:
      if (dark::CloseButtonHit(hwnd, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)})) {
        Close(hwnd, state, false);
      }
      return 0;
    case WM_COMMAND:
      if (HIWORD(wp) == BN_CLICKED &&
          (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
        Close(hwnd, state, LOWORD(wp) == IDOK);
      }
      return 0;
    case WM_CLOSE:
      Close(hwnd, state, false);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      HDC dc = BeginPaint(hwnd, &ps);
      dark::PaintBorderlessFrame(hwnd, dc, state->title, state->close_hover);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_DRAWITEM:
      dark::HandleDrawItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
      return TRUE;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
      return reinterpret_cast<LRESULT>(dark::HandleCtlColor(
          reinterpret_cast<HWND>(lp), reinterpret_cast<HDC>(wp), message));
    default:
      return DefWindowProcW(hwnd, message, wp, lp);
  }
  return DefWindowProcW(hwnd, message, wp, lp);
}

}  // namespace

bool DeleteDialog::Show(HWND owner, const std::wstring& summary, bool permanent)
{
  WNDCLASSW window_class = {};
  window_class.lpfnWndProc = DialogProc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = L"NexusDeleteDialog";
  if (!RegisterClassW(&window_class) &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }

  DialogState state;
  state.owner = owner;
  state.title = permanent ? L"永久删除" : L"删除资源";
  constexpr int kWidth = 800;
  constexpr int kHeight = 460;
  RECT owner_rect = {};
  GetWindowRect(owner, &owner_rect);
  MONITORINFO monitor = {sizeof(monitor)};
  GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor);
  const int x = std::clamp(
      static_cast<int>(owner_rect.left + owner_rect.right - kWidth) / 2,
      static_cast<int>(monitor.rcWork.left),
      std::max(static_cast<int>(monitor.rcWork.left),
               static_cast<int>(monitor.rcWork.right) - kWidth));
  const int y = std::clamp(
      static_cast<int>(owner_rect.top + owner_rect.bottom - kHeight) / 2,
      static_cast<int>(monitor.rcWork.top),
      std::max(static_cast<int>(monitor.rcWork.top),
               static_cast<int>(monitor.rcWork.bottom) - kHeight));
  HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT,
                                window_class.lpszClassName, state.title,
                                WS_POPUP | WS_SYSMENU, x, y, kWidth, kHeight,
                                owner, nullptr, window_class.hInstance, &state);
  if (window == nullptr) {
    return false;
  }
  dark::ApplyRoundedWindow(window);
  SetWindowPos(window, nullptr, 0, 0, 0, 0,
               SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                   SWP_NOACTIVATE);
  CreateWindowExW(0, L"STATIC", summary.c_str(), WS_CHILD | WS_VISIBLE, 40, 108,
                  720, 238, window, nullptr, nullptr, nullptr);
  HWND cancel = CreateWindowExW(
      0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
      280, 368, 180, 56, window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), nullptr,
      nullptr);
  HWND confirm = CreateWindowExW(
      0, L"BUTTON", permanent ? L"永久删除" : L"移到回收站",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 480, 368, 280, 56,
      window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), nullptr,
      nullptr);
  dark::StyleButton(cancel, false);
  dark::StyleButton(confirm, false);
  dark::MarkAccent(confirm);
  dark::ApplyFontToChildren(window);
  EnableWindow(owner, FALSE);
  ShowWindow(window, SW_SHOW);
  SetForegroundWindow(window);
  SetFocus(cancel);

  MSG message = {};
  while (!state.done) {
    const BOOL result = GetMessageW(&message, nullptr, 0, 0);
    if (result <= 0) {
      Close(window, &state, false);
      if (result == 0) {
        PostQuitMessage(static_cast<int>(message.wParam));
      }
      break;
    }
    if (message.message == WM_KEYDOWN &&
        (message.wParam == VK_ESCAPE || message.wParam == VK_RETURN)) {
      Close(window, &state,
            message.wParam == VK_RETURN && GetFocus() == confirm);
    } else if (!IsDialogMessageW(window, &message)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  return state.confirmed;
}

}  // namespace nexus::ui

#ifndef NEXUS_SRC_UI_SEARCH_BAR_H_
#define NEXUS_SRC_UI_SEARCH_BAR_H_

#include <functional>
#include <string>

#include "render/renderer.h"

namespace nexus::ui {

class SearchBar
{
public:
    void Layout(const D2D1_RECT_F& bounds) { bounds_ = bounds; }

    void Draw(render::Renderer* renderer);

    bool HitTest(POINT point) const;

    bool OnChar(wchar_t ch);

    // Handles navigation and editing keys while the custom search field is
    // focused. Returns true when the key was consumed.
    bool OnKeyDown(WPARAM key, bool control, bool shift);

    void OnLButtonDown();

    void SetFocus(bool focused);

    bool focused() const { return focused_; }

    const std::wstring& text() const { return text_; }

    void SetText(const std::wstring& text);

    void Clear();

    void set_on_text_changed(std::function<void()> callback)
    {
        onTextChanged_ = std::move(callback);
    }

    // Returns true when the caret toggled and the view must repaint.
    bool TickCaret();

private:
    bool HasSelection() const { return cursor_ != anchor_; }

    void DeleteSelection();

    void MoveCursor(std::size_t position, bool extendSelection);

    D2D1_RECT_F bounds_ = {};
    std::wstring text_;
    std::size_t cursor_ = 0;
    std::size_t anchor_ = 0;
    bool focused_ = false;
    bool caretVisible_ = true;
    std::function<void()> onTextChanged_;
};

}  // namespace nexus::ui

#endif  // NEXUS_SRC_UI_SEARCH_BAR_H_

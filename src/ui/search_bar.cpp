#include "ui/search_bar.h"

#include <algorithm>
#include <cwctype>

namespace nexus::ui {

void SearchBar::Draw(render::Renderer* renderer)
{
    const float scale = renderer->scale();
    const float radius = (bounds_.bottom - bounds_.top) / 2.0f;
    renderer->FillRoundRect(bounds_, render::Theme::kInput, radius);
    renderer->StrokeRoundRect(bounds_, focused_ ? render::Theme::kAccent
                                                : render::Theme::kBorder,
                              1.0f * scale, radius);

    const float inset = 10.0f * scale;
    D2D1_RECT_F textRect = {bounds_.left + inset, bounds_.top,
                            bounds_.right - 12.0f * scale, bounds_.bottom};
    if (text_.empty())
    {
        if (!focused_)
        {
            renderer->DrawTextWide(L"搜索资源…", textRect,
                                   render::TextRole::kSearch,
                                   render::Theme::kTextSecondary);
        }
    }
    else
    {
        renderer->DrawTextWide(text_, textRect, render::TextRole::kSearch,
                               render::Theme::kText);
    }

    if (focused_ && caretVisible_)
    {
        const std::wstring prefix = text_.substr(0, cursor_);
        const float textWidth =
            prefix.empty() ? 0.0f : renderer->MeasureTextWide(
                                       prefix, render::TextRole::kSearch);
        const float caretX = textRect.left + textWidth + 2.0f * scale;
        if (caretX < textRect.right - 4.0f * scale)
        {
            renderer->FillRect(
                {caretX, bounds_.top + 6.0f * scale, caretX + 1.5f * scale,
                 bounds_.bottom - 6.0f * scale},
                render::Theme::kAccent);
        }
    }
}

bool SearchBar::HitTest(POINT point) const
{
    return point.x >= bounds_.left && point.x <= bounds_.right &&
           point.y >= bounds_.top && point.y <= bounds_.bottom;
}

bool SearchBar::OnChar(wchar_t ch)
{
    if (!focused_)
    {
        return false;
    }
    if (ch == L'\b')
    {
        if (HasSelection())
        {
            DeleteSelection();
            caretVisible_ = true;
            if (onTextChanged_)
            {
                onTextChanged_();
            }
        }
        else if (cursor_ > 0)
        {
            text_.erase(cursor_ - 1, 1);
            --cursor_;
            anchor_ = cursor_;
            caretVisible_ = true;
            if (onTextChanged_)
            {
                onTextChanged_();
            }
        }
        return true;
    }
    if (ch < 0x20)
    {
        return true;  // swallow control characters
    }
    DeleteSelection();
    text_.insert(cursor_, 1, ch);
    ++cursor_;
    anchor_ = cursor_;
    caretVisible_ = true;
    if (onTextChanged_)
    {
        onTextChanged_();
    }
    return true;
}

bool SearchBar::OnKeyDown(WPARAM key, bool control, bool shift)
{
    if (!focused_)
    {
        return false;
    }
    if (control && key == 'A')
    {
        anchor_ = 0;
        cursor_ = text_.size();
        caretVisible_ = true;
        return true;
    }
    if (key == VK_DELETE)
    {
        if (HasSelection())
        {
            DeleteSelection();
        }
        else if (cursor_ < text_.size())
        {
            text_.erase(cursor_, 1);
        }
        else
        {
            return true;
        }
        anchor_ = cursor_;
        caretVisible_ = true;
        if (onTextChanged_)
        {
            onTextChanged_();
        }
        return true;
    }
    if (key != VK_LEFT && key != VK_RIGHT && key != VK_HOME &&
        key != VK_END)
    {
        return false;
    }

    std::size_t target = cursor_;
    if (key == VK_HOME)
    {
        target = 0;
    }
    else if (key == VK_END)
    {
        target = text_.size();
    }
    else if (key == VK_LEFT)
    {
        if (control)
        {
            while (target > 0 && iswspace(text_[target - 1]))
            {
                --target;
            }
            while (target > 0 && !iswspace(text_[target - 1]))
            {
                --target;
            }
        }
        else if (target > 0)
        {
            --target;
        }
    }
    else if (control)
    {
        while (target < text_.size() && iswspace(text_[target]))
        {
            ++target;
        }
        while (target < text_.size() && !iswspace(text_[target]))
        {
            ++target;
        }
    }
    else if (target < text_.size())
    {
        ++target;
    }
    if (!shift && HasSelection())
    {
        target = key == VK_LEFT || key == VK_HOME
            ? std::min(cursor_, anchor_)
            : std::max(cursor_, anchor_);
    }
    MoveCursor(target, shift);
    return true;
}

void SearchBar::DeleteSelection()
{
    if (!HasSelection())
    {
        return;
    }
    const std::size_t first = std::min(cursor_, anchor_);
    const std::size_t count =
        std::max(cursor_, anchor_) - first;
    text_.erase(first, count);
    cursor_ = first;
    anchor_ = first;
}

void SearchBar::MoveCursor(std::size_t position, bool extendSelection)
{
    cursor_ = std::min(position, text_.size());
    if (!extendSelection)
    {
        anchor_ = cursor_;
    }
    caretVisible_ = true;
}

void SearchBar::OnLButtonDown()
{
    SetFocus(true);
}

void SearchBar::SetFocus(bool focused)
{
    focused_ = focused;
    caretVisible_ = true;
}

void SearchBar::SetText(const std::wstring& text)
{
    text_ = text;
    cursor_ = text_.size();
    anchor_ = cursor_;
    caretVisible_ = true;
    if (onTextChanged_)
    {
        onTextChanged_();
    }
}

void SearchBar::Clear()
{
    if (text_.empty())
    {
        return;
    }
    text_.clear();
    cursor_ = 0;
    anchor_ = 0;
    caretVisible_ = true;
    if (onTextChanged_)
    {
        onTextChanged_();
    }
}

bool SearchBar::TickCaret()
{
    if (!focused_)
    {
        return false;
    }
    caretVisible_ = !caretVisible_;
    return true;
}

}  // namespace nexus::ui

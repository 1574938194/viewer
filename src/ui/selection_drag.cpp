#include "ui/selection_drag.h"

#include <algorithm>

namespace nexus::ui {
namespace {

D2D1_POINT_2F ContentPoint(POINT pointer, const D2D1_RECT_F& viewport,
                           float scroll)
{
  return {
      std::clamp(static_cast<float>(pointer.x), viewport.left, viewport.right) -
          viewport.left,
      std::clamp(static_cast<float>(pointer.y), viewport.top, viewport.bottom) -
          viewport.top + scroll};
}

}  // namespace

void SelectionDrag::Begin(POINT pointer, const D2D1_RECT_F& viewport,
                          float scroll, bool append,
                          const std::unordered_set<ResourceId>& selected,
                          ResourceId primary)
{
  active_ = true;
  append_ = append;
  original_ = selected;
  original_primary_ = primary;
  anchor_ = ContentPoint(pointer, viewport, scroll);
  Update(pointer, viewport, scroll);
}

void SelectionDrag::Update(POINT pointer, const D2D1_RECT_F& viewport,
                           float scroll)
{
  pointer_ = pointer;
  current_ = ContentPoint(pointer, viewport, scroll);
}

D2D1_RECT_F SelectionDrag::content_rect() const
{
  return {std::min(anchor_.x, current_.x), std::min(anchor_.y, current_.y),
          std::max(anchor_.x, current_.x), std::max(anchor_.y, current_.y)};
}

D2D1_RECT_F SelectionDrag::window_rect(const D2D1_RECT_F& viewport,
                                       float scroll) const
{
  const auto rect = content_rect();
  return {rect.left + viewport.left, rect.top + viewport.top - scroll,
          rect.right + viewport.left, rect.bottom + viewport.top - scroll};
}

std::unordered_set<ResourceId> SelectionDrag::Select(
    const std::vector<ResourceId>& hits) const
{
  std::unordered_set<ResourceId> selected =
      append_ ? original_ : std::unordered_set<ResourceId>();
  selected.insert(hits.begin(), hits.end());
  return selected;
}

float SelectionDrag::ScrollStep(const D2D1_RECT_F& viewport, float scale) const
{
  if (!active_ || pointer_.x < viewport.left || pointer_.x > viewport.right) {
    return 0.0f;
  }
  const float band =
      std::min(32.0f * scale, (viewport.bottom - viewport.top) / 2.0f);
  if (band <= 0.0f) {
    return 0.0f;
  }
  if (pointer_.y < viewport.top + band) {
    return -21.6f * scale *
           std::clamp((viewport.top + band - pointer_.y) / band, 0.0f, 1.0f);
  }
  if (pointer_.y > viewport.bottom - band) {
    return 21.6f * scale *
           std::clamp((pointer_.y - viewport.bottom + band) / band, 0.0f, 1.0f);
  }
  return 0.0f;
}

}  // namespace nexus::ui

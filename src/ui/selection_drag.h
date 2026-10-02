#ifndef NEXUS_SRC_UI_SELECTION_DRAG_H_
#define NEXUS_SRC_UI_SELECTION_DRAG_H_

#include <d2d1.h>

#include <unordered_set>
#include <vector>

#include "domain/resource.h"

namespace nexus::ui {

class SelectionDrag {
 public:
  void Begin(POINT pointer, const D2D1_RECT_F& viewport, float scroll,
             bool append, const std::unordered_set<ResourceId>& selected,
             ResourceId primary);
  void Update(POINT pointer, const D2D1_RECT_F& viewport, float scroll);
  void End() { active_ = false; }
  bool active() const { return active_; }
  POINT pointer() const { return pointer_; }
  ResourceId original_primary() const { return original_primary_; }
  const std::unordered_set<ResourceId>& original() const { return original_; }
  D2D1_RECT_F content_rect() const;
  D2D1_RECT_F window_rect(const D2D1_RECT_F& viewport, float scroll) const;
  std::unordered_set<ResourceId> Select(
      const std::vector<ResourceId>& hits) const;
  // Pixels per 30 ms tick; the edge band and speed scale with DPI.
  float ScrollStep(const D2D1_RECT_F& viewport, float scale) const;

 private:
  bool active_ = false;
  bool append_ = false;
  POINT pointer_ = {};
  D2D1_POINT_2F anchor_ = {};
  D2D1_POINT_2F current_ = {};
  ResourceId original_primary_ = 0;
  std::unordered_set<ResourceId> original_;
};

}  // namespace nexus::ui

#endif  // NEXUS_SRC_UI_SELECTION_DRAG_H_

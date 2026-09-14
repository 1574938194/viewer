#ifndef NEXUS_SRC_UI_RESOURCE_GRID_H_
#define NEXUS_SRC_UI_RESOURCE_GRID_H_

#include <d2d1.h>

#include <optional>
#include <unordered_set>
#include <vector>

#include "domain/resource.h"

namespace nexus::render {
class Renderer;
}

namespace nexus::service {
class ThumbnailService;
}

namespace nexus::ui {

struct GridView
{
    const std::vector<const Resource*>* items = nullptr;
    ResourceId hovered = 0;
    ResourceId selected = 0;
    const std::unordered_set<ResourceId>* selectedItems = nullptr;
    D2D1_RECT_F content = {};  // area available to the grid, in pixels
    float scrollY = 0.0f;
    float scale = 1.0f;
};

// Virtualized thumbnail grid (design doc section 54): only visible cards are
// drawn, and thumbnail requests are issued for visible cards each frame.
class ResourceGrid
{
public:
    static float CardWidth(float scale);

    static float CellHeight(float scale);

    static int Columns(float contentWidth, float scale);

    static float ContentHeight(const GridView& view);

    static std::optional<ResourceId> HitTest(const GridView& view, POINT point);

    static void Draw(render::Renderer* renderer, const GridView& view,
                     service::ThumbnailService* thumbnails);
};

}  // namespace nexus::ui

#endif  // NEXUS_SRC_UI_RESOURCE_GRID_H_

#include "ui/resource_grid.h"

#include <algorithm>
#include <string>

#include "render/renderer.h"
#include "render/theme.h"
#include "service/thumbnail_service.h"
#include "util/string_util.h"

namespace nexus::ui {

namespace {

constexpr float kCardWidthDp = 176.0f;
constexpr float kThumbHeightDp = 99.0f;
constexpr float kInfoHeightDp = 42.0f;
constexpr float kGapXDp = 12.0f;
constexpr float kGapYDp = 14.0f;
constexpr float kPaddingDp = 16.0f;
constexpr float kCardRadiusDp = 10.0f;

struct CardGeometry
{
    int columns = 1;
    float cardWidth = 0.0f;
    float thumbHeight = 0.0f;
    float infoHeight = 0.0f;
    float cellStepX = 0.0f;
    float cellStepY = 0.0f;
    float padding = 0.0f;
};

CardGeometry Geometry(const GridView& view)
{
    CardGeometry g;
    const float scale = view.scale;
    g.cardWidth = kCardWidthDp * scale;
    g.thumbHeight = kThumbHeightDp * scale;
    g.infoHeight = kInfoHeightDp * scale;
    g.padding = kPaddingDp * scale;
    const float gapX = kGapXDp * scale;
    const float gapY = kGapYDp * scale;
    g.cellStepX = g.cardWidth + gapX;
    g.cellStepY = g.thumbHeight + g.infoHeight + gapY;
    const float contentWidth = view.content.right - view.content.left;
    const float usable = contentWidth - 2.0f * g.padding + gapX;
    g.columns = std::max(1, static_cast<int>(usable / g.cellStepX));
    return g;
}

void DrawPlaceholder(render::Renderer* renderer, const D2D1_RECT_F& thumbRect,
                     const Resource& item)
{
    const float scale = renderer->scale();
    const float centerX = (thumbRect.left + thumbRect.right) / 2.0f;
    D2D1_RECT_F glyphRect = {thumbRect.left, thumbRect.top + 12.0f * scale,
                             thumbRect.right, thumbRect.bottom - 18.0f * scale};
    D2D1_RECT_F noteRect = {thumbRect.left, thumbRect.bottom - 20.0f * scale,
                            thumbRect.right, thumbRect.bottom - 4.0f * scale};
    if (item.missing)
    {
        renderer->DrawTextWide(L"▶", glyphRect, render::TextRole::kTitle,
                               render::Theme::kTextSecondary);
        renderer->DrawTextWide(L"文件缺失", noteRect,
                               render::TextRole::kSecondary,
                               render::Theme::kDanger);
    }
    else if (item.type == ResourceType::kVideo)
    {
        renderer->DrawTextWide(L"▶", glyphRect, render::TextRole::kTitle,
                               render::Theme::kTextSecondary);
        renderer->DrawTextWide(L"生成缩略图中…", noteRect,
                               render::TextRole::kSecondary,
                               render::Theme::kTextSecondary);
    }
    else
    {
        renderer->DrawTextWide(L"加载中…", glyphRect,
                               render::TextRole::kSecondary,
                               render::Theme::kTextSecondary);
    }
    (void)centerX;
}

}  // namespace

float ResourceGrid::CardWidth(float scale)
{
    return kCardWidthDp * scale;
}

float ResourceGrid::CellHeight(float scale)
{
    return (kThumbHeightDp + kInfoHeightDp) * scale;
}

int ResourceGrid::Columns(float contentWidth, float scale)
{
    GridView view;
    view.scale = scale;
    view.content = {0.0f, 0.0f, contentWidth, 1.0f};
    return Geometry(view).columns;
}

float ResourceGrid::ContentHeight(const GridView& view)
{
    if (view.items == nullptr || view.items->empty())
    {
        return 0.0f;
    }
    CardGeometry g = Geometry(view);
    const int count = static_cast<int>(view.items->size());
    const int rows = (count + g.columns - 1) / g.columns;
    const float gapY = kGapYDp * view.scale;
    return 2.0f * g.padding + rows * (g.thumbHeight + g.infoHeight) +
           (rows - 1) * gapY;
}

std::optional<ResourceId> ResourceGrid::HitTest(const GridView& view,
                                                POINT point)
{
    if (view.items == nullptr)
    {
        return std::nullopt;
    }
    CardGeometry g = Geometry(view);
    const float x = static_cast<float>(point.x) - view.content.left - g.padding;
    const float y = static_cast<float>(point.y) - view.content.top +
                    view.scrollY - g.padding;
    if (x < 0.0f || y < 0.0f)
    {
        return std::nullopt;
    }

    const int column = static_cast<int>(x / g.cellStepX);
    const int row = static_cast<int>(y / g.cellStepY);
    if (column >= g.columns)
    {
        return std::nullopt;
    }
    // Reject clicks in the gaps between cards.
    if (x - column * g.cellStepX > g.cardWidth ||
        y - row * g.cellStepY > g.thumbHeight + g.infoHeight)
    {
        return std::nullopt;
    }

    const size_t index =
        static_cast<size_t>(row) * g.columns + static_cast<size_t>(column);
    if (index >= view.items->size())
    {
        return std::nullopt;
    }
    return (*view.items)[index]->id;
}

void ResourceGrid::Draw(render::Renderer* renderer, const GridView& view,
                        service::ThumbnailService* thumbnails)
{
    if (view.items == nullptr)
    {
        return;
    }
    CardGeometry g = Geometry(view);

    const int count = static_cast<int>(view.items->size());
    for (int index = 0; index < count; ++index)
    {
        const int row = index / g.columns;
        const int column = index % g.columns;
        const float x = view.content.left + g.padding + column * g.cellStepX;
        const float y = view.content.top + g.padding - view.scrollY +
                        row * g.cellStepY;
        const float cardBottom = y + g.thumbHeight + g.infoHeight;
        if (cardBottom < view.content.top || y > view.content.bottom)
        {
            continue;  // virtualization: skip rows outside the viewport
        }

        const Resource* item = (*view.items)[index];
        const D2D1_RECT_F cardRect = {x, y, x + g.cardWidth, cardBottom};
        const float radius = kCardRadiusDp * view.scale;
        const bool hovered = item->id == view.hovered;
        const bool selected = view.selectedItems != nullptr
            ? view.selectedItems->contains(item->id)
            : item->id == view.selected;

        renderer->FillRoundRect(cardRect,
                                hovered ? render::Theme::kCardHover
                                        : render::Theme::kCard,
                                radius);
        if (selected)
        {
            renderer->StrokeRoundRect(cardRect, render::Theme::kAccent,
                                      1.5f * view.scale, radius);
        }
        else if (hovered)
        {
            renderer->StrokeRoundRect(cardRect, render::Theme::kBorder,
                                      1.0f * view.scale, radius);
        }

        const D2D1_RECT_F thumbRect = {x, y, x + g.cardWidth,
                                       y + g.thumbHeight};
        renderer->FillRoundRect(thumbRect, render::Theme::kInput, radius);
        renderer->PushRoundedClip(thumbRect, radius);
        thumbnails->Request(*item);
        ID2D1Bitmap* bitmap = thumbnails->BitmapFor(item->id, renderer);
        if (bitmap != nullptr)
        {
            const D2D1_SIZE_F size = bitmap->GetSize();
            const float fit = std::min(g.cardWidth / size.width,
                                       g.thumbHeight / size.height);
            const float drawWidth = size.width * fit;
            const float drawHeight = size.height * fit;
            const D2D1_RECT_F dest = {
                x + (g.cardWidth - drawWidth) / 2.0f,
                y + (g.thumbHeight - drawHeight) / 2.0f,
                x + (g.cardWidth + drawWidth) / 2.0f,
                y + (g.thumbHeight + drawHeight) / 2.0f};
            renderer->DrawBitmap(bitmap, dest);
        }
        else
        {
            DrawPlaceholder(renderer, thumbRect, *item);
        }
        renderer->PopClip();

        const float infoTop = y + g.thumbHeight;
        const float inset = 8.0f * view.scale;
        D2D1_RECT_F titleRect = {x + inset, infoTop + 4.0f * view.scale,
                                 x + g.cardWidth - inset,
                                 infoTop + 22.0f * view.scale};
        std::string title;
        if (item->title.empty())
        {
            title = item->code;
        }
        else if (item->code.empty() || item->title == item->code)
        {
            title = item->title;
        }
        else
        {
            title = item->code + " " + item->title;
        }
        renderer->DrawTextUtf8(title, titleRect, render::TextRole::kTitle,
                               render::Theme::kText);

        std::string tags;
        for (size_t i = 0; i < item->tags.size(); ++i)
        {
            if (i > 0)
            {
                tags += " · ";
            }
            tags += item->tags[i];
        }
        D2D1_RECT_F tagRect = {x + inset, infoTop + 22.0f * view.scale,
                               x + g.cardWidth - inset,
                               infoTop + 40.0f * view.scale};
        renderer->DrawTextUtf8(tags, tagRect, render::TextRole::kSecondary,
                               render::Theme::kTextSecondary);

        if (item->favorite)
        {
            D2D1_RECT_F starRect = {x + g.cardWidth - 22.0f * view.scale,
                                    infoTop + 3.0f * view.scale,
                                    x + g.cardWidth - 4.0f * view.scale,
                                    infoTop + 21.0f * view.scale};
            renderer->DrawTextWide(L"★", starRect, render::TextRole::kTitle,
                                   render::Theme::kStar);
        }
    }
}

}  // namespace nexus::ui

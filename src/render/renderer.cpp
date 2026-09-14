#include "render/renderer.h"

#include <algorithm>
#include <utility>

#include "util/string_util.h"

namespace nexus::render {

namespace {

constexpr float kFontSizes[] = {13.0f, 12.0f, 14.0f, 12.0f};

D2D1_RECT_F InsetRect(const D2D1_RECT_F& rect, float inset)
{
    return {rect.left + inset, rect.top + inset, rect.right - inset,
            rect.bottom - inset};
}

}  // namespace

bool Renderer::Init(HWND hwnd, float scale)
{
    hwnd_ = hwnd;
    scale_ = scale;

    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   factory_.GetAddressOf());
    if (FAILED(hr))
    {
        return false;
    }

    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));
    if (FAILED(hr))
    {
        return false;
    }

    RECT client = {};
    GetClientRect(hwnd, &client);
    UINT width =
        std::max<UINT>(static_cast<UINT>(client.right - client.left), 1);
    UINT height =
        std::max<UINT>(static_cast<UINT>(client.bottom - client.top), 1);

    auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    hr = factory_->CreateHwndRenderTarget(
        props, D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(width, height)),
        &rt_);
    if (FAILED(hr))
    {
        return false;
    }

    RecreateFormats();

    hr = rt_->CreateSolidColorBrush(Theme::kText, brush_.GetAddressOf());
    return SUCCEEDED(hr);
}

void Renderer::Shutdown()
{
    for (auto& format : formats_)
    {
        format = nullptr;
    }
    brush_ = nullptr;
    rt_ = nullptr;
    dwrite_ = nullptr;
    factory_ = nullptr;
}

void Renderer::RecreateFormats()
{
    for (int i = 0; i < 4; ++i)
    {
        float size = kFontSizes[i] * scale_;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
        HRESULT hr = dwrite_->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
            L"zh-CN", format.GetAddressOf());
        if (FAILED(hr))
        {
            continue;
        }
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        if (i == static_cast<int>(TextRole::kSearch))
        {
            format->SetParagraphAlignment(
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0,
                                    0};
        format->SetTrimming(&trimming, nullptr);
        formats_[i] = std::move(format);
    }
}

IDWriteTextFormat* Renderer::Format(TextRole role)
{
    int index = static_cast<int>(role);
    if (index < 0 || index >= 4)
    {
        return nullptr;
    }
    return formats_[index].Get();
}

void Renderer::Resize(UINT width, UINT height)
{
    if (rt_)
    {
        rt_->Resize(D2D1::SizeU(std::max<UINT>(width, 1),
                                std::max<UINT>(height, 1)));
    }
}

void Renderer::SetScale(float scale)
{
    if (scale_ == scale)
    {
        return;
    }
    scale_ = scale;
    for (auto& format : formats_)
    {
        format = nullptr;
    }
    RecreateFormats();}

void Renderer::Begin()
{
    if (rt_)
    {
        rt_->BeginDraw();
    }
}

bool Renderer::End()
{
    if (!rt_)
    {
        return false;
    }
    return SUCCEEDED(rt_->EndDraw());
}

void Renderer::Clear(const D2D1_COLOR_F& color)
{
    if (rt_)
    {
        rt_->Clear(color);
    }
}

void Renderer::FillRect(const D2D1_RECT_F& rect, const D2D1_COLOR_F& color)
{
    if (!rt_ || !brush_)
    {
        return;
    }
    brush_->SetColor(color);
    rt_->FillRectangle(rect, brush_.Get());
}

void Renderer::FillRoundRect(const D2D1_RECT_F& rect,
                             const D2D1_COLOR_F& color, float radius)
{
    if (!rt_ || !brush_)
    {
        return;
    }
    brush_->SetColor(color);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius),
                              brush_.Get());
}

void Renderer::StrokeRect(const D2D1_RECT_F& rect, const D2D1_COLOR_F& color,
                          float width)
{
    if (!rt_ || !brush_)
    {
        return;
    }
    brush_->SetColor(color);
    rt_->DrawRectangle(rect, brush_.Get(), width);
}

void Renderer::StrokeRoundRect(const D2D1_RECT_F& rect,
                               const D2D1_COLOR_F& color, float width,
                               float radius)
{
    if (!rt_ || !brush_)
    {
        return;
    }
    brush_->SetColor(color);
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius),
                              brush_.Get(), width);
}

void Renderer::PushRoundedClip(const D2D1_RECT_F& rect, float radius)
{
    if (!rt_ || !factory_ || radius <= 0.0f)
    {
        return;
    }
    Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> geometry;
    HRESULT hr = factory_->CreateRoundedRectangleGeometry(
        D2D1::RoundedRect(rect, radius, radius), geometry.GetAddressOf());
    if (FAILED(hr) || !geometry)
    {
        return;
    }
    if (!clipLayer_)
    {
        hr = rt_->CreateLayer(clipLayer_.GetAddressOf());
        if (FAILED(hr))
        {
            return;
        }
    }
    D2D1_LAYER_PARAMETERS parameters =
        D2D1::LayerParameters(D2D1::InfiniteRect(), geometry.Get());
    rt_->PushLayer(parameters, clipLayer_.Get());
    ++clipDepth_;
}

void Renderer::PopClip()
{
    if (!rt_ || clipDepth_ == 0)
    {
        return;
    }
    rt_->PopLayer();
    --clipDepth_;
}

void Renderer::DrawBitmap(ID2D1Bitmap* bitmap, const D2D1_RECT_F& dest)
{
    if (!rt_)
    {
        return;
    }
    rt_->DrawBitmap(bitmap, &dest);
}

void Renderer::DrawTextWide(const std::wstring& text, const D2D1_RECT_F& rect,
                            TextRole role, const D2D1_COLOR_F& color)
{
    if (!rt_ || !brush_ || text.empty())
    {
        return;
    }
    IDWriteTextFormat* format = Format(role);
    if (!format)
    {
        return;
    }
    brush_->SetColor(color);
    rt_->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format, rect,
                  brush_.Get());
}

void Renderer::DrawTextUtf8(const std::string& text, const D2D1_RECT_F& rect,
                            TextRole role, const D2D1_COLOR_F& color)
{
    DrawTextWide(Utf8ToWide(text), rect, role, color);
}

float Renderer::MeasureTextWide(const std::wstring& text, TextRole role)
{
    if (!dwrite_ || text.empty())
    {
        return 0.0f;
    }
    IDWriteTextFormat* format = Format(role);
    if (!format)
    {
        return 0.0f;
    }
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    HRESULT hr = dwrite_->CreateTextLayout(
        text.c_str(), static_cast<UINT32>(text.size()), format, 10000.0f,
        1000.0f, layout.GetAddressOf());
    if (FAILED(hr))
    {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics = {};
    layout->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
}

bool Renderer::CreateBitmap(UINT width, UINT height, const std::uint8_t* bgra,
                            ID2D1Bitmap** out)
{
    if (!rt_ || width == 0 || height == 0 || bgra == nullptr)
    {
        return false;
    }
    D2D1_BITMAP_PROPERTIES props = {};
    props.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM,
                         D2D1_ALPHA_MODE_PREMULTIPLIED};
    props.dpiX = 96.0f;
    props.dpiY = 96.0f;
    HRESULT hr = rt_->CreateBitmap(D2D1::SizeU(width, height), bgra,
                                   width * 4, props, out);
    return SUCCEEDED(hr);
}

}  // namespace nexus::render

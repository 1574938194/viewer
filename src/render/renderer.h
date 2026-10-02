#ifndef NEXUS_SRC_RENDER_RENDERER_H_
#define NEXUS_SRC_RENDER_RENDERER_H_

#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <string>

#include "render/theme.h"

namespace nexus::render {

// Owns the Direct2D / DirectWrite state for the main window. All drawing
// happens on the UI thread. Coordinates are physical pixels; sizes passed in
// as dp must be multiplied by scale().
class Renderer
{
public:
    bool Init(HWND hwnd, float scale);

    void Shutdown();

    void Resize(UINT width, UINT height);

    void SetScale(float scale);

    float scale() const { return scale_; }

    void Begin();

    // Returns false when the device must be recreated (D2DERR_RECREATE_TARGET).
    bool End();

    void Clear(const D2D1_COLOR_F& color);

    void FillRect(const D2D1_RECT_F& rect, const D2D1_COLOR_F& color);

    void FillRoundRect(const D2D1_RECT_F& rect, const D2D1_COLOR_F& color,
                       float radius);

    void StrokeRect(const D2D1_RECT_F& rect, const D2D1_COLOR_F& color,
                    float width);

    void StrokeRoundRect(const D2D1_RECT_F& rect, const D2D1_COLOR_F& color,
                         float width, float radius);

    // Clips subsequent drawing to a rounded rectangle until the matching
    // PopClip; used to round bitmap thumbnails.
    void PushRoundedClip(const D2D1_RECT_F& rect, float radius);

    void PopClip();

    // Clips drawing to an axis-aligned content viewport. This is separate
    // from the rounded layer stack because grid viewports are rectangular.
    void PushRectClip(const D2D1_RECT_F& rect);

    void PopRectClip();

    void DrawBitmap(ID2D1Bitmap* bitmap, const D2D1_RECT_F& dest);

    // Rasterizes and caches the embedded application icon at the current size.
    void DrawIcon(HICON icon, const D2D1_RECT_F& dest);

    void DrawTextWide(const std::wstring& text, const D2D1_RECT_F& rect,
                      TextRole role, const D2D1_COLOR_F& color);

    void DrawTextUtf8(const std::string& text, const D2D1_RECT_F& rect,
                      TextRole role, const D2D1_COLOR_F& color);

    float MeasureTextWide(const std::wstring& text, TextRole role);

    bool CreateBitmap(UINT width, UINT height, const std::uint8_t* bgra,
                      ID2D1Bitmap** out);

private:
    void RecreateFormats();

    IDWriteTextFormat* Format(TextRole role);

    HWND hwnd_ = nullptr;
    float scale_ = 1.0f;
    Microsoft::WRL::ComPtr<ID2D1Factory1> factory_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> rt_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> iconBitmap_;
    HICON cachedIcon_ = nullptr;
    UINT iconSize_ = 0;
    Microsoft::WRL::ComPtr<ID2D1Layer> clipLayer_;
    int clipDepth_ = 0;
    int rectClipDepth_ = 0;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> formats_[4];
};

}  // namespace nexus::render

#endif  // NEXUS_SRC_RENDER_RENDERER_H_

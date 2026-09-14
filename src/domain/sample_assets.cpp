#include "domain/sample_assets.h"

#include <Windows.h>

#include <gdiplus.h>
#include <objidl.h>

#include <cstdio>
#include <random>
#include <vector>

#include "util/string_util.h"

namespace nexus {

namespace {

struct Palette
{
    Gdiplus::Color from;
    Gdiplus::Color to;
};

const Palette kPalettes[] = {
    {Gdiplus::Color(255, 42, 54, 74), Gdiplus::Color(255, 26, 32, 44)},
    {Gdiplus::Color(255, 62, 48, 60), Gdiplus::Color(255, 30, 26, 32)},
    {Gdiplus::Color(255, 38, 58, 52), Gdiplus::Color(255, 22, 32, 30)},
    {Gdiplus::Color(255, 58, 50, 34), Gdiplus::Color(255, 32, 28, 20)},
    {Gdiplus::Color(255, 44, 42, 64), Gdiplus::Color(255, 26, 24, 40)},
    {Gdiplus::Color(255, 48, 48, 48), Gdiplus::Color(255, 24, 24, 24)},
};

int GetEncoderClsid(const WCHAR* mime, CLSID* clsid)
{
    UINT num = 0;
    UINT size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0)
    {
        return -1;
    }
    std::vector<char> buffer(size);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    Gdiplus::GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i)
    {
        if (wcscmp(codecs[i].MimeType, mime) == 0)
        {
            *clsid = codecs[i].Clsid;
            return static_cast<int>(i);
        }
    }
    return -1;
}

void GenerateSampleImage(const std::filesystem::path& path, int index,
                         const std::wstring& code)
{
    const int kWidth = 1280;
    const int kHeight = 720;

    Gdiplus::Bitmap bitmap(kWidth, kHeight, PixelFormat32bppARGB);
    Gdiplus::Graphics graphics(&bitmap);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);

    const Palette& palette = kPalettes[index % 6];
    Gdiplus::LinearGradientBrush background(
        Gdiplus::Rect(0, 0, kWidth, kHeight), palette.from, palette.to,
        Gdiplus::LinearGradientModeBackwardDiagonal);
    graphics.FillRectangle(&background, 0, 0, kWidth, kHeight);

    std::mt19937 rng(static_cast<unsigned>(index) * 7919u + 13u);
    std::uniform_int_distribution<int> distX(0, kWidth);
    std::uniform_int_distribution<int> distY(0, kHeight);
    std::uniform_int_distribution<int> distR(40, 240);
    for (int i = 0; i < 14; ++i)
    {
        Gdiplus::SolidBrush circle(Gdiplus::Color(16, 255, 255, 255));
        int radius = distR(rng);
        graphics.FillEllipse(&circle, distX(rng) - radius, distY(rng) - radius,
                             radius * 2, radius * 2);
    }

    Gdiplus::FontFamily family(L"Segoe UI");
    Gdiplus::Font big(&family, 88, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font caption(&family, 34, Gdiplus::FontStyleRegular,
                          Gdiplus::UnitPixel);
    Gdiplus::SolidBrush foreground(Gdiplus::Color(225, 238, 240, 244));
    Gdiplus::SolidBrush dim(Gdiplus::Color(150, 200, 205, 215));
    graphics.DrawString(code.c_str(), -1, &big, Gdiplus::PointF(56, 56),
                        &foreground);
    graphics.DrawString(L"NEXUS SAMPLE", -1, &caption,
                        Gdiplus::PointF(60, kHeight - 90), &dim);

    CLSID pngClsid = {};
    if (GetEncoderClsid(L"image/png", &pngClsid) < 0)
    {
        return;
    }
    bitmap.Save(path.c_str(), &pngClsid, nullptr);
}

}  // namespace

std::filesystem::path SampleAssets::SampleDir()
{
    wchar_t buffer[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path() / L"sample";
}

void SampleAssets::EnsureSampleImages(const std::filesystem::path& dir)
{
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok)
    {
        return;
    }
    for (int i = 0; i < 18; ++i)
    {
        wchar_t fileName[32] = {};
        swprintf(fileName, 32, L"sample-%02d.png", i);
        std::filesystem::path path = dir / fileName;
        if (std::filesystem::exists(path))
        {
            continue;
        }
        wchar_t code[16] = {};
        swprintf(code, 16, L"IMG-%03d", 100 + i);
        GenerateSampleImage(path, i, code);
    }
    Gdiplus::GdiplusShutdown(token);
}

}  // namespace nexus

#include "service/thumbnail_service.h"

#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>

#include "render/renderer.h"
#include "util/string_util.h"

namespace nexus::service {

namespace {

constexpr std::uint32_t kMaxThumbWidth = 320;
constexpr std::uint32_t kMaxThumbHeight = 180;
constexpr size_t kCacheLimit = 256;
constexpr std::uint32_t kDiskMagic = 0x4E585448;  // "NXTH"
constexpr std::uint32_t kDiskVersion = 1;

std::string CacheKey(const Resource& resource)
{
    const std::string input =
        WideToUtf8(resource.path.wstring()) + "\n" +
        std::to_string(resource.fileSize) + "\n" +
        std::to_string(resource.modifiedTime) + "\n" +
        std::to_string(static_cast<int>(resource.type));
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char character : input)
    {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}

bool ReadDiskCache(const std::filesystem::path& path,
                   std::vector<std::uint8_t>* pixels,
                   std::uint32_t* width, std::uint32_t* height)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return false;
    }
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t cachedWidth = 0;
    std::uint32_t cachedHeight = 0;
    std::uint64_t byteCount = 0;
    input.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    input.read(reinterpret_cast<char*>(&version), sizeof(version));
    input.read(reinterpret_cast<char*>(&cachedWidth), sizeof(cachedWidth));
    input.read(reinterpret_cast<char*>(&cachedHeight), sizeof(cachedHeight));
    input.read(reinterpret_cast<char*>(&byteCount), sizeof(byteCount));
    const std::uint64_t expected = static_cast<std::uint64_t>(cachedWidth) *
                                   cachedHeight * 4;
    if (!input || magic != kDiskMagic || version != kDiskVersion ||
        cachedWidth == 0 || cachedHeight == 0 ||
        cachedWidth > kMaxThumbWidth || cachedHeight > kMaxThumbHeight ||
        byteCount != expected || byteCount > kMaxThumbWidth *
                                                  kMaxThumbHeight * 4)
    {
        return false;
    }
    pixels->resize(static_cast<size_t>(byteCount));
    input.read(reinterpret_cast<char*>(pixels->data()),
               static_cast<std::streamsize>(byteCount));
    if (!input)
    {
        pixels->clear();
        return false;
    }
    *width = cachedWidth;
    *height = cachedHeight;
    return true;
}

void WriteDiskCache(const std::filesystem::path& path,
                    const std::vector<std::uint8_t>& pixels,
                    std::uint32_t width, std::uint32_t height)
{
    if (pixels.empty() || width == 0 || height == 0 ||
        pixels.size() != static_cast<size_t>(width) * height * 4)
    {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
    {
        return;
    }
    const std::filesystem::path temporary =
        path.wstring() + L".tmp-" +
        std::to_wstring(GetCurrentThreadId());
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        return;
    }
    const std::uint64_t byteCount = pixels.size();
    output.write(reinterpret_cast<const char*>(&kDiskMagic),
                 sizeof(kDiskMagic));
    output.write(reinterpret_cast<const char*>(&kDiskVersion),
                 sizeof(kDiskVersion));
    output.write(reinterpret_cast<const char*>(&width), sizeof(width));
    output.write(reinterpret_cast<const char*>(&height), sizeof(height));
    output.write(reinterpret_cast<const char*>(&byteCount), sizeof(byteCount));
    output.write(reinterpret_cast<const char*>(pixels.data()),
                 static_cast<std::streamsize>(pixels.size()));
    output.close();
    if (!output)
    {
        std::filesystem::remove(temporary, error);
        return;
    }
    if (std::filesystem::exists(path, error))
    {
        std::filesystem::remove(temporary, error);
        return;
    }
    std::filesystem::rename(temporary, path, error);
    if (error)
    {
        std::filesystem::remove(temporary, error);
    }
}

// Decodes an image file and scales it to fit inside the thumbnail box,
// preserving the aspect ratio. Output is 32bpp BGRA premultiplied.
bool DecodeImage(const std::filesystem::path& path,
                 std::vector<std::uint8_t>* pixels, std::uint32_t* outWidth,
                 std::uint32_t* outHeight)
{
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (FAILED(hr))
    {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    hr = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                        WICDecodeMetadataCacheOnDemand,
                                        &decoder);
    if (FAILED(hr))
    {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr))
    {
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    frame->GetSize(&width, &height);
    if (width == 0 || height == 0)
    {
        return false;
    }

    double scale = std::min(
        1.0, std::min(static_cast<double>(kMaxThumbWidth) / width,
                      static_cast<double>(kMaxThumbHeight) / height));
    UINT targetWidth = std::max<UINT>(
        1, static_cast<UINT>(static_cast<double>(width) * scale + 0.5));
    UINT targetHeight = std::max<UINT>(
        1, static_cast<UINT>(static_cast<double>(height) * scale + 0.5));

    Microsoft::WRL::ComPtr<IWICBitmapSource> source = frame;
    if (targetWidth != width || targetHeight != height)
    {
        Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
        hr = wic->CreateBitmapScaler(&scaler);
        if (SUCCEEDED(hr))
        {
            hr = scaler->Initialize(frame.Get(), targetWidth, targetHeight,
                                    WICBitmapInterpolationModeFant);
            if (SUCCEEDED(hr))
            {
                source = scaler;
            }
        }
    }

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    hr = wic->CreateFormatConverter(&converter);
    if (FAILED(hr))
    {
        return false;
    }
    hr = converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA,
                               WICBitmapDitherTypeNone, nullptr, 0.0f,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr))
    {
        return false;
    }

    std::vector<std::uint8_t> buffer(static_cast<size_t>(targetWidth) *
                                     targetHeight * 4);
    hr = converter->CopyPixels(nullptr, targetWidth * 4,
                               static_cast<UINT>(buffer.size()),
                               buffer.data());
    if (FAILED(hr))
    {
        return false;
    }

    *pixels = std::move(buffer);
    *outWidth = targetWidth;
    *outHeight = targetHeight;
    return true;
}

bool HBitmapToPixels(HBITMAP bitmap, std::vector<std::uint8_t>* pixels,
                     std::uint32_t* outWidth, std::uint32_t* outHeight)
{
    BITMAP info = {};
    if (GetObjectW(bitmap, sizeof(info), &info) == 0)
    {
        return false;
    }
    const int width = info.bmWidth;
    const int height = info.bmHeight < 0 ? -info.bmHeight : info.bmHeight;
    if (width <= 0 || height <= 0)
    {
        return false;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    std::vector<std::uint8_t> buffer(static_cast<size_t>(width) * height * 4);
    HDC hdc = GetDC(nullptr);
    int rows = GetDIBits(hdc, bitmap, 0, height, buffer.data(), &bmi,
                         DIB_RGB_COLORS);
    ReleaseDC(nullptr, hdc);
    if (rows != height)
    {
        return false;
    }

    // Make premultiplied so D2D can consume it directly.
    for (size_t i = 0; i + 3 < buffer.size(); i += 4)
    {
        unsigned alpha = buffer[i + 3];
        if (alpha != 255)
        {
            buffer[i + 0] = static_cast<std::uint8_t>(buffer[i + 0] * alpha / 255);
            buffer[i + 1] = static_cast<std::uint8_t>(buffer[i + 1] * alpha / 255);
            buffer[i + 2] = static_cast<std::uint8_t>(buffer[i + 2] * alpha / 255);
        }
    }

    *pixels = std::move(buffer);
    *outWidth = static_cast<std::uint32_t>(width);
    *outHeight = static_cast<std::uint32_t>(height);
    return true;
}

// Uses the Windows Shell thumbnail cache for videos.
bool DecodeShellThumbnail(const std::filesystem::path& path,
                          std::vector<std::uint8_t>* pixels,
                          std::uint32_t* outWidth, std::uint32_t* outHeight)
{
    Microsoft::WRL::ComPtr<IShellItemImageFactory> factory;
    HRESULT hr = SHCreateItemFromParsingName(path.c_str(), nullptr,
                                             IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        return false;
    }

    HBITMAP bitmap = nullptr;
    hr = factory->GetImage({kMaxThumbWidth, kMaxThumbHeight},
                           SIIGBF_RESIZETOFIT | SIIGBF_THUMBNAILONLY, &bitmap);
    if (FAILED(hr))
    {
        hr = factory->GetImage({kMaxThumbWidth, kMaxThumbHeight},
                               SIIGBF_RESIZETOFIT, &bitmap);
    }
    if (FAILED(hr) || bitmap == nullptr)
    {
        return false;
    }
    bool ok = HBitmapToPixels(bitmap, pixels, outWidth, outHeight);
    DeleteObject(bitmap);
    return ok;
}

}  // namespace

void ThumbnailService::Start(HWND hwnd, UINT readyMessage,
                             std::filesystem::path cacheRoot)
{
    if (!workers_.empty())
    {
        return;
    }
    hwnd_ = hwnd;
    readyMessage_ = readyMessage;
    cacheRoot_ = std::move(cacheRoot);
    stop_ = false;
    for (int i = 0; i < 4; ++i)
    {
        workers_.emplace_back([this] { WorkerLoop(); });
    }
}

void ThumbnailService::Shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (std::thread& worker : workers_)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
    workers_.clear();
}

void ThumbnailService::Request(const Resource& resource)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string cacheKey = CacheKey(resource);
    auto it = cache_.find(resource.id);
    if (it != cache_.end())
    {
        if (it->second.cacheKey != cacheKey)
        {
            cache_.erase(it);
            it = cache_.end();
        }
    }
    if (it != cache_.end())
    {
        it->second.lastUse = ++useCounter_;
        return;
    }

    Entry& entry = cache_[resource.id];
    entry.state = resource.missing ? State::kFailed : State::kLoading;
    entry.lastUse = ++useCounter_;
    if (!resource.missing)
    {
        std::filesystem::path cacheFile;
        if (!cacheRoot_.empty())
        {
            cacheFile = cacheRoot_ /
                        std::filesystem::path(Utf8ToWide(cacheKey + ".bin"));
        }
        entry.cacheKey = cacheKey;
        queue_.push_back(
            {resource.id, resource.path, cacheFile, cacheKey, resource.type});
        cv_.notify_one();
    }
}

ThumbnailService::State ThumbnailService::StateOf(ResourceId id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(id);
    return it == cache_.end() ? State::kLoading : it->second.state;
}

ID2D1Bitmap* ThumbnailService::BitmapFor(ResourceId id,
                                         render::Renderer* renderer)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(id);
    if (it == cache_.end() || it->second.state != State::kReady)
    {
        return nullptr;
    }
    Entry& entry = it->second;
    entry.lastUse = ++useCounter_;
    if (!entry.bitmap && !entry.pixels.empty())
    {
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        if (renderer->CreateBitmap(entry.width, entry.height,
                                   entry.pixels.data(),
                                   bitmap.ReleaseAndGetAddressOf()))
        {
            entry.bitmap = bitmap;
        }
    }
    return entry.bitmap.Get();
}

void ThumbnailService::Evict(ResourceId id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.erase(id);
}

void ThumbnailService::Complete(ResourceId id, const std::string& cacheKey,
                                std::vector<std::uint8_t> pixels,
                                std::uint32_t width, std::uint32_t height)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(id);
        if (it == cache_.end() || it->second.cacheKey != cacheKey)
        {
            return;
        }
        Entry& entry = it->second;
        entry.state = State::kReady;
        entry.pixels = std::move(pixels);
        entry.width = width;
        entry.height = height;
        entry.cacheKey = cacheKey;
    }
    if (hwnd_ != nullptr && IsWindow(hwnd_))
    {
        PostMessageW(hwnd_, readyMessage_, static_cast<WPARAM>(id), 0);
    }
}

void ThumbnailService::TrimCache()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (cache_.size() <= kCacheLimit)
    {
        return;
    }
    // Evict ready entries with the oldest use; loading entries stay.
    std::vector<std::pair<std::uint64_t, ResourceId>> ready;
    ready.reserve(cache_.size());
    for (const auto& [id, entry] : cache_)
    {
        if (entry.state == State::kReady)
        {
            ready.emplace_back(entry.lastUse, id);
        }
    }
    size_t toEvict = cache_.size() - kCacheLimit;
    if (ready.size() <= toEvict)
    {
        return;
    }
    std::sort(ready.begin(), ready.end());
    for (size_t i = 0; i < toEvict; ++i)
    {
        cache_.erase(ready[i].second);
    }
}

void ThumbnailService::WorkerLoop()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInitialized = SUCCEEDED(hr);

    while (true)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock,
                     [this] { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty())
            {
                break;
            }
            job = queue_.front();
            queue_.pop_front();
        }

        std::vector<std::uint8_t> pixels;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        bool ok = !job.cacheFile.empty() &&
                  ReadDiskCache(job.cacheFile, &pixels, &width, &height);
        if (!ok && job.type == ResourceType::kImage)
        {
            ok = DecodeImage(job.path, &pixels, &width, &height);
        }
        else if (!ok)
        {
            ok = DecodeShellThumbnail(job.path, &pixels, &width, &height);
        }

        if (ok)
        {
            if (!job.cacheFile.empty())
            {
                WriteDiskCache(job.cacheFile, pixels, width, height);
            }
            Complete(job.id, job.cacheKey, std::move(pixels), width, height);
        }
        else
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = cache_.find(job.id);
                if (it == cache_.end() || it->second.cacheKey != job.cacheKey)
                {
                    continue;
                }
                it->second.state = State::kFailed;
            }
            if (hwnd_ != nullptr && IsWindow(hwnd_))
            {
                PostMessageW(hwnd_, readyMessage_,
                             static_cast<WPARAM>(job.id), 0);
            }
        }
        TrimCache();
    }

    if (comInitialized)
    {
        CoUninitialize();
    }
}

}  // namespace nexus::service

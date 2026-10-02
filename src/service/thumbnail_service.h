#ifndef NEXUS_SRC_SERVICE_THUMBNAIL_SERVICE_H_
#define NEXUS_SRC_SERVICE_THUMBNAIL_SERVICE_H_

#include <d2d1.h>
#include <wrl/client.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "domain/resource.h"

namespace nexus::render {
class Renderer;
}

namespace nexus::service {

// Async thumbnail pipeline (design doc section 52): UI thread only requests
// and reads; worker threads decode via WIC (image) or the Shell thumbnail
// cache (video) and post a message back when ready. Decoded pixels live in
// an LRU cache; D2D bitmaps are created lazily on the UI thread.
class ThumbnailService
{
public:
    enum class State
    {
        kLoading,
        kReady,
        kFailed,
    };

    void Start(HWND hwnd, UINT readyMessage,
               std::filesystem::path cacheRoot = {});

    void Shutdown();

    // Safe to call every frame for visible items; enqueues only once.
    void Request(const Resource& resource);

    State StateOf(ResourceId id);

    // UI thread only. Returns nullptr while loading or on failure.
    ID2D1Bitmap* BitmapFor(ResourceId id, render::Renderer* renderer);

    // Releases render-target-owned bitmaps while retaining decoded pixels.
    // Must be called on the UI thread after a renderer/device recreation.
    void ResetBitmaps();

    // Drops all decoded entries and removes the on-disk thumbnail files. The
    // caller must stop the service first so no worker can recreate a file
    // while the directory is being cleared.
    void ClearCache();

    void Evict(ResourceId id);

private:
    struct Job
    {
        ResourceId id = 0;
        std::filesystem::path path;
        std::filesystem::path cacheFile;
        std::string cacheKey;
        ResourceType type = ResourceType::kImage;
    };

    struct Entry
    {
        State state = State::kLoading;
        std::vector<std::uint8_t> pixels;  // BGRA premultiplied
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::string cacheKey;
        std::filesystem::path cacheFile;
        bool retryWhenAvailable = false;
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;  // UI thread only
        std::uint64_t lastUse = 0;
    };

    void WorkerLoop();

    void Complete(ResourceId id, const std::string& cacheKey,
                  std::vector<std::uint8_t> pixels, std::uint32_t width,
                  std::uint32_t height);

    void TrimCache();

    HWND hwnd_ = nullptr;
    UINT readyMessage_ = 0;
    std::vector<std::thread> workers_;
    std::deque<Job> queue_;
    std::deque<std::filesystem::path> cleanupQueue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::filesystem::path cacheRoot_;
    std::unordered_map<ResourceId, Entry> cache_;
    std::uint64_t useCounter_ = 0;
};

}  // namespace nexus::service

#endif  // NEXUS_SRC_SERVICE_THUMBNAIL_SERVICE_H_

#ifndef NEXUS_SRC_SERVICE_IMPORT_SERVICE_H_
#define NEXUS_SRC_SERVICE_IMPORT_SERVICE_H_

#include <Windows.h>

#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "domain/resource.h"
#include "service/archive_service.h"

namespace nexus::repo {
class ResourceRepository;
class TagRepository;
}

namespace nexus::service {

struct ImportMetadata {
    std::string title;
    std::string category;
    std::vector<std::string> tags;
    std::vector<std::string> actors;
    bool favorite = false;
    bool overrideTitle = false;
    bool deleteSource = false;
};

// Background import pipeline (design doc sections 25, 29, 71): validate ->
// archive (ArchiveService) -> database commit -> recycle source. Runs on a
// worker thread; the UI is notified via a posted message when a batch
// finishes.
class ImportService
{
public:
    struct Progress
    {
        bool active = false;
        std::size_t currentFile = 0;
        std::size_t totalFiles = 0;
        std::uint64_t bytesCopied = 0;
        std::uint64_t totalBytes = 0;
        std::wstring currentPath;
    };

    struct BatchResult
    {
        int imported = 0;
        int skipped = 0;
        int failed = 0;
    };

    ImportService(std::filesystem::path databasePath,
                  ArchiveService& archive);

    ~ImportService();

    void Start(HWND notifyWindow, UINT doneMessage, UINT progressMessage = 0);

    Progress GetProgress() const;

    void Shutdown();

    // Queues files dropped onto the window; invalid types are ignored.
    void Import(std::vector<std::filesystem::path> files,
                ImportMetadata metadata = {});

    // Returns the resource type for a supported extension, or nullopt.
    static std::optional<ResourceType> Classify(
        const std::filesystem::path& path);

private:
    void WorkerLoop();

    BatchResult ImportOne(const std::filesystem::path& file,
                          const ImportMetadata& metadata,
                          repo::ResourceRepository& resources,
                          repo::TagRepository& tags,
                          const ArchiveService::ProgressCallback& progress);

    void SetProgress(const Progress& progress, bool notify);

    std::filesystem::path databasePath_;
    ArchiveService& archive_;
    HWND notifyWindow_ = nullptr;
    UINT doneMessage_ = 0;
    UINT progressMessage_ = 0;
    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    struct Request {
        std::vector<std::filesystem::path> files;
        ImportMetadata metadata;
    };
    std::deque<Request> queue_;
    bool stop_ = false;
    Progress progress_;
    std::chrono::steady_clock::time_point lastProgressPost_ = {};
};

}  // namespace nexus::service

#endif  // NEXUS_SRC_SERVICE_IMPORT_SERVICE_H_

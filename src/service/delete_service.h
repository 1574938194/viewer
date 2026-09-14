#ifndef NEXUS_SRC_SERVICE_DELETE_SERVICE_H_
#define NEXUS_SRC_SERVICE_DELETE_SERVICE_H_

#include <Windows.h>

#include <filesystem>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "domain/resource.h"

namespace nexus::service {

enum class DeleteMode
{
    kRecycle,     // default: move to recycle bin (design doc section 36)
    kPermanent,   // Shift+Delete, requires confirmation by the caller
};

// File-first delete (design doc section 38): the caller only touches the
// database / in-memory metadata after this returns true.
bool DeleteResource(HWND hwnd, const std::filesystem::path& path,
                    DeleteMode mode, std::wstring* error);

struct DeleteItem {
    ResourceId id = 0;
    std::filesystem::path path;
};

struct DeleteBatchResult {
    std::vector<ResourceId> deleted;
    int failed = 0;
};

class DeleteService {
public:
    explicit DeleteService(std::filesystem::path databasePath);
    ~DeleteService();

    void Start(HWND notifyWindow, UINT doneMessage);
    void Shutdown();
    void Delete(std::vector<DeleteItem> items, DeleteMode mode);
    std::vector<DeleteBatchResult> TakeCompleted();

private:
    struct Request {
        std::vector<DeleteItem> items;
        DeleteMode mode = DeleteMode::kRecycle;
    };

    void WorkerLoop();

    std::filesystem::path databasePath_;
    HWND notifyWindow_ = nullptr;
    UINT doneMessage_ = 0;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Request> queue_;
    std::vector<DeleteBatchResult> completed_;
    bool stop_ = false;
};

}  // namespace nexus::service

#endif  // NEXUS_SRC_SERVICE_DELETE_SERVICE_H_

#include "service/delete_service.h"

#include <Windows.h>
#include <shellapi.h>

#include <utility>

#include "database/migration.h"
#include "database/sqlite_database.h"
#include "repository/resource_repository.h"

namespace nexus::service {

bool DeleteResource(HWND hwnd, const std::filesystem::path& path,
                    DeleteMode mode, std::wstring* error)
{
    std::error_code existenceError;
    if (!std::filesystem::exists(path, existenceError) || existenceError)
    {
        if (error)
        {
            *error = existenceError
                ? L"无法访问文件：" + path.wstring()
                : L"文件不存在：" + path.wstring();
        }
        return false;
    }

    std::wstring from = path.wstring();
    from.push_back(L'\0');  // SHFILEOPSTRUCTW wants a double-null list

    SHFILEOPSTRUCTW operation = {};
    operation.hwnd = hwnd;
    operation.wFunc = FO_DELETE;
    operation.pFrom = from.c_str();
    operation.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    if (mode == DeleteMode::kRecycle)
    {
        operation.fFlags |= FOF_ALLOWUNDO;
    }

    int result = SHFileOperationW(&operation);
    if (result != 0 || operation.fAnyOperationsAborted)
    {
        if (error)
        {
            *error = L"删除文件失败（错误码 " + std::to_wstring(result) + L"）";
        }
        return false;
    }
    return true;
}

DeleteService::DeleteService(std::filesystem::path databasePath)
    : databasePath_(std::move(databasePath))
{
}

DeleteService::~DeleteService()
{
    Shutdown();
}

void DeleteService::Start(HWND notifyWindow, UINT doneMessage)
{
    if (worker_.joinable())
    {
        return;
    }
    notifyWindow_ = notifyWindow;
    doneMessage_ = doneMessage;
    stop_ = false;
    worker_ = std::thread([this] { WorkerLoop(); });
}

void DeleteService::Shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable())
    {
        worker_.join();
    }
}

void DeleteService::Delete(std::vector<DeleteItem> items, DeleteMode mode)
{
    if (items.empty())
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back({std::move(items), mode});
    }
    cv_.notify_one();
}

std::vector<DeleteBatchResult> DeleteService::TakeCompleted()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DeleteBatchResult> result = std::move(completed_);
    completed_.clear();
    return result;
}

void DeleteService::WorkerLoop()
{
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInitialized = SUCCEEDED(comResult);
    db::Database database;
    const bool databaseReady =
        database.Open(databasePath_) && db::Migrate(database);
    repo::ResourceRepository resources(database);
    while (true)
    {
        Request request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty())
            {
                break;
            }
            request = std::move(queue_.front());
            queue_.pop_front();
        }
        DeleteBatchResult result;
        for (const DeleteItem& item : request.items)
        {
            if (!databaseReady)
            {
                ++result.failed;
                continue;
            }
            std::error_code existenceError;
            const bool sourceExists =
                std::filesystem::exists(item.path, existenceError);
            const bool fileReady =
                (!sourceExists && !existenceError) ||
                (!existenceError &&
                 DeleteResource(nullptr, item.path, request.mode, nullptr));
            if (fileReady && resources.Delete(item.id))
            {
                resources.LogOperation(
                    request.mode == DeleteMode::kPermanent ? "DELETE"
                                                           : "RECYCLE",
                    item.path, {}, item.id, true);
                result.deleted.push_back(item.id);
            }
            else
            {
                ++result.failed;
                if (databaseReady)
                {
                    resources.LogOperation(
                        request.mode == DeleteMode::kPermanent ? "DELETE"
                                                               : "RECYCLE",
                        item.path, {}, item.id, false);
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            completed_.push_back(std::move(result));
        }
        if (notifyWindow_ != nullptr && IsWindow(notifyWindow_))
        {
            PostMessageW(notifyWindow_, doneMessage_, 0, 0);
        }
    }
    if (comInitialized)
    {
        CoUninitialize();
    }
}

}  // namespace nexus::service

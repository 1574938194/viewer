#include "service/scan_service.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <optional>
#include <string>
#include <thread>

#include "database/migration.h"
#include "database/sqlite_database.h"
#include "repository/resource_repository.h"
#include "util/content_hash.h"
#include "service/media_probe.h"
#include "util/string_util.h"

namespace nexus::service {
namespace {

bool IsExtension(const std::filesystem::path& path,
                const wchar_t* first, const wchar_t* second,
                const wchar_t* third, const wchar_t* fourth,
                const wchar_t* fifth, const wchar_t* sixth,
                const wchar_t* seventh, const wchar_t* eighth)
{
    std::wstring extension = path.extension().wstring();
    for (wchar_t& character : extension)
    {
        if (character >= L'A' && character <= L'Z')
        {
            character += L'a' - L'A';
        }
    }
    const wchar_t* values[] = {first, second, third, fourth,
                               fifth, sixth, seventh, eighth};
    for (const wchar_t* value : values)
    {
        if (extension == value)
        {
            return true;
        }
    }
    return false;
}

std::optional<ResourceType> Classify(const std::filesystem::path& path)
{
    if (IsExtension(path, L".mp4", L".mkv", L".avi", L".wmv", L".mov",
                    L".m4v", L".webm", L".ts"))
    {
        return ResourceType::kVideo;
    }
    if (IsExtension(path, L".jpg", L".jpeg", L".png", L".webp", L".bmp",
                    L"", L"", L""))
    {
        return ResourceType::kImage;
    }
    return std::nullopt;
}

std::int64_t ModifiedTime(const std::filesystem::path& path,
                          std::int64_t fallback)
{
    std::error_code error;
    const auto fileTime = std::filesystem::last_write_time(path, error);
    if (error)
    {
        return fallback;
    }
    const auto systemTime =
        std::chrono::system_clock::now() +
        (fileTime - std::filesystem::file_time_type::clock::now());
    return std::chrono::duration_cast<std::chrono::seconds>(
               systemTime.time_since_epoch())
        .count();
}

}  // namespace

ScanResult ScanService::RunLightweight(
    const std::filesystem::path& databasePath,
    const std::filesystem::path& libraryRoot,
    const std::function<void()>& onFirstCard,
    const std::function<bool()>& shouldStop)
{
    ScanResult result;
    bool firstCardNotified = false;
    db::Database database;
    if (!database.Open(databasePath) || !db::Migrate(database))
    {
        result.failed = 1;
        return result;
    }
    repo::ResourceRepository resources(database);
    std::error_code rootError;
    if (!std::filesystem::exists(libraryRoot, rootError) || rootError)
    {
        if (rootError)
        {
            result.failed = 1;
        }
        return result;
    }

    std::error_code iterationError;
    std::filesystem::recursive_directory_iterator iterator(
        libraryRoot, std::filesystem::directory_options::skip_permission_denied,
        iterationError);
    if (iterationError)
    {
        result.failed = 1;
        return result;
    }
    const std::filesystem::recursive_directory_iterator end;
    for (; iterator != end; iterator.increment(iterationError))
    {
        if (shouldStop && shouldStop())
        {
            break;
        }
        if (iterationError)
        {
            ++result.failed;
            iterationError.clear();
            continue;
        }
        std::error_code entryError;
        if (iterator->is_directory(entryError))
        {
            if (entryError)
            {
                ++result.failed;
            }
            else if (iterator->path().filename() == L".incoming")
            {
                iterator.disable_recursion_pending();
            }
            continue;
        }
        if (entryError)
        {
            ++result.failed;
            continue;
        }
        if (!iterator->is_regular_file(entryError))
        {
            if (entryError)
            {
                ++result.failed;
            }
            continue;
        }
        const auto type = Classify(iterator->path());
        if (!type.has_value())
        {
            ++result.skipped;
            continue;
        }
        const std::filesystem::path path = iterator->path();
        const std::string filename = WideToUtf8(path.filename().wstring());
        if (resources.FindIndexedFile(path).has_value())
        {
            ++result.skipped;
            continue;
        }
        std::error_code sizeError;
        const std::uint64_t fileSize = iterator->file_size(sizeError);
        if (sizeError)
        {
            ++result.failed;
            continue;
        }
        if (resources.FindDuplicate(fileSize, filename).has_value())
        {
            ++result.skipped;
            continue;
        }
        const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        Resource resource;
        resource.type = *type;
        resource.path = path;
        resource.originalFilename = filename;
        resource.title = WideToUtf8(path.stem().wstring());
        resource.fileSize = fileSize;
        resource.addedTime = now;
        resource.modifiedTime = ModifiedTime(path, now);
        if (!database.Exec("BEGIN;") || !resources.Insert(resource) ||
            !database.Exec("COMMIT;"))
        {
            database.Exec("ROLLBACK;");
            ++result.failed;
            continue;
        }
        resources.LogOperation("SCAN_LIGHTWEIGHT", {}, resource.path,
                               resource.id, true);
        ++result.added;
        if (!firstCardNotified && onFirstCard)
        {
            firstCardNotified = true;
            onFirstCard();
        }
    }
    if (iterationError)
    {
        ++result.failed;
    }
    return result;
}

ScanResult ScanService::RunLightweight(
    const std::filesystem::path& databasePath,
    const std::filesystem::path& libraryRoot,
    const std::function<void()>& onFirstCard)
{
    return RunLightweight(databasePath, libraryRoot, onFirstCard, {});
}

ScanResult ScanService::RunIncremental(
    const std::filesystem::path& databasePath,
    const std::filesystem::path& libraryRoot, std::size_t batchSize,
    const std::function<void(const ScanResult&)>& onBatch,
    const std::function<bool()>& shouldStop)
{
    ScanResult result;
    if (batchSize == 0)
    {
        batchSize = 24;
    }

    db::Database database;
    if (!database.Open(databasePath) || !db::Migrate(database))
    {
        result.failed = 1;
        return result;
    }
    repo::ResourceRepository resources(database);
    std::error_code rootError;
    if (!std::filesystem::exists(libraryRoot, rootError) || rootError)
    {
        // An absent managed folder is a valid empty-library state.  There is
        // simply no work to enrich until a folder is selected or restored.
        if (rootError)
        {
            result.failed = 1;
        }
        return result;
    }

    // Clear stale missing flags before selecting work.  A file restored while
    // the application was closed should be enriched during this same startup
    // pass instead of waiting for the next launch.
    resources.RefreshMissing();

    // WIC and the Windows property APIs used by the probes require COM on
    // this worker thread.  The caller may already have COM initialized on a
    // different apartment, so use the multithreaded apartment here.
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInitialized = SUCCEEDED(comResult);
    ResourceId afterId = 0;
    while (true)
    {
        if (shouldStop && shouldStop())
        {
            break;
        }
        const std::vector<Resource> batch =
            resources.LoadUnfilledBatch(batchSize, afterId);
        if (batch.empty())
        {
            break;
        }

        ScanResult page;
        for (const Resource& resource : batch)
        {
            if (shouldStop && shouldStop())
            {
                break;
            }
            afterId = std::max(afterId, resource.id);
            std::error_code fileError;
            if (!std::filesystem::is_regular_file(resource.path, fileError) ||
                fileError)
            {
                if (resources.SetMissing(resource.id, true))
                {
                    ++page.missingUpdated;
                    ++result.missingUpdated;
                }
                continue;
            }

            std::error_code sizeError;
            const std::uint64_t fileSize =
                std::filesystem::file_size(resource.path, sizeError);
            if (sizeError)
            {
                ++page.failed;
                ++result.failed;
                continue;
            }
            const std::int64_t modifiedTime = ModifiedTime(
                resource.path, static_cast<std::int64_t>(std::time(nullptr)));

            bool changed = false;
            bool failed = false;
            // Empty hashes are the primary marker left by the lightweight
            // scan.  Recompute only those rows so completed records are never
            // needlessly read again on subsequent launches.
            if (resource.contentHash.empty())
            {
                const std::optional<std::string> hash =
                    util::ComputeFileSha256(resource.path);
                if (hash.has_value() &&
                    resources.UpdateIndexedFile(resource.id, fileSize,
                                                modifiedTime, *hash))
                {
                    changed = true;
                }
                else
                {
                    failed = true;
                }
            }

            if (resource.type == ResourceType::kImage &&
                (resource.width == 0 || resource.height == 0))
            {
                std::uint32_t width = 0;
                std::uint32_t height = 0;
                if (DetectImageDimensions(resource.path, &width, &height) &&
                    resources.UpdateMediaInfo(resource.id, fileSize, width,
                                              height))
                {
                    changed = true;
                }
                else
                {
                    failed = true;
                }
            }
            else if (resource.type == ResourceType::kVideo &&
                     resource.duration <= 0)
            {
                const std::int64_t duration =
                    DetectMediaDuration(resource.path);
                if (duration > 0 &&
                    resources.UpdateDuration(resource.id, duration))
                {
                    changed = true;
                }
                else
                {
                    // A video without a readable duration remains eligible
                    // for a future startup retry, but should not prevent the
                    // rest of this page from completing.
                    failed = true;
                }
            }

            if (changed)
            {
                ++page.updated;
                ++result.updated;
                resources.LogOperation("CACHE_BUILD", resource.path,
                                       resource.path, resource.id, true);
            }
            if (failed)
            {
                ++page.failed;
                ++result.failed;
            }
        }

        result.added += page.added;
        result.skipped += page.skipped;
        if (onBatch)
        {
            onBatch(page);
        }
        // Yield between pages.  This both keeps CPU/disk contention low for
        // the interactive process and gives the UI/import worker a chance to
        // acquire SQLite's short-lived write lock.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (resources.RefreshMissing())
    {
        ++result.missingUpdated;
    }
    if (comInitialized)
    {
        CoUninitialize();
    }
    return result;
}

ScanResult ScanService::Run(const std::filesystem::path& databasePath,
                            const std::filesystem::path& libraryRoot)
{
    ScanResult result;
    db::Database database;
    if (!database.Open(databasePath) || !db::Migrate(database))
    {
        result.failed = 1;
        return result;
    }
    repo::ResourceRepository resources(database);
    std::error_code rootError;
    const bool rootExists = std::filesystem::exists(libraryRoot, rootError);
    if (rootError)
    {
        result.failed = 1;
        return result;
    }
    if (!rootExists)
    {
        if (resources.RefreshMissing())
        {
            ++result.missingUpdated;
        }
        return result;
    }
    const std::filesystem::path incomingRoot = libraryRoot / L".incoming";
    std::error_code incomingError;
    const bool incomingExists =
        std::filesystem::exists(incomingRoot, incomingError);
    if (incomingError)
    {
        ++result.failed;
    }
    if (incomingExists && !incomingError)
    {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 incomingRoot, std::filesystem::directory_options::skip_permission_denied,
                 incomingError))
        {
            if (incomingError)
            {
                ++result.failed;
                incomingError.clear();
                break;
            }
            std::error_code entryError;
            if (entry.is_regular_file(entryError) && !entryError)
            {
                ++result.incoming;
            }
            else if (entryError)
            {
                ++result.failed;
            }
        }
        if (incomingError)
        {
            ++result.failed;
        }
    }
    std::error_code iterationError;
    std::filesystem::recursive_directory_iterator iterator(
        libraryRoot, std::filesystem::directory_options::skip_permission_denied,
        iterationError);
    if (iterationError)
    {
        result.failed = 1;
        return result;
    }
    const std::filesystem::recursive_directory_iterator end;
    for (; iterator != end; iterator.increment(iterationError))
    {
        if (iterationError)
        {
            ++result.failed;
            iterationError.clear();
            break;
        }
        std::error_code entryError;
        const bool directory = iterator->is_directory(entryError);
        if (entryError)
        {
            ++result.failed;
            continue;
        }
        if (directory)
        {
            if (iterator->path().filename() == L".incoming")
            {
                iterator.disable_recursion_pending();
            }
            continue;
        }
        if (!iterator->is_regular_file(entryError))
        {
            if (entryError)
            {
                ++result.failed;
            }
            continue;
        }
        const auto type = Classify(iterator->path());
        if (!type.has_value())
        {
            ++result.skipped;
            continue;
        }
        const std::filesystem::path path = iterator->path();
        std::error_code error;
        const std::uint64_t fileSize = iterator->file_size(error);
        if (error)
        {
            ++result.failed;
            continue;
        }
        const std::int64_t modifiedTime = ModifiedTime(
            path, static_cast<std::int64_t>(std::time(nullptr)));
        const auto existing = resources.FindIndexedFile(path);
        const bool durationMissing = *type == ResourceType::kVideo &&
                                     existing.has_value() &&
                                     existing->duration <= 0;
        const bool metadataChanged =
            !existing.has_value() || existing->fileSize != fileSize ||
            existing->modifiedTime != modifiedTime ||
            existing->contentHash.empty();
        const std::optional<std::string> contentHash = metadataChanged
            ? util::ComputeFileSha256(path)
            : existing.has_value()
                ? std::optional<std::string>(existing->contentHash)
                : std::nullopt;
        if (existing.has_value())
        {
            if (!metadataChanged && !durationMissing)
            {
                ++result.skipped;
                continue;
            }
            bool updated = true;
            if (metadataChanged)
            {
                updated = contentHash.has_value() &&
                          resources.UpdateIndexedFile(existing->id, fileSize,
                                                     modifiedTime,
                                                     *contentHash);
            }
            if (updated && durationMissing)
            {
                const std::int64_t duration = DetectMediaDuration(path);
                if (duration > 0)
                {
                    updated = resources.UpdateDuration(existing->id, duration);
                }
            }
            if (!updated)
            {
                ++result.failed;
            }
            else
            {
                resources.LogOperation("SCAN_UPDATE", path, path,
                                       existing->id, true);
                ++result.updated;
            }
            continue;
        }
        Resource resource;
        resource.type = *type;
        resource.path = path;
        resource.originalFilename =
            WideToUtf8(path.filename().wstring());
        resource.title = WideToUtf8(path.stem().wstring());
        resource.fileSize = fileSize;
        resource.contentHash = contentHash.value_or(std::string{});
        if (*type == ResourceType::kVideo)
        {
            resource.duration = DetectMediaDuration(path);
        }
        if (const auto duplicate = resources.FindDuplicate(
                resource.fileSize, resource.originalFilename,
                resource.contentHash);
            duplicate.has_value())
        {
            ++result.skipped;
            continue;
        }
        resource.addedTime = static_cast<std::int64_t>(std::time(nullptr));
        resource.modifiedTime = modifiedTime;
        if (database.Exec("BEGIN;") && resources.Insert(resource) &&
            database.Exec("COMMIT;"))
        {
            resources.LogOperation("SCAN", {}, resource.path, resource.id,
                                   true);
            ++result.added;
        }
        else
        {
            database.Exec("ROLLBACK;");
            ++result.failed;
        }
    }
    if (iterationError)
    {
        ++result.failed;
    }
    if (resources.RefreshMissing())
    {
        ++result.missingUpdated;
    }
    return result;
}

}  // namespace nexus::service

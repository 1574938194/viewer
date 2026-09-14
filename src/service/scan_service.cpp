#include "service/scan_service.h"

#include <Windows.h>

#include <chrono>
#include <ctime>
#include <optional>
#include <string>

#include "database/migration.h"
#include "database/sqlite_database.h"
#include "repository/resource_repository.h"
#include "util/content_hash.h"
#include "util/string_util.h"

namespace nexus::service {
namespace {

bool IsExtension(const std::filesystem::path& path,
                const wchar_t* first, const wchar_t* second,
                const wchar_t* third, const wchar_t* fourth,
                const wchar_t* fifth, const wchar_t* sixth,
                const wchar_t* seventh)
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
                               fifth, sixth, seventh};
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
                    L".m4v", L".webm"))
    {
        return ResourceType::kVideo;
    }
    if (IsExtension(path, L".jpg", L".jpeg", L".png", L".webp", L".bmp",
                    L"", L""))
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
        const bool metadataChanged =
            !existing.has_value() || existing->fileSize != fileSize ||
            existing->modifiedTime != modifiedTime ||
            existing->contentHash.empty();
        const std::optional<std::string> contentHash = metadataChanged
            ? util::ComputeFileSha256(path)
            : std::optional<std::string>(existing->contentHash);
        if (existing.has_value())
        {
            if (!metadataChanged)
            {
                ++result.skipped;
                continue;
            }
            if (!contentHash.has_value() ||
                !resources.UpdateIndexedFile(existing->id, fileSize,
                                             modifiedTime, *contentHash))
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

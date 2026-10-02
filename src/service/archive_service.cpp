#include "service/archive_service.h"

#include <Windows.h>

#include <cstdio>
#include <random>
#include <system_error>

#include "service/delete_service.h"

namespace nexus::service {

namespace {

struct CopyProgressContext
{
    const ArchiveService::ProgressCallback* callback = nullptr;
};

DWORD CALLBACK CopyProgressRoutine(
    LARGE_INTEGER totalFileSize, LARGE_INTEGER totalBytesTransferred,
    LARGE_INTEGER /*streamSize*/, LARGE_INTEGER /*streamBytesTransferred*/,
    DWORD /*streamNumber*/, DWORD /*callbackReason*/, HANDLE /*sourceFile*/,
    HANDLE /*destinationFile*/, LPVOID data)
{
    auto* context = static_cast<CopyProgressContext*>(data);
    if (context != nullptr && context->callback != nullptr &&
        *context->callback)
    {
        (*context->callback)(
            static_cast<std::uint64_t>(totalBytesTransferred.QuadPart),
            static_cast<std::uint64_t>(totalFileSize.QuadPart));
    }
    return PROGRESS_CONTINUE;
}

bool CopyFileWithProgress(const std::filesystem::path& source,
                          const std::filesystem::path& target,
                          const ArchiveService::ProgressCallback& progress,
                          std::error_code* error)
{
    CopyProgressContext context = {&progress};
    if (CopyFileExW(source.c_str(), target.c_str(), CopyProgressRoutine,
                    &context, nullptr, COPY_FILE_FAIL_IF_EXISTS))
    {
        return true;
    }
    if (error != nullptr)
    {
        *error = std::error_code(static_cast<int>(GetLastError()),
                                 std::system_category());
    }
    return false;
}

std::wstring Lower(std::wstring text)
{
    for (wchar_t& c : text)
    {
        if (c >= L'A' && c <= L'Z')
        {
            c += L'a' - L'A';
        }
    }
    return text;
}

std::filesystem::path ManagedPathForRoot(
    const std::filesystem::path& root, ResourceId id, ResourceType type,
    const std::wstring& extension)
{
    const wchar_t* folder = type == ResourceType::kVideo ? L"video" : L"image";
    const unsigned bucket = static_cast<unsigned>((id / 100) % 100);
    wchar_t bucketName[8] = {};
    swprintf(bucketName, 8, L"%02u", bucket);

    wchar_t fileName[24] = {};
    swprintf(fileName, 24, L"%012llu",
             static_cast<unsigned long long>(id));
    return root / folder / bucketName / (fileName + extension);
}

std::filesystem::path ManagedPathForFilename(
    const std::filesystem::path& root, ResourceId id, ResourceType type,
    const std::filesystem::path& filename)
{
    const wchar_t* folder = type == ResourceType::kVideo ? L"video" : L"image";
    const unsigned bucket = static_cast<unsigned>((id / 100) % 100);
    wchar_t bucketName[8] = {};
    swprintf(bucketName, 8, L"%02u", bucket);
    return root / folder / bucketName / filename.filename();
}

}  // namespace

ArchiveService::ArchiveService(std::filesystem::path libraryRoot)
    : libraryRoot_(std::move(libraryRoot))
{
}

bool ArchiveService::SameVolume(const std::filesystem::path& a,
                                const std::filesystem::path& b)
{
    return Lower(a.root_name().wstring()) == Lower(b.root_name().wstring());
}

bool ArchiveService::EnsureDirectories(
    ResourceType type, const std::filesystem::path& libraryRoot)
{
    const wchar_t* folder = type == ResourceType::kVideo ? L"video" : L"image";
    std::error_code ec;
    std::filesystem::create_directories(libraryRoot / folder, ec);
    if (ec)
    {
        return false;
    }
    ec.clear();
    std::filesystem::create_directories(libraryRoot / L".incoming", ec);
    return !ec;
}

std::filesystem::path ArchiveService::ManagedPath(
    ResourceId id, ResourceType type, const std::wstring& extension) const
{
    return ManagedPathForRoot(LibraryRootSnapshot(), id, type, extension);
}

ArchiveResult ArchiveService::Archive(const std::filesystem::path& source,
                                      ResourceId id, ResourceType type,
                                      bool deleteSource,
                                      ProgressCallback progress)
{
    ArchiveResult result;
    const std::filesystem::path libraryRoot = LibraryRootSnapshot();
    if (source.empty() || libraryRoot.empty())
    {
        result.error = L"归档路径为空。";
        return result;
    }
    if (!EnsureDirectories(type, libraryRoot))
    {
        result.error = L"无法创建库目录：" + libraryRoot.wstring();
        return result;
    }

    // Keep the user's original filename in managed storage.  The type and
    // deterministic id bucket still provide organization without rewriting
    // names into opaque numeric identifiers.
    const std::filesystem::path target = ManagedPathForFilename(
        libraryRoot, id, type, source.filename());
    std::error_code ec;
    if (std::filesystem::exists(target, ec) || ec)
    {
        result.error = ec ? L"无法访问归档目标：" + target.wstring()
                          : L"目标已存在：" + target.wstring();
        return result;
    }

    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec)
    {
        result.error = L"无法创建归档目录：" + target.parent_path().wstring();
        return result;
    }
    if (SameVolume(source, target))
    {
        // Same volume: rename is a metadata-only move (section 30).
        std::filesystem::rename(source, target, ec);
        if (ec)
        {
            // Fall back to copy+verify in case rename is refused (e.g. some
            // network shares), keeping the safety guarantees.
            if (!CopyFileWithProgress(source, target, progress, &ec))
            {
                result.error = L"归档失败：" + source.wstring() + L"（" +
                               std::to_wstring(ec.value()) + L"）";
                std::filesystem::remove(target, ec);
                return result;
            }
            const auto sourceSize = std::filesystem::file_size(source, ec);
            const bool sourceSizeOk = !ec;
            const auto copiedSize = std::filesystem::file_size(target, ec);
            if (ec || !sourceSizeOk || sourceSize != copiedSize)
            {
                std::filesystem::remove(target, ec);
                result.error = L"归档失败（复制校验大小不一致）：" +
                               source.wstring();
                return result;
            }
            if (!deleteSource ||
                !DeleteResource(nullptr, source, DeleteMode::kRecycle, nullptr))
            {
                // Keep the archive; warn but do not fail the import
                // (section 33: keep data, surface a warning).
                result.ok = true;
                result.finalPath = target;
                return result;
            }
        }
        result.ok = true;
        result.finalPath = target;
        return result;
    }

    // Cross volume: copy to .incoming, verify, then atomic rename into place
    // (sections 31-32). The source is only removed afterwards.
    std::random_device device;
    wchar_t tempName[40] = {};
    swprintf(tempName, 40, L"%012llu-%08x.tmp",
             static_cast<unsigned long long>(id), device());
    const std::filesystem::path incoming =
        libraryRoot_ / L".incoming" / tempName;

    if (!CopyFileWithProgress(source, incoming, progress, &ec))
    {
        std::filesystem::remove(incoming, ec);
        result.error = L"复制失败：" + source.wstring() + L"（" +
                       std::to_wstring(ec.value()) + L"）";
        return result;
    }

    // Verify by size; the import worker computes and persists a SHA-256
    // content hash after the archive has been verified.
    const auto sourceSize = std::filesystem::file_size(source, ec);
    const bool sourceSizeOk = !ec;
    const auto copiedSize = std::filesystem::file_size(incoming, ec);
    if (ec || !sourceSizeOk || sourceSize != copiedSize)
    {
        std::filesystem::remove(incoming, ec);
        result.error = L"校验失败（大小不一致）：" + source.wstring();
        return result;
    }

    std::filesystem::rename(incoming, target, ec);
    if (ec)
    {
        std::filesystem::remove(incoming, ec);
        result.error = L"落盘失败：" + target.wstring();
        return result;
    }

    if (deleteSource)
    {
        // Recycling (not permanent delete) keeps the safety net.
        if (!DeleteResource(nullptr, source, DeleteMode::kRecycle, nullptr))
        {
            // Target verified: keep the archive and report success; the
            // duplicate source is the acceptable outcome (section 33).
            result.ok = true;
            result.finalPath = target;
            return result;
        }
    }

    result.ok = true;
    result.finalPath = target;
    return result;
}

bool ArchiveService::Rollback(const std::filesystem::path& source,
                              const ArchiveResult& archive)
{
    if (!archive.ok || archive.finalPath.empty())
    {
        return true;
    }

    std::error_code ec;
    if (!std::filesystem::exists(archive.finalPath, ec))
    {
        return !ec;
    }
    if (!std::filesystem::exists(source, ec))
    {
        const std::filesystem::path parent = source.parent_path();
        if (!parent.empty())
        {
            std::filesystem::create_directories(parent, ec);
            if (ec)
            {
                return false;
            }
        }
        ec.clear();
        std::filesystem::rename(archive.finalPath, source, ec);
        return !ec;
    }
    std::filesystem::remove(archive.finalPath, ec);
    return !ec;
}

}  // namespace nexus::service

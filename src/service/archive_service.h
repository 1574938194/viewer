#ifndef NEXUS_SRC_SERVICE_ARCHIVE_SERVICE_H_
#define NEXUS_SRC_SERVICE_ARCHIVE_SERVICE_H_

#include <filesystem>
#include <functional>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

#include "domain/resource.h"

namespace nexus::service {

struct ArchiveResult
{
    bool ok = false;
    std::filesystem::path finalPath;
    std::wstring error;
};

// Moves external files into the managed library (design doc sections 9-11,
// 30-34). Same-volume archives use a filesystem rename; cross-volume copies
// go through .incoming and are verified before the source is removed. On any
// failure the source is left untouched (section 33).
class ArchiveService
{
public:
    using ProgressCallback =
        std::function<void(std::uint64_t copied, std::uint64_t total)>;
    explicit ArchiveService(std::filesystem::path libraryRoot);

    // |deleteSource| only applies to cross-volume copies; callers that also
    // write metadata should pass false and delete the source after COMMIT.
    ArchiveResult Archive(const std::filesystem::path& source, ResourceId id,
                          ResourceType type, bool deleteSource,
                          ProgressCallback progress = {});

    // Reverses an archive whose database transaction did not commit. A
    // same-volume move is restored to its original path; a cross-volume copy
    // is removed while its source remains intact.
    bool Rollback(const std::filesystem::path& source,
                  const ArchiveResult& archive);

    std::filesystem::path library_root() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return libraryRoot_;
    }

    void SetLibraryRoot(std::filesystem::path libraryRoot)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        libraryRoot_ = std::move(libraryRoot);
    }

    std::filesystem::path ManagedPath(ResourceId id, ResourceType type,
                                      const std::wstring& extension) const;

private:
    bool EnsureDirectories(ResourceType type,
                           const std::filesystem::path& libraryRoot);

    static bool SameVolume(const std::filesystem::path& a,
                           const std::filesystem::path& b);

    std::filesystem::path LibraryRootSnapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return libraryRoot_;
    }

    std::filesystem::path libraryRoot_;
    mutable std::mutex mutex_;
};

}  // namespace nexus::service

#endif  // NEXUS_SRC_SERVICE_ARCHIVE_SERVICE_H_

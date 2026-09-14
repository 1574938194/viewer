#ifndef NEXUS_SRC_REPOSITORY_RESOURCE_REPOSITORY_H_
#define NEXUS_SRC_REPOSITORY_RESOURCE_REPOSITORY_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <optional>
#include <vector>

#include "database/sqlite_database.h"
#include "domain/resource.h"

namespace nexus::repo {

// SQLite-backed persistence for resources, their tags and open history
// (design doc sections 21-23, 47).
class ResourceRepository
{
public:
    explicit ResourceRepository(db::Database& database);

    // Loads all resources with their tags joined in.
    std::vector<Resource> LoadAll();

    // Inserts the resource and fills resource.id. The caller is expected to
    // run inside a transaction and UpdatePath() after archiving.
    bool Insert(Resource& resource);

    bool UpdatePath(ResourceId id, const std::filesystem::path& path);

    bool UpdateMediaInfo(ResourceId id, std::uint64_t fileSize,
                         std::uint32_t width, std::uint32_t height);

    bool SetMissing(ResourceId id, bool missing);

    bool SetFavorite(ResourceId id, bool favorite);

    bool UpdateMetadata(ResourceId id, const std::string& title,
                        bool favorite);

    bool RefreshMissing();

    // Deletes the row; tag/actor/history relations cascade.
    bool Delete(ResourceId id);

    bool ExistsPath(const std::filesystem::path& path);

    struct IndexedFileState
    {
        ResourceId id = 0;
        std::uint64_t fileSize = 0;
        std::int64_t modifiedTime = 0;
        std::string contentHash;
    };

    std::optional<IndexedFileState> FindIndexedFile(
        const std::filesystem::path& path);

    bool UpdateIndexedFile(ResourceId id, std::uint64_t fileSize,
                           std::int64_t modifiedTime,
                           const std::string& contentHash);

    std::optional<ResourceId> FindDuplicate(std::uint64_t fileSize,
                                            const std::string& filename,
                                            const std::string& contentHash = {});

    // Returns resource ids ranked by the mandatory SQLite FTS5 index. The
    // match-all expression "*" is used for filter-only queries.
    std::vector<ResourceId> SearchFts(const std::string& query);

    std::uint64_t Count();

    // Records an open event and refreshes the cached statistics + hot score
    // (design doc sections 45, 47, 49).
    bool NoteOpen(ResourceId id);

    bool LoadOpenStats(ResourceId id, std::int64_t* lastOpenTime,
                       std::uint32_t* totalOpenCount, double* hotScore);

    bool LogOperation(const std::string& operation,
                      const std::filesystem::path& sourcePath,
                      const std::filesystem::path& targetPath, ResourceId id,
                      bool success);

    db::Database& database() { return database_; }

private:
    db::Database& database_;
};

}  // namespace nexus::repo

#endif  // NEXUS_SRC_REPOSITORY_RESOURCE_REPOSITORY_H_

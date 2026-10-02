#include "repository/resource_repository.h"

#include <Windows.h>

#include <cmath>
#include <map>
#include <time.h>

#include "util/string_util.h"

namespace nexus::repo {

namespace {

std::string PathKey(const std::filesystem::path& path)
{
    return WideToUtf8(path.wstring());
}

// Keep the scalar projection used by LoadAll and the bounded background
// loaders identical.  ReadResource relies on these column positions.
constexpr const char* kResourceColumns =
    "r.id, r.resource_type, r.path,"
    " r.original_filename, r.title, r.code,"
    " r.category_id, r.file_size, r.width, r.height,"
    " r.duration, r.content_hash, r.added_time,"
    " r.modified_time, r.last_open_time, r.total_open_count, r.hot_score,"
    " r.favorite, r.thumbnail_key, r.missing, c.name";

Resource ReadResource(db::Statement& statement);

std::vector<Resource> LoadResourceBatch(db::Database& database,
                                        const char* condition,
                                        std::size_t limit,
                                        ResourceId afterId)
{
    std::vector<Resource> items;
    if (limit == 0 || condition == nullptr)
    {
        return items;
    }
    const std::string sql =
        std::string("SELECT ") + kResourceColumns +
        " FROM resources r LEFT JOIN categories c ON c.id=r.category_id " +
        condition + " ORDER BY r.id ASC LIMIT ?;";
    db::Statement statement;
    if (!statement.Prepare(database.handle(), sql.c_str()))
    {
        return items;
    }
    statement.BindInt(1, static_cast<std::int64_t>(afterId));
    statement.BindInt(2, static_cast<std::int64_t>(limit));
    while (statement.StepRow())
    {
        items.push_back(ReadResource(statement));
    }
    return items;
}

Resource ReadResource(db::Statement& statement)
{
    Resource resource;
    resource.id = static_cast<ResourceId>(statement.ColumnInt(0));
    const int type = statement.ColumnInt(1);
    resource.type = type == 0 ? ResourceType::kVideo
                              : type == 1 ? ResourceType::kImage
                                          : ResourceType::kFolder;
    resource.path = std::filesystem::path(
        Utf8ToWide(statement.ColumnText(2)));
    resource.originalFilename = statement.ColumnText(3);
    resource.title = statement.ColumnText(4);
    resource.code = statement.ColumnText(5);
    if (statement.ColumnInt(6) != 0)
    {
        resource.categoryId = statement.ColumnInt(6);
    }
    resource.categoryName = statement.ColumnText(20);
    resource.fileSize = static_cast<std::uint64_t>(statement.ColumnInt(7));
    resource.width = static_cast<std::uint32_t>(statement.ColumnInt(8));
    resource.height = static_cast<std::uint32_t>(statement.ColumnInt(9));
    resource.duration = statement.ColumnInt(10);
    resource.contentHash = statement.ColumnText(11);
    resource.addedTime = statement.ColumnInt(12);
    resource.modifiedTime = statement.ColumnInt(13);
    resource.lastOpenTime = statement.ColumnInt(14);
    resource.totalOpenCount =
        static_cast<std::uint32_t>(statement.ColumnInt(15));
    resource.hotScore = statement.ColumnDouble(16);
    resource.favorite = statement.ColumnInt(17) != 0;
    resource.missing = statement.ColumnInt(19) != 0;
    return resource;
}

}  // namespace

ResourceRepository::ResourceRepository(db::Database& database)
    : database_(database)
{
}

std::vector<Resource> ResourceRepository::LoadAll()
{
    std::vector<Resource> items;

    db::Statement statement;
    if (!statement.Prepare(database_.handle(),
                           "SELECT r.id, r.resource_type, r.path,"
                           " r.original_filename, r.title, r.code,"
                           " r.category_id, r.file_size, r.width, r.height,"
                           " r.duration, r.content_hash, r.added_time,"
                           " r.modified_time,"
                           " r.last_open_time, r.total_open_count, r.hot_score,"
                           " r.favorite, r.thumbnail_key, r.missing"
                           ", c.name FROM resources r"
                           " LEFT JOIN categories c ON c.id=r.category_id"
                           " ORDER BY added_time DESC, r.id DESC;"))
    {
        return items;
    }
    while (statement.StepRow())
    {
        items.push_back(ReadResource(statement));
    }

    // Attach tags in one pass instead of a query per resource.
    db::Statement tagStatement;
    if (tagStatement.Prepare(database_.handle(),
                             "SELECT rt.resource_id, t.name FROM resource_tags rt"
                             " JOIN tags t ON t.id = rt.tag_id;"))
    {
        std::map<ResourceId, std::vector<std::string>> byResource;
        while (tagStatement.StepRow())
        {
            byResource[static_cast<ResourceId>(tagStatement.ColumnInt(0))]
                .push_back(tagStatement.ColumnText(1));
        }
        for (Resource& item : items)
        {
            auto it = byResource.find(item.id);
            if (it != byResource.end())
            {
                item.tags = std::move(it->second);
            }
        }
    }
    db::Statement actorStatement;
    if (actorStatement.Prepare(
            database_.handle(),
            "SELECT ra.resource_id, a.name FROM resource_actors ra"
            " JOIN actors a ON a.id=ra.actor_id;"))
    {
        std::map<ResourceId, std::vector<std::string>> byResource;
        while (actorStatement.StepRow())
        {
            byResource[static_cast<ResourceId>(actorStatement.ColumnInt(0))]
                .push_back(actorStatement.ColumnText(1));
        }
        for (Resource& item : items)
        {
            auto it = byResource.find(item.id);
            if (it != byResource.end())
            {
                item.actors = std::move(it->second);
            }
        }
    }
    return items;
}

std::vector<Resource> ResourceRepository::LoadMediaBatch(std::size_t limit,
                                                         ResourceId afterId)
{
    return LoadResourceBatch(
        database_,
        " WHERE r.resource_type IN (0,1) AND r.id > ?",
        limit, afterId);
}

std::vector<Resource> ResourceRepository::LoadUnfilledBatch(
    std::size_t limit, ResourceId afterId)
{
    // A missing file is deliberately left for RefreshMissing()/the periodic
    // missing-file worker.  Once it reappears, the next startup pass will
    // pick it up again because the derived fields are still empty.
    return LoadResourceBatch(
        database_,
        " WHERE r.resource_type IN (0,1) AND r.id > ?"
        " AND COALESCE(r.missing,0)=0"
        " AND (COALESCE(r.content_hash,'')=''"
        " OR (r.resource_type=1 AND (COALESCE(r.width,0)<=0 OR"
        " COALESCE(r.height,0)<=0))"
        " OR (r.resource_type=0 AND COALESCE(r.duration,0)<=0))",
        limit, afterId);
}

bool ResourceRepository::Insert(Resource& resource)
{
    db::Statement statement;
    if (!statement.Prepare(database_.handle(),
                           "INSERT INTO resources(resource_type, path,"
                           " original_filename, title, code, file_size, width,"
                           " height, duration, content_hash, added_time,"
                           " modified_time, favorite)"
                           " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?);"))
    {
        return false;
    }
    statement.BindInt(0 + 1, resource.type == ResourceType::kVideo
                                ? 0
                                : resource.type == ResourceType::kImage ? 1 : 2);
    statement.BindText(1 + 1, PathKey(resource.path));
    statement.BindText(2 + 1, resource.originalFilename);
    statement.BindText(3 + 1, resource.title);
    statement.BindText(4 + 1, resource.code);
    statement.BindInt(5 + 1, static_cast<std::int64_t>(resource.fileSize));
    statement.BindInt(6 + 1, resource.width);
    statement.BindInt(7 + 1, resource.height);
    statement.BindInt(8 + 1, resource.duration);
    statement.BindText(9 + 1, resource.contentHash);
    statement.BindInt(10 + 1, resource.addedTime);
    statement.BindInt(11 + 1, resource.modifiedTime);
    statement.BindInt(12 + 1, resource.favorite ? 1 : 0);
    if (!statement.StepDone())
    {
        return false;
    }
    resource.id = static_cast<ResourceId>(
        sqlite3_last_insert_rowid(database_.handle()));
    return true;
}

bool ResourceRepository::UpdatePath(ResourceId id,
                                    const std::filesystem::path& path)
{
    db::Statement statement;
    if (!statement.Prepare(database_.handle(),
                           "UPDATE resources SET path = ? WHERE id = ?;"))
    {
        return false;
    }
    statement.BindText(1, PathKey(path));
    statement.BindInt(2, static_cast<std::int64_t>(id));
    return statement.StepDone();
}

bool ResourceRepository::UpdateMediaInfo(ResourceId id,
                                         std::uint64_t fileSize,
                                         std::uint32_t width,
                                         std::uint32_t height)
{
    db::Statement statement;
    if (!statement.Prepare(database_.handle(),
                           "UPDATE resources SET file_size = ?, width = ?,"
                           " height = ? WHERE id = ?;"))
    {
        return false;
    }
    statement.BindInt(1, static_cast<std::int64_t>(fileSize));
    statement.BindInt(2, width);
    statement.BindInt(3, height);
    statement.BindInt(4, static_cast<std::int64_t>(id));
    return statement.StepDone();
}

bool ResourceRepository::SetMissing(ResourceId id, bool missing)
{
    db::Statement statement;
    if (!statement.Prepare(database_.handle(),
                           "UPDATE resources SET missing = ? WHERE id = ?;"))
    {
        return false;
    }
    statement.BindInt(1, missing ? 1 : 0);
    statement.BindInt(2, static_cast<std::int64_t>(id));
    return statement.StepDone();
}

bool ResourceRepository::SetFavorite(ResourceId id, bool favorite)
{
    db::Statement statement;
    if (!statement.Prepare(database_.handle(),
                           "UPDATE resources SET favorite = ? WHERE id = ?;"))
    {
        return false;
    }
    statement.BindInt(1, favorite ? 1 : 0);
    statement.BindInt(2, static_cast<std::int64_t>(id));
    return statement.StepDone();
}

bool ResourceRepository::UpdateMetadata(ResourceId id,
                                        const std::string& title,
                                        bool favorite)
{
    db::Statement statement;
    if (!statement.Prepare(
            database_.handle(),
            "UPDATE resources SET title=?, favorite=? WHERE id=?;"))
    {
        return false;
    }
    statement.BindText(1, title);
    statement.BindInt(2, favorite ? 1 : 0);
    statement.BindInt(3, static_cast<std::int64_t>(id));
    return statement.StepDone();
}

bool ResourceRepository::RefreshMissing()
{
    struct MissingState
    {
        ResourceId id;
        std::filesystem::path path;
        bool missing;
    };
    std::vector<MissingState> states;
    db::Statement select;
    if (!select.Prepare(database_.handle(),
                        "SELECT id, path, missing FROM resources;"))
    {
        return false;
    }
    while (select.StepRow())
    {
        states.push_back({static_cast<ResourceId>(select.ColumnInt(0)),
                          std::filesystem::path(Utf8ToWide(select.ColumnText(1))),
                          select.ColumnInt(2) != 0});
    }

    bool changed = false;
    for (const MissingState& state : states)
    {
        std::error_code error;
        const bool missing = !std::filesystem::exists(state.path, error) ||
                             static_cast<bool>(error);
        if (missing == state.missing)
        {
            continue;
        }
        db::Statement update;
        if (!update.Prepare(database_.handle(),
                            "UPDATE resources SET missing=? WHERE id=?;") ||
            (update.BindInt(1, missing ? 1 : 0),
             update.BindInt(2, static_cast<std::int64_t>(state.id)),
             !update.StepDone()))
        {
            return false;
        }
        changed = true;
    }
    return changed;
}

bool ResourceRepository::Delete(ResourceId id)
{
    if (!database_.Exec("BEGIN;"))
    {
        return false;
    }
    db::Statement statement;
    if (!statement.Prepare(database_.handle(),
                           "DELETE FROM resources WHERE id = ?;"))
    {
        database_.Exec("ROLLBACK;");
        return false;
    }
    statement.BindInt(1, static_cast<std::int64_t>(id));
    if (!statement.StepDone() || sqlite3_changes(database_.handle()) != 1 ||
        !database_.Exec("COMMIT;"))
    {
        database_.Exec("ROLLBACK;");
        return false;
    }
    return true;
}

bool ResourceRepository::ExistsPath(const std::filesystem::path& path)
{
    return FindIndexedFile(path).has_value();
}

std::optional<ResourceRepository::IndexedFileState>
ResourceRepository::FindIndexedFile(const std::filesystem::path& path)
{
    db::Statement statement;
    if (!statement.Prepare(
            database_.handle(),
            "SELECT id, file_size, modified_time, content_hash, duration FROM "
            "resources WHERE path = ? COLLATE NOCASE LIMIT 1;"))
    {
        return std::nullopt;
    }
    statement.BindText(1, PathKey(path));
    if (!statement.StepRow())
    {
        return std::nullopt;
    }
    return IndexedFileState{
        static_cast<ResourceId>(statement.ColumnInt(0)),
        static_cast<std::uint64_t>(statement.ColumnInt(1)),
        statement.ColumnInt(2), statement.ColumnText(3), statement.ColumnInt(4)};
}

bool ResourceRepository::UpdateIndexedFile(
    ResourceId id, std::uint64_t fileSize, std::int64_t modifiedTime,
    const std::string& contentHash)
{
    db::Statement statement;
    if (!statement.Prepare(
            database_.handle(),
            "UPDATE resources SET file_size=?, modified_time=?, "
            "content_hash=?, missing=0 WHERE id=?;"))
    {
        return false;
    }
    statement.BindInt(1, static_cast<std::int64_t>(fileSize));
    statement.BindInt(2, modifiedTime);
    statement.BindText(3, contentHash);
    statement.BindInt(4, static_cast<std::int64_t>(id));
    return statement.StepDone();
}

bool ResourceRepository::UpdateDuration(ResourceId id, std::int64_t duration)
{
    db::Statement statement;
    if (!statement.Prepare(database_.handle(),
                           "UPDATE resources SET duration=? WHERE id=?;"))
    {
        return false;
    }
    statement.BindInt(1, duration);
    statement.BindInt(2, static_cast<std::int64_t>(id));
    return statement.StepDone();
}

std::optional<ResourceId> ResourceRepository::FindDuplicate(
    std::uint64_t fileSize, const std::string& filename,
    const std::string& contentHash)
{
    db::Statement statement;
    const char* sql = contentHash.empty()
        ? "SELECT id FROM resources WHERE file_size=? AND "
          "original_filename=? COLLATE NOCASE LIMIT 1;"
        : "SELECT id FROM resources WHERE file_size=? AND "
          "((content_hash<>'' AND content_hash=? COLLATE NOCASE) OR "
          "original_filename=? COLLATE NOCASE) LIMIT 1;";
    if (!statement.Prepare(database_.handle(), sql))
    {
        return std::nullopt;
    }
    statement.BindInt(1, static_cast<std::int64_t>(fileSize));
    if (contentHash.empty())
    {
        statement.BindText(2, filename);
    }
    else
    {
        statement.BindText(2, contentHash);
        statement.BindText(3, filename);
    }
    if (!statement.StepRow())
    {
        return std::nullopt;
    }
    return static_cast<ResourceId>(statement.ColumnInt(0));
}

std::vector<ResourceId> ResourceRepository::SearchFts(
    const std::string& query)
{
    std::vector<ResourceId> ids;
    const std::string match = query.empty() ? "*" : query;
    db::Statement statement;
    const char* sql = match == "*"
        ? "SELECT rowid FROM resource_fts ORDER BY rowid;"
        : "SELECT rowid FROM resource_fts WHERE resource_fts MATCH ? "
          "ORDER BY rank;";
    if (!statement.Prepare(database_.handle(), sql))
    {
        return ids;
    }
    if (match != "*")
    {
        statement.BindText(1, match);
    }
    while (statement.StepRow())
    {
        ids.push_back(static_cast<ResourceId>(statement.ColumnInt(0)));
    }
    return ids;
}

std::uint64_t ResourceRepository::Count()
{
    db::Statement statement;
    if (!statement.Prepare(database_.handle(),
                           "SELECT COUNT(*) FROM resources;"))
    {
        return 0;
    }
    if (!statement.StepRow())
    {
        return 0;
    }
    return static_cast<std::uint64_t>(statement.ColumnInt(0));
}

bool ResourceRepository::NoteOpen(ResourceId id)
{
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));

    if (!database_.Exec("BEGIN;"))
    {
        return false;
    }
    auto rollback = [this]() {
        database_.Exec("ROLLBACK;");
        return false;
    };

    db::Statement insert;
    if (!insert.Prepare(database_.handle(),
                        "INSERT INTO open_history(resource_id, open_time)"
                        " VALUES(?, ?);"))
    {
        return rollback();
    }
    insert.BindInt(1, static_cast<std::int64_t>(id));
    insert.BindInt(2, now);
    if (!insert.StepDone())
    {
        return rollback();
    }

    db::Statement counts;
    if (!counts.Prepare(database_.handle(),
                        "SELECT"
                        " SUM(CASE WHEN open_time > ? THEN 1 ELSE 0 END),"
                        " SUM(CASE WHEN open_time > ? THEN 1 ELSE 0 END),"
                        " SUM(CASE WHEN open_time > ? THEN 1 ELSE 0 END),"
                        " COUNT(*)"
                        " FROM open_history WHERE resource_id = ?;"))
    {
        return rollback();
    }
    counts.BindInt(1, now - 86400);
    counts.BindInt(2, now - 7 * 86400);
    counts.BindInt(3, now - 30 * 86400);
    counts.BindInt(4, static_cast<std::int64_t>(id));
    if (!counts.StepRow())
    {
        return rollback();
    }
    const double opens1d = static_cast<double>(counts.ColumnInt(0));
    const double opens7d = static_cast<double>(counts.ColumnInt(1));
    const double opens30d = static_cast<double>(counts.ColumnInt(2));
    const double total = static_cast<double>(counts.ColumnInt(3));

    // HotScore = opens1d*10 + opens7d*5 + opens30d*2 + log(1 + total)
    // (design doc section 45).
    const double hotScore =
        opens1d * 10.0 + opens7d * 5.0 + opens30d * 2.0 + std::log1p(total);

    db::Statement update;
    if (!update.Prepare(database_.handle(),
                        "UPDATE resources SET last_open_time = ?,"
                        " total_open_count = ?, hot_score = ? WHERE id = ?;"))
    {
        return rollback();
    }
    update.BindInt(1, now);
    update.BindInt(2, static_cast<std::int64_t>(total));
    update.BindDouble(3, hotScore);
    update.BindInt(4, static_cast<std::int64_t>(id));
    if (!update.StepDone() || !database_.Exec("COMMIT;"))
    {
        return rollback();
    }
    return true;
}

bool ResourceRepository::LoadOpenStats(
    ResourceId id, std::int64_t* lastOpenTime, std::uint32_t* totalOpenCount,
    double* hotScore)
{
    if (lastOpenTime == nullptr || totalOpenCount == nullptr ||
        hotScore == nullptr)
    {
        return false;
    }
    db::Statement statement;
    if (!statement.Prepare(
            database_.handle(),
            "SELECT last_open_time, total_open_count, hot_score "
            "FROM resources WHERE id=?;"))
    {
        return false;
    }
    statement.BindInt(1, static_cast<std::int64_t>(id));
    if (!statement.StepRow())
    {
        return false;
    }
    *lastOpenTime = statement.ColumnInt(0);
    *totalOpenCount = static_cast<std::uint32_t>(statement.ColumnInt(1));
    *hotScore = statement.ColumnDouble(2);
    return true;
}

bool ResourceRepository::LogOperation(
    const std::string& operation, const std::filesystem::path& sourcePath,
    const std::filesystem::path& targetPath, ResourceId id, bool success)
{
    db::Statement statement;
    if (!statement.Prepare(database_.handle(),
                           "INSERT INTO operation_log(operation, source_path,"
                           " target_path, resource_id, time, success)"
                           " VALUES(?,?,?,?,?,?);"))
    {
        return false;
    }
    statement.BindText(1, operation);
    statement.BindText(2, PathKey(sourcePath));
    statement.BindText(3, PathKey(targetPath));
    statement.BindInt(4, static_cast<std::int64_t>(id));
    statement.BindInt(5, static_cast<std::int64_t>(std::time(nullptr)));
    statement.BindInt(6, success ? 1 : 0);
    return statement.StepDone();
}

}  // namespace nexus::repo

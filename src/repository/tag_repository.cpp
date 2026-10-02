#include "repository/tag_repository.h"

#include "util/string_util.h"

namespace nexus::repo {

namespace {

std::string NormalizeName(std::string value)
{
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return {};
    }
    const size_t last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1);
    return AsciiLower(value);
}

}  // namespace

TagRepository::TagRepository(db::Database& database) : database_(database)
{
}

std::int64_t TagRepository::FindOrCreate(const std::string& name)
{
    const std::string normalized = NormalizeName(name);
    if (normalized.empty())
    {
        return 0;
    }
    db::Statement find;
    if (!find.Prepare(database_.handle(),
                      "SELECT id FROM tags WHERE name = ?;"))
    {
        return 0;
    }
    find.BindText(1, normalized);
    if (find.StepRow())
    {
        return find.ColumnInt(0);
    }

    db::Statement insert;
    if (!insert.Prepare(database_.handle(),
                        "INSERT OR IGNORE INTO tags(name) VALUES(?);"))
    {
        return 0;
    }
    insert.BindText(1, normalized);
    if (!insert.StepDone())
    {
        return 0;
    }
    db::Statement resolve;
    if (!resolve.Prepare(database_.handle(),
                         "SELECT id FROM tags WHERE name = ?;"))
    {
        return 0;
    }
    resolve.BindText(1, normalized);
    return resolve.StepRow() ? resolve.ColumnInt(0) : 0;
}

std::vector<std::string> TagRepository::ResolveWord(const std::string& word)
{
    std::vector<std::string> names;

    db::Statement alias;
    if (alias.Prepare(database_.handle(),
                      "SELECT t.name FROM tag_alias a"
                      " JOIN tags t ON t.id = a.tag_id WHERE a.alias = ?;"))
    {
        alias.BindText(1, NormalizeName(word));
        while (alias.StepRow())
        {
            names.push_back(alias.ColumnText(0));
        }
    }

    db::Statement exact;
    if (exact.Prepare(database_.handle(),
                      "SELECT name FROM tags WHERE name = ?;"))
    {
        exact.BindText(1, NormalizeName(word));
        if (exact.StepRow())
        {
            names.push_back(exact.ColumnText(0));
        }
    }
    return names;
}

bool TagRepository::SetAlias(const std::string& alias,
                             const std::string& tagName)
{
    const std::string normalizedAlias = NormalizeName(alias);
    const std::int64_t tagId = FindOrCreate(tagName);
    if (tagId == 0 || normalizedAlias.empty())
    {
        return false;
    }
    db::Statement statement;
    if (!statement.Prepare(
            database_.handle(),
            "INSERT INTO tag_alias(alias, tag_id) VALUES(?, ?)"
            " ON CONFLICT(alias) DO UPDATE SET tag_id=excluded.tag_id;"))
    {
        return false;
    }
    statement.BindText(1, normalizedAlias);
    statement.BindInt(2, tagId);
    return statement.StepDone();
}

bool TagRepository::Attach(ResourceId resourceId, const std::string& tagName)
{
    const std::int64_t tagId = FindOrCreate(tagName);
    if (tagId == 0)
    {
        return false;
    }
    db::Statement statement;
    if (!statement.Prepare(database_.handle(),
                           "INSERT OR IGNORE INTO resource_tags(resource_id,"
                           " tag_id) VALUES(?, ?);"))
    {
        return false;
    }
    statement.BindInt(1, static_cast<std::int64_t>(resourceId));
    statement.BindInt(2, tagId);
    return statement.StepDone();
}

void TagRepository::Detach(ResourceId resourceId, const std::string& tagName)
{
    db::Statement statement;
    if (!statement.Prepare(database_.handle(),
                           "DELETE FROM resource_tags WHERE resource_id = ?"
                           " AND tag_id = (SELECT id FROM tags WHERE name = ?);"))
    {
        return;
    }
    statement.BindInt(1, static_cast<std::int64_t>(resourceId));
    statement.BindText(2, NormalizeName(tagName));
    statement.StepDone();
}

bool TagRepository::ClearForResource(ResourceId resourceId)
{
    db::Statement statement;
    if (!statement.Prepare(database_.handle(),
                           "DELETE FROM resource_tags WHERE resource_id=?;"))
    {
        return false;
    }
    statement.BindInt(1, static_cast<std::int64_t>(resourceId));
    return statement.StepDone();
}

std::vector<TagCount> TagRepository::LoadAllWithCounts()
{
    std::vector<TagCount> result;
    db::Statement statement;
    if (!statement.Prepare(database_.handle(),
                           "SELECT t.name, COUNT(rt.resource_id) FROM tags t"
                           " LEFT JOIN resource_tags rt ON rt.tag_id = t.id"
                           " GROUP BY t.id"
                           " ORDER BY COUNT(rt.resource_id) DESC, t.name;"))
    {
        return result;
    }
    while (statement.StepRow())
    {
        result.push_back({statement.ColumnText(0),
                          static_cast<int>(statement.ColumnInt(1))});
    }
    return result;
}

std::vector<std::string> TagRepository::RecentTags(int limit)
{
    std::vector<std::string> names;
    db::Statement statement;
    if (!statement.Prepare(database_.handle(),
                           "SELECT name FROM tags ORDER BY id DESC LIMIT ?;"))
    {
        return names;
    }
    statement.BindInt(1, limit);
    while (statement.StepRow())
    {
        names.push_back(statement.ColumnText(0));
    }
    return names;
}

}  // namespace nexus::repo

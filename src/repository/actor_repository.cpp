#include "repository/actor_repository.h"

namespace nexus::repo {

ActorRepository::ActorRepository(db::Database& database) : database_(database)
{
}

std::int64_t ActorRepository::FindOrCreate(const std::string& name)
{
    db::Statement insert;
    if (!insert.Prepare(database_.handle(),
                        "INSERT OR IGNORE INTO actors(name) VALUES(?);"))
    {
        return 0;
    }
    insert.BindText(1, name);
    if (!insert.StepDone())
    {
        return 0;
    }
    db::Statement find;
    if (!find.Prepare(database_.handle(),
                      "SELECT id FROM actors WHERE name=?;"))
    {
        return 0;
    }
    find.BindText(1, name);
    return find.StepRow() ? find.ColumnInt(0) : 0;
}

bool ActorRepository::Attach(ResourceId resourceId,
                             const std::string& actorName)
{
    const std::int64_t actorId = FindOrCreate(actorName);
    if (actorId == 0)
    {
        return false;
    }
    db::Statement statement;
    if (!statement.Prepare(
            database_.handle(),
            "INSERT OR IGNORE INTO resource_actors(resource_id, actor_id)"
            " VALUES(?,?);"))
    {
        return false;
    }
    statement.BindInt(1, static_cast<std::int64_t>(resourceId));
    statement.BindInt(2, actorId);
    return statement.StepDone();
}

bool ActorRepository::Detach(ResourceId resourceId,
                             const std::string& actorName)
{
    db::Statement statement;
    if (!statement.Prepare(
            database_.handle(),
            "DELETE FROM resource_actors WHERE resource_id=? AND actor_id="
            "(SELECT id FROM actors WHERE name=?);"))
    {
        return false;
    }
    statement.BindInt(1, static_cast<std::int64_t>(resourceId));
    statement.BindText(2, actorName);
    return statement.StepDone();
}

bool ActorRepository::ClearForResource(ResourceId resourceId)
{
    db::Statement statement;
    if (!statement.Prepare(database_.handle(),
                           "DELETE FROM resource_actors WHERE resource_id=?;"))
    {
        return false;
    }
    statement.BindInt(1, static_cast<std::int64_t>(resourceId));
    return statement.StepDone();
}

std::vector<std::string> ActorRepository::ForResource(ResourceId resourceId)
{
    std::vector<std::string> actors;
    db::Statement statement;
    if (!statement.Prepare(
            database_.handle(),
            "SELECT a.name FROM resource_actors ra JOIN actors a"
            " ON a.id=ra.actor_id WHERE ra.resource_id=? ORDER BY a.name;"))
    {
        return actors;
    }
    statement.BindInt(1, static_cast<std::int64_t>(resourceId));
    while (statement.StepRow())
    {
        actors.push_back(statement.ColumnText(0));
    }
    return actors;
}

}  // namespace nexus::repo

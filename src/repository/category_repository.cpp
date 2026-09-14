#include "repository/category_repository.h"

namespace nexus::repo {

CategoryRepository::CategoryRepository(db::Database& database)
    : database_(database)
{
}

std::int64_t CategoryRepository::FindOrCreate(const std::string& name)
{
    db::Statement insert;
    if (!insert.Prepare(database_.handle(),
                        "INSERT OR IGNORE INTO categories(name) VALUES(?);"))
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
                      "SELECT id FROM categories WHERE name = ?;"))
    {
        return 0;
    }
    find.BindText(1, name);
    return find.StepRow() ? find.ColumnInt(0) : 0;
}

bool CategoryRepository::Assign(ResourceId resourceId,
                                std::int64_t categoryId)
{
    db::Statement statement;
    if (!statement.Prepare(database_.handle(),
                           "UPDATE resources SET category_id=? WHERE id=?;"))
    {
        return false;
    }
    statement.BindInt(1, categoryId);
    statement.BindInt(2, static_cast<std::int64_t>(resourceId));
    return statement.StepDone();
}

bool CategoryRepository::Clear(ResourceId resourceId)
{
    db::Statement statement;
    if (!statement.Prepare(database_.handle(),
                           "UPDATE resources SET category_id=NULL WHERE id=?;"))
    {
        return false;
    }
    statement.BindInt(1, static_cast<std::int64_t>(resourceId));
    return statement.StepDone();
}

std::vector<Category> CategoryRepository::LoadAll()
{
    std::vector<Category> categories;
    db::Statement statement;
    if (!statement.Prepare(database_.handle(),
                           "SELECT id, name FROM categories ORDER BY name;"))
    {
        return categories;
    }
    while (statement.StepRow())
    {
        categories.push_back({statement.ColumnInt(0), statement.ColumnText(1)});
    }
    return categories;
}

}  // namespace nexus::repo

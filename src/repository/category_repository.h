#ifndef NEXUS_SRC_REPOSITORY_CATEGORY_REPOSITORY_H_
#define NEXUS_SRC_REPOSITORY_CATEGORY_REPOSITORY_H_

#include <cstdint>
#include <string>
#include <vector>

#include "database/sqlite_database.h"
#include "domain/resource.h"

namespace nexus::repo {

struct Category
{
    std::int64_t id = 0;
    std::string name;
};

class CategoryRepository {
public:
    explicit CategoryRepository(db::Database& database);

    std::int64_t FindOrCreate(const std::string& name);

    bool Assign(ResourceId resourceId, std::int64_t categoryId);

    bool Clear(ResourceId resourceId);

    std::vector<Category> LoadAll();

private:
    db::Database& database_;
};

}  // namespace nexus::repo

#endif  // NEXUS_SRC_REPOSITORY_CATEGORY_REPOSITORY_H_

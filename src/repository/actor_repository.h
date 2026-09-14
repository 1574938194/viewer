#ifndef NEXUS_SRC_REPOSITORY_ACTOR_REPOSITORY_H_
#define NEXUS_SRC_REPOSITORY_ACTOR_REPOSITORY_H_

#include <cstdint>
#include <string>
#include <vector>

#include "database/sqlite_database.h"
#include "domain/resource.h"

namespace nexus::repo {

class ActorRepository {
public:
    explicit ActorRepository(db::Database& database);

    std::int64_t FindOrCreate(const std::string& name);

    bool Attach(ResourceId resourceId, const std::string& actorName);

    bool Detach(ResourceId resourceId, const std::string& actorName);

    bool ClearForResource(ResourceId resourceId);

    std::vector<std::string> ForResource(ResourceId resourceId);

private:
    db::Database& database_;
};

}  // namespace nexus::repo

#endif  // NEXUS_SRC_REPOSITORY_ACTOR_REPOSITORY_H_

#ifndef NEXUS_SRC_REPOSITORY_TAG_REPOSITORY_H_
#define NEXUS_SRC_REPOSITORY_TAG_REPOSITORY_H_

#include <cstdint>
#include <string>
#include <vector>

#include "database/sqlite_database.h"
#include "domain/resource.h"

namespace nexus::repo {

// Tag name with the number of resources attached to it; used by the sidebar.
struct TagCount
{
    std::string name;
    int count = 0;
};

// Tag and alias persistence (design doc sections 17-18).
class TagRepository
{
public:
    explicit TagRepository(db::Database& database);

    // Returns the tag id, creating the tag when needed.
    std::int64_t FindOrCreate(const std::string& name);

    // All tags with resource counts, most used first.
    std::vector<TagCount> LoadAllWithCounts();

    // Resolves a query word through the alias table first, then exact tag
    // names; empty when the word matches no tag (design doc section 18).
    std::vector<std::string> ResolveWord(const std::string& word);

    bool SetAlias(const std::string& alias, const std::string& tagName);

    bool Attach(ResourceId resourceId, const std::string& tagName);

    void Detach(ResourceId resourceId, const std::string& tagName);

    bool ClearForResource(ResourceId resourceId);

    std::vector<std::string> RecentTags(int limit);

private:
    db::Database& database_;
};

}  // namespace nexus::repo

#endif  // NEXUS_SRC_REPOSITORY_TAG_REPOSITORY_H_

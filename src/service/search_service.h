#ifndef NEXUS_SRC_SERVICE_SEARCH_SERVICE_H_
#define NEXUS_SRC_SERVICE_SEARCH_SERVICE_H_

#include <functional>
#include <string>
#include <vector>

#include "domain/resource.h"

namespace nexus::repo {
class ResourceRepository;
}

namespace nexus::service {

using AliasResolver =
    std::function<std::vector<std::string>(const std::string&)>;

class SearchService {
public:
    // Searches the SQLite FTS5 index first and applies the structured query
    // filters/ranking to the returned candidate set. FTS5 is required by the
    // database schema; there is intentionally no full in-memory fallback.
    static std::vector<const Resource*> Search(
        repo::ResourceRepository& repository,
        const std::vector<Resource>& resources, const std::wstring& query,
        const AliasResolver& resolveAlias);
};

}  // namespace nexus::service

#endif  // NEXUS_SRC_SERVICE_SEARCH_SERVICE_H_

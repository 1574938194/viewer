#ifndef NEXUS_SRC_DOMAIN_RESOURCE_H_
#define NEXUS_SRC_DOMAIN_RESOURCE_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace nexus {

using ResourceId = std::uint64_t;

enum class ResourceType : std::uint8_t
{
    kVideo,
    kImage,
    kFolder,
};

struct Resource
{
    ResourceId id = 0;

    ResourceType type = ResourceType::kVideo;

    std::filesystem::path path;

    std::string originalFilename;
    std::string title;
    std::string code;

    std::optional<std::int64_t> categoryId;
    std::string categoryName;

    std::uint64_t fileSize = 0;

    // Lowercase SHA-256 of the file contents. Empty for legacy records whose
    // hash has not been computed yet.
    std::string contentHash;

    std::uint32_t width = 0;
    std::uint32_t height = 0;

    std::int64_t duration = 0;  // seconds, video only

    std::int64_t addedTime = 0;      // unix seconds
    std::int64_t modifiedTime = 0;   // unix seconds
    std::int64_t lastOpenTime = 0;   // unix seconds
    std::uint32_t totalOpenCount = 0;

    double hotScore = 0.0;  // cached in DB, refreshed on open (section 45)

    std::vector<std::string> tags;
    std::vector<std::string> actors;

    bool favorite = false;
    bool missing = false;
};

}  // namespace nexus

#endif  // NEXUS_SRC_DOMAIN_RESOURCE_H_

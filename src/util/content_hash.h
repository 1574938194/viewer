#ifndef NEXUS_SRC_UTIL_CONTENT_HASH_H_
#define NEXUS_SRC_UTIL_CONTENT_HASH_H_

#include <filesystem>
#include <optional>
#include <string>

namespace nexus::util {

// Computes a lowercase SHA-256 digest without loading the whole file into
// memory. A missing or unreadable file returns nullopt.
std::optional<std::string> ComputeFileSha256(
    const std::filesystem::path& path);

}  // namespace nexus::util

#endif  // NEXUS_SRC_UTIL_CONTENT_HASH_H_

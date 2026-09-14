#ifndef NEXUS_SRC_DOMAIN_SAMPLE_ASSETS_H_
#define NEXUS_SRC_DOMAIN_SAMPLE_ASSETS_H_

#include <filesystem>

namespace nexus {

// Generates the demo images used to seed an empty library on first run.
// Real content arrives through the import pipeline.
class SampleAssets
{
public:
    static std::filesystem::path SampleDir();

    static void EnsureSampleImages(const std::filesystem::path& dir);
};

}  // namespace nexus

#endif  // NEXUS_SRC_DOMAIN_SAMPLE_ASSETS_H_

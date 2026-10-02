#ifndef NEXUS_SRC_SERVICE_SCAN_SERVICE_H_
#define NEXUS_SRC_SERVICE_SCAN_SERVICE_H_

#include <cstddef>
#include <filesystem>
#include <functional>

namespace nexus::service {

struct ScanResult {
    int added = 0;
    int skipped = 0;
    int failed = 0;
    int missingUpdated = 0;
    int incoming = 0;
    int updated = 0;
};

class ScanService {
public:
    // Adds lightweight rows for media files that are not indexed yet.  This
    // intentionally skips content hashes, dimensions, durations and other
    // expensive work so callers can render card text immediately; Run() can
    // be scheduled afterwards to enrich those rows in the background.
    static ScanResult RunLightweight(
        const std::filesystem::path& databasePath,
        const std::filesystem::path& libraryRoot,
        const std::function<void()>& onFirstCard,
        const std::function<bool()>& shouldStop);

    static ScanResult RunLightweight(const std::filesystem::path& databasePath,
                                     const std::filesystem::path& libraryRoot,
                                     const std::function<void()>& onFirstCard);

    static ScanResult RunLightweight(
        const std::filesystem::path& databasePath,
        const std::filesystem::path& libraryRoot)
    {
        return RunLightweight(databasePath, libraryRoot, {});
    }

    // Enriches only rows with missing derived data (hash, image dimensions or
    // video duration).  Work is fetched and committed in bounded pages so a
    // large library can be built incrementally after the UI is ready.  The
    // callback runs once per completed page on the worker thread.
    static ScanResult RunIncremental(
        const std::filesystem::path& databasePath,
        const std::filesystem::path& libraryRoot, std::size_t batchSize = 24,
        const std::function<void(const ScanResult&)>& onBatch = {},
        const std::function<bool()>& shouldStop = {});

    static ScanResult Run(const std::filesystem::path& databasePath,
                          const std::filesystem::path& libraryRoot);
};

}  // namespace nexus::service

#endif  // NEXUS_SRC_SERVICE_SCAN_SERVICE_H_

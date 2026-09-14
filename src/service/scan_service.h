#ifndef NEXUS_SRC_SERVICE_SCAN_SERVICE_H_
#define NEXUS_SRC_SERVICE_SCAN_SERVICE_H_

#include <filesystem>

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
    static ScanResult Run(const std::filesystem::path& databasePath,
                          const std::filesystem::path& libraryRoot);
};

}  // namespace nexus::service

#endif  // NEXUS_SRC_SERVICE_SCAN_SERVICE_H_

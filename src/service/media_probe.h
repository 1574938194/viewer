#ifndef NEXUS_SRC_SERVICE_MEDIA_PROBE_H_
#define NEXUS_SRC_SERVICE_MEDIA_PROBE_H_

#include <cstdint>
#include <filesystem>

namespace nexus::service {

// Returns the duration of a media file in whole seconds.  FFmpeg's libavformat
// is used first, followed by a bundled/system ffprobe and finally the Windows
// media property system for installations that do not ship FFmpeg binaries.
// A non-positive result means that the duration could not be determined.
std::int64_t DetectMediaDuration(const std::filesystem::path& path);

// Reads the first image frame dimensions through WIC.  This is intentionally
// a small, worker-friendly probe used when a legacy database row has not yet
// been enriched.  A false return means the file could not be decoded.
bool DetectImageDimensions(const std::filesystem::path& path,
                           std::uint32_t* width, std::uint32_t* height);

}  // namespace nexus::service

#endif  // NEXUS_SRC_SERVICE_MEDIA_PROBE_H_

#include "service/media_probe.h"

#include <Windows.h>
#include <propkey.h>
#include <propsys.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>

extern "C" {
#include <libavcodec/codec_par.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "util/string_util.h"

namespace nexus::service {

namespace {

std::filesystem::path ModuleDirectory()
{
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
    {
        return {};
    }
    return std::filesystem::path(buffer, buffer + length).parent_path();
}

std::optional<std::filesystem::path> FindFfprobe()
{
    const std::filesystem::path moduleDirectory = ModuleDirectory();
    const std::vector<std::filesystem::path> candidates = {
        moduleDirectory / L"ffprobe.exe",
        moduleDirectory / L"tools" / L"ffmpeg" / L"bin" / L"ffprobe.exe",
        moduleDirectory / L"ffmpeg" / L"bin" / L"ffprobe.exe"};
    for (const std::filesystem::path& candidate : candidates)
    {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error)
        {
            return candidate;
        }
    }

    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = SearchPathW(nullptr, L"ffprobe.exe", nullptr,
                                     MAX_PATH, buffer, nullptr);
    if (length != 0 && length < MAX_PATH)
    {
        return std::filesystem::path(buffer, buffer + length);
    }
    return std::nullopt;
}

std::int64_t RoundSeconds(long double seconds)
{
    if (!(seconds > 0.0L) ||
        seconds > static_cast<long double>(
                      std::numeric_limits<std::int64_t>::max()))
    {
        return 0;
    }
    return static_cast<std::int64_t>(std::llround(seconds));
}

std::int64_t ProbeWithLibraries(const std::filesystem::path& path)
{
    static std::once_flag quietLogs;
    std::call_once(quietLogs, [] { av_log_set_level(AV_LOG_QUIET); });

    const std::string utf8Path = WideToUtf8(path.wstring());
    if (utf8Path.empty())
    {
        return 0;
    }

    AVFormatContext* context = nullptr;
    if (avformat_open_input(&context, utf8Path.c_str(), nullptr, nullptr) < 0 ||
        context == nullptr)
    {
        return 0;
    }

    // Most MP4/MKV headers already carry a format duration.  Avoid probing the
    // packet stream in that common case; it can otherwise read a substantial
    // amount of data from a large file.  Containers without a header duration
    // are given the usual stream-info pass below.
    if (context->duration == AV_NOPTS_VALUE || context->duration <= 0)
    {
        avformat_find_stream_info(context, nullptr);
    }

    std::int64_t result = 0;
    if (context->duration != AV_NOPTS_VALUE && context->duration > 0)
    {
        result = RoundSeconds(static_cast<long double>(context->duration) /
                              static_cast<long double>(AV_TIME_BASE));
    }
    if (result == 0)
    {
        for (unsigned int i = 0; i < context->nb_streams; ++i)
        {
            const AVStream* stream = context->streams[i];
            if (stream == nullptr || stream->codecpar == nullptr ||
                stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO ||
                stream->duration <= 0 || stream->time_base.num <= 0 ||
                stream->time_base.den <= 0)
            {
                continue;
            }
            const long double seconds =
                static_cast<long double>(stream->duration) *
                static_cast<long double>(stream->time_base.num) /
                static_cast<long double>(stream->time_base.den);
            result = RoundSeconds(seconds);
            if (result > 0)
            {
                break;
            }
        }
    }
    avformat_close_input(&context);
    return result;
}

std::int64_t ProbeWithFfprobe(const std::filesystem::path& path)
{
    const std::optional<std::filesystem::path> executable = FindFfprobe();
    if (!executable.has_value())
    {
        return 0;
    }
    const std::wstring command =
        L"\"" + executable->wstring() +
        L"\" -v error -show_entries format=duration "
        L"-of default=noprint_wrappers=1:nokey=1 \"" + path.wstring() +
        L"\" 2>NUL";
    FILE* pipe = _wpopen(command.c_str(), L"r");
    if (pipe == nullptr)
    {
        return 0;
    }
    wchar_t buffer[128] = {};
    const bool read = fgetws(buffer, static_cast<int>(std::size(buffer)),
                              pipe) != nullptr;
    const int exitCode = _pclose(pipe);
    if (!read || exitCode != 0)
    {
        return 0;
    }
    wchar_t* end = nullptr;
    const double seconds = std::wcstod(buffer, &end);
    if (end == buffer || !std::isfinite(seconds) || seconds <= 0.0)
    {
        return 0;
    }
    return RoundSeconds(static_cast<long double>(seconds));
}

std::int64_t ProbeWithPropertySystem(const std::filesystem::path& path)
{
    Microsoft::WRL::ComPtr<IPropertyStore> store;
    const HRESULT openResult = SHGetPropertyStoreFromParsingName(
        path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&store));
    if (FAILED(openResult) || !store)
    {
        return 0;
    }

    PROPVARIANT value;
    PropVariantInit(&value);
    const HRESULT readResult = store->GetValue(PKEY_Media_Duration, &value);
    std::uint64_t ticks = 0;
    if (SUCCEEDED(readResult))
    {
        if (value.vt == VT_UI8)
        {
            ticks = value.uhVal.QuadPart;
        }
        else if (value.vt == VT_I8 && value.hVal.QuadPart > 0)
        {
            ticks = static_cast<std::uint64_t>(value.hVal.QuadPart);
        }
        else if (value.vt == VT_UI4)
        {
            ticks = value.ulVal;
        }
    }
    PropVariantClear(&value);
    // System.Media.Duration is expressed in 100-nanosecond units.
    return static_cast<std::int64_t>(ticks / 10000000ULL);
}

}  // namespace

std::int64_t DetectMediaDuration(const std::filesystem::path& path)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error)
    {
        return 0;
    }
    const std::int64_t libraryDuration = ProbeWithLibraries(path);
    if (libraryDuration > 0)
    {
        return libraryDuration;
    }
    const std::int64_t executableDuration = ProbeWithFfprobe(path);
    if (executableDuration > 0)
    {
        return executableDuration;
    }
    return ProbeWithPropertySystem(path);
}

bool DetectImageDimensions(const std::filesystem::path& path,
                           std::uint32_t* width, std::uint32_t* height)
{
    if (width == nullptr || height == nullptr)
    {
        return false;
    }
    *width = 0;
    *height = 0;
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error)
    {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (FAILED(hr) || !wic)
    {
        return false;
    }
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    hr = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                        WICDecodeMetadataCacheOnDemand,
                                        &decoder);
    if (FAILED(hr) || !decoder)
    {
        return false;
    }
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame)
    {
        return false;
    }
    UINT imageWidth = 0;
    UINT imageHeight = 0;
    if (FAILED(frame->GetSize(&imageWidth, &imageHeight)) || imageWidth == 0 ||
        imageHeight == 0)
    {
        return false;
    }
    *width = imageWidth;
    *height = imageHeight;
    return true;
}

}  // namespace nexus::service

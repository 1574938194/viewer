#include "service/import_service.h"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <ctime>
#include <chrono>
#include <regex>
#include <sstream>
#include <string>

#include "database/migration.h"
#include "database/sqlite_database.h"
#include "repository/actor_repository.h"
#include "repository/category_repository.h"
#include "repository/resource_repository.h"
#include "repository/tag_repository.h"
#include "service/delete_service.h"
#include "service/media_probe.h"
#include "util/content_hash.h"
#include "util/string_util.h"

namespace nexus::service {

namespace {

// Supported first-version formats (design doc sections 13-14).
bool IsVideoExtension(const std::wstring& extension)
{
    return extension == L".mp4" || extension == L".mkv" ||
           extension == L".avi" || extension == L".wmv" ||
           extension == L".mov" || extension == L".m4v" ||
           extension == L".webm" || extension == L".ts";
}

bool IsImageExtension(const std::wstring& extension)
{
    return extension == L".jpg" || extension == L".jpeg" ||
           extension == L".png" || extension == L".webp" ||
           extension == L".bmp";
}

// Reads pixel dimensions with WIC (import-time only, worker thread).
bool DetectImageSize(const std::filesystem::path& path, std::uint32_t* width,
                     std::uint32_t* height)
{
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (FAILED(hr))
    {
        return false;
    }
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    hr = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                        WICDecodeMetadataCacheOnDemand,
                                        &decoder);
    if (FAILED(hr))
    {
        return false;
    }
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr))
    {
        return false;
    }
    UINT w = 0;
    UINT h = 0;
    frame->GetSize(&w, &h);
    *width = w;
    *height = h;
    return w > 0 && h > 0;
}

// Extracts codes like ABC-123 / abc007 from the filename (best effort).
std::string ExtractCode(const std::string& filename)
{
    std::regex pattern(R"(([A-Za-z]{2,8})[-_ ]?(\d{2,5}))");
    std::smatch match;
    if (std::regex_search(filename, match, pattern))
    {
        std::string code = match[1].str() + "-" + match[2].str();
        for (char& c : code)
        {
            if (c >= 'a' && c <= 'z')
            {
                c += 'A' - 'a';
            }
        }
        return code;
    }
    return {};
}

std::vector<std::string> ExtractTags(const std::string& filename,
                                     const std::string& code)
{
    std::string stem = filename;
    const size_t extension = stem.find_last_of('.');
    if (extension != std::string::npos)
    {
        stem.resize(extension);
    }
    for (char& character : stem)
    {
        if (character == '-' || character == '_' || character == '[' ||
            character == ']' || character == '(' || character == ')')
        {
            character = ' ';
        }
    }
    std::vector<std::string> tags;
    std::string codePrefix = code;
    const size_t separator = codePrefix.find('-');
    if (separator != std::string::npos)
    {
        codePrefix.resize(separator);
    }
    codePrefix = AsciiLower(codePrefix);
    std::istringstream stream(stem);
    std::string token;
    while (stream >> token)
    {
        if (token.size() < 2 || token == code || token == "sample" ||
            AsciiLower(token) == codePrefix)
        {
            continue;
        }
        bool numeric = true;
        for (const char character : token)
        {
            numeric = numeric && character >= '0' && character <= '9';
        }
        if (!numeric)
        {
            tags.push_back(token);
        }
    }
    return tags;
}

std::int64_t FileModifiedTime(const std::filesystem::path& path,
                              std::int64_t fallback)
{
    std::error_code error;
    const auto fileTime = std::filesystem::last_write_time(path, error);
    if (error)
    {
        return fallback;
    }
    const auto systemTime =
        std::chrono::system_clock::now() +
        (fileTime - std::filesystem::file_time_type::clock::now());
    return std::chrono::duration_cast<std::chrono::seconds>(
               systemTime.time_since_epoch())
        .count();
}

std::string DirectoryTag(const std::filesystem::path& directory)
{
    std::filesystem::path name = directory.filename();
    if (name.empty())
    {
        name = directory.parent_path().filename();
    }
    return WideToUtf8(name.wstring());
}

void AppendDirectoryItems(const std::filesystem::path& directory,
                          const std::vector<std::string>& inheritedTags,
                          std::vector<ImportItem>* output)
{
    if (output == nullptr)
    {
        return;
    }

    std::vector<std::string> directoryTags = inheritedTags;
    const std::string directoryTag = DirectoryTag(directory);
    if (!directoryTag.empty())
    {
        directoryTags.push_back(directoryTag);
    }

    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        directory, std::filesystem::directory_options::skip_permission_denied,
        error);
    if (error)
    {
        return;
    }
    const std::filesystem::recursive_directory_iterator end;
    for (; iterator != end; iterator.increment(error))
    {
        if (error)
        {
            error.clear();
            continue;
        }
        std::error_code entryError;
        if (!iterator->is_regular_file(entryError) || entryError)
        {
            continue;
        }
        if (!ImportService::Classify(iterator->path()).has_value())
        {
            continue;
        }
        output->push_back({iterator->path(), directoryTags});
    }
}

std::vector<ImportItem> ExpandImportItems(
    const std::vector<ImportItem>& inputs)
{
    std::vector<ImportItem> expanded;
    for (const ImportItem& input : inputs)
    {
        std::error_code error;
        if (std::filesystem::is_directory(input.path, error) && !error)
        {
            AppendDirectoryItems(input.path, input.inheritedTags, &expanded);
        }
        else
        {
            // Preserve regular and invalid paths.  ImportOne performs the
            // final validation and reports unsupported/missing files as
            // skipped, matching the pre-directory API behaviour.
            expanded.push_back(input);
        }
    }
    return expanded;
}

}  // namespace

ImportService::ImportService(std::filesystem::path databasePath,
                             ArchiveService& archive)
    : databasePath_(std::move(databasePath)), archive_(archive)
{
}

ImportService::~ImportService()
{
    Shutdown();
}

void ImportService::Start(HWND notifyWindow, UINT doneMessage,
                          UINT progressMessage)
{
    if (worker_.joinable())
    {
        return;
    }
    notifyWindow_ = notifyWindow;
    doneMessage_ = doneMessage;
    progressMessage_ = progressMessage;
    stop_ = false;
    worker_ = std::thread([this] { WorkerLoop(); });
}

ImportService::Progress ImportService::GetProgress() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return progress_;
}

void ImportService::SetProgress(const Progress& progress, bool notify)
{
    HWND notifyWindow = nullptr;
    UINT progressMessage = 0;
    bool shouldNotify = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool becameActive = progress.active && !progress_.active;
        progress_ = progress;
        notifyWindow = notifyWindow_;
        progressMessage = progressMessage_;
        if (notify && progressMessage != 0 && notifyWindow != nullptr)
        {
            const auto now = std::chrono::steady_clock::now();
            const bool first = becameActive ||
                lastProgressPost_.time_since_epoch().count() == 0;
            const bool finished = !progress.active;
            const bool elapsed = !first &&
                now - lastProgressPost_ >= std::chrono::milliseconds(100);
            if (first || finished || elapsed)
            {
                lastProgressPost_ = now;
                shouldNotify = true;
            }
        }
    }
    if (shouldNotify && IsWindow(notifyWindow))
    {
        PostMessageW(notifyWindow, progressMessage, 0, 0);
    }
}

void ImportService::Shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable())
    {
        worker_.join();
    }
}

std::optional<ResourceType> ImportService::Classify(
    const std::filesystem::path& path)
{
    std::wstring extension = path.extension().wstring();
    for (wchar_t& c : extension)
    {
        if (c >= L'A' && c <= L'Z')
        {
            c += L'a' - L'A';
        }
    }
    if (IsVideoExtension(extension))
    {
        return ResourceType::kVideo;
    }
    if (IsImageExtension(extension))
    {
        return ResourceType::kImage;
    }
    return std::nullopt;
}

void ImportService::Import(std::vector<std::filesystem::path> files,
                           ImportMetadata metadata)
{
    if (files.empty())
    {
        return;
    }
    std::vector<ImportItem> items;
    items.reserve(files.size());
    for (std::filesystem::path& file : files)
    {
        items.push_back({std::move(file), {}});
    }
    ImportItems(std::move(items), std::move(metadata));
}

void ImportService::ImportItems(std::vector<ImportItem> items,
                                ImportMetadata metadata)
{
    if (items.empty())
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back({std::move(items), std::move(metadata)});
    }
    cv_.notify_one();
}

std::vector<ImportItem> ImportService::ExpandInputs(
    const std::vector<std::filesystem::path>& inputs)
{
    std::vector<ImportItem> items;
    items.reserve(inputs.size());
    for (const std::filesystem::path& input : inputs)
    {
        items.push_back({input, {}});
    }
    return ExpandImportItems(items);
}

ImportService::BatchResult ImportService::ImportOne(
    const ImportItem& item, const ImportMetadata& metadata,
    repo::ResourceRepository& resources, repo::TagRepository& tags,
    const ArchiveService::ProgressCallback& progress)
{
    BatchResult result;
    std::error_code ec;
    const std::filesystem::path& file = item.path;

    auto type = Classify(file);
    if (!type.has_value())
    {
        ++result.skipped;
        return result;
    }
    if (!std::filesystem::exists(file, ec) || ec)
    {
        ++result.skipped;
        return result;
    }
    // Cheap duplicate check: same source path already imported (section 74).
    if (resources.ExistsPath(file))
    {
        ++result.skipped;
        return result;
    }

    const std::string filename = WideToUtf8(file.filename().wstring());
    std::error_code sizeError;
    const std::uint64_t sourceSize = std::filesystem::file_size(file, sizeError);
    if (sizeError)
    {
        ++result.failed;
        return result;
    }
    if (const auto duplicate = resources.FindDuplicate(sourceSize, filename);
        duplicate.has_value())
    {
        resources.LogOperation("DUPLICATE", file, {}, *duplicate, false);
        ++result.skipped;
        return result;
    }
    const std::optional<std::string> contentHash =
        util::ComputeFileSha256(file);
    if (contentHash.has_value())
    {
        if (const auto duplicate = resources.FindDuplicate(
                sourceSize, filename, *contentHash);
            duplicate.has_value())
        {
            resources.LogOperation("DUPLICATE", file, {}, *duplicate, false);
            ++result.skipped;
            return result;
        }
    }
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));

    Resource resource;
    resource.type = *type;
    // The source path is temporary until the archive assigns the managed path.
    resource.path = file;
    resource.originalFilename = filename;
    resource.title = WideToUtf8(file.stem().wstring());
    if (metadata.overrideTitle && !metadata.title.empty())
    {
        resource.title = metadata.title;
    }
    resource.code = ExtractCode(filename);
    resource.tags = ExtractTags(filename, resource.code);
    resource.tags.insert(resource.tags.end(), metadata.tags.begin(),
                         metadata.tags.end());
    resource.tags.insert(resource.tags.end(), item.inheritedTags.begin(),
                         item.inheritedTags.end());
    resource.favorite = metadata.favorite;
    resource.fileSize = sourceSize;
    resource.contentHash = contentHash.value_or(std::string{});
    resource.addedTime = now;
    resource.modifiedTime = FileModifiedTime(file, now);
    if (*type == ResourceType::kImage)
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        if (DetectImageSize(file, &width, &height))
        {
            resource.width = width;
            resource.height = height;
        }
    }
    else
    {
        resource.duration = DetectMediaDuration(file);
    }

    // Insert + archive + update run in one transaction: on failure the
    // rollback leaves no trace and the source is untouched (section 33).
    db::Database& db = resources.database();
    if (!db.Exec("BEGIN;"))
    {
        ++result.failed;
        return result;
    }
    if (!resources.Insert(resource))
    {
        db.Exec("ROLLBACK;");
        ++result.failed;
        return result;
    }

    // Source deletion must occur only after the resource transaction commits.
    // Same-volume moves are restored below should that transaction fail.
    ArchiveResult archive = archive_.Archive(file, resource.id, resource.type,
                                             /*deleteSource=*/false, progress);
    if (!archive.ok)
    {
        db.Exec("ROLLBACK;");
        resources.LogOperation("IMPORT", file, {}, resource.id, false);
        ++result.failed;
        return result;
    }
    if (contentHash.has_value())
    {
        const std::optional<std::string> archivedHash =
            util::ComputeFileSha256(archive.finalPath);
        if (!archivedHash.has_value() || *archivedHash != *contentHash)
        {
            db.Exec("ROLLBACK;");
            archive_.Rollback(file, archive);
            resources.LogOperation("IMPORT", file, archive.finalPath,
                                   resource.id, false);
            ++result.failed;
            return result;
        }
    }
    resource.path = archive.finalPath;
    if (!resources.UpdatePath(resource.id, archive.finalPath))
    {
        db.Exec("ROLLBACK;");
        archive_.Rollback(file, archive);
        ++result.failed;
        return result;
    }
    for (const std::string& tag : resource.tags)
    {
        if (!tags.Attach(resource.id, tag))
        {
            db.Exec("ROLLBACK;");
            archive_.Rollback(file, archive);
            ++result.failed;
            return result;
        }
    }
    repo::CategoryRepository categories(db);
    if (!metadata.category.empty())
    {
        const std::int64_t categoryId =
            categories.FindOrCreate(metadata.category);
        if (categoryId == 0 || !categories.Assign(resource.id, categoryId))
        {
            db.Exec("ROLLBACK;");
            archive_.Rollback(file, archive);
            ++result.failed;
            return result;
        }
    }
    repo::ActorRepository actors(db);
    for (const std::string& actor : metadata.actors)
    {
        if (!actors.Attach(resource.id, actor))
        {
            db.Exec("ROLLBACK;");
            archive_.Rollback(file, archive);
            ++result.failed;
            return result;
        }
    }
    if (!db.Exec("COMMIT;"))
    {
        db.Exec("ROLLBACK;");
        archive_.Rollback(file, archive);
        ++result.failed;
        return result;
    }
    if (metadata.deleteSource && std::filesystem::exists(file, ec) &&
        !DeleteResource(nullptr, file, DeleteMode::kRecycle, nullptr))
    {
        // The committed archive is authoritative. Keeping a duplicate source
        // is safe and is recorded for recovery rather than failing the import.
        resources.LogOperation("DELETE", file, archive.finalPath, resource.id,
                               false);
    }
    resources.LogOperation("IMPORT", file, archive.finalPath, resource.id,
                           true);
    ++result.imported;
    return result;
}

void ImportService::WorkerLoop()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInitialized = SUCCEEDED(hr);

    db::Database database;
    const bool databaseReady =
        database.Open(databasePath_) && db::Migrate(database);
    repo::ResourceRepository resources(database);
    repo::TagRepository tags(database);

    while (true)
    {
        Request request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty())
            {
                break;
            }
            request = std::move(queue_.front());
            queue_.pop_front();
        }

        const std::vector<ImportItem> items = ExpandImportItems(request.items);
        BatchResult total;
        std::vector<std::uint64_t> fileSizes(items.size(), 0);
        std::uint64_t totalBytes = 0;
        for (std::size_t i = 0; i < items.size(); ++i)
        {
            std::error_code sizeError;
            fileSizes[i] = std::filesystem::file_size(items[i].path,
                                                      sizeError);
            if (!sizeError)
            {
                totalBytes += fileSizes[i];
            }
        }
        Progress state;
        state.active = true;
        state.totalFiles = items.size();
        state.totalBytes = totalBytes;
        SetProgress(state, true);
        std::uint64_t completedBytes = 0;
        for (std::size_t i = 0; i < items.size(); ++i)
        {
            const std::filesystem::path& file = items[i].path;
            state.currentFile = i;
            state.currentPath = file.wstring();
            state.bytesCopied = completedBytes;
            state.totalBytes = totalBytes;
            SetProgress(state, true);
            if (!databaseReady)
            {
                ++total.failed;
            }
            else
            {
                const ArchiveService::ProgressCallback callback =
                    [this, state, completedBytes, totalBytes](
                        std::uint64_t copied, std::uint64_t fileTotal) mutable {
                        state.bytesCopied = completedBytes + copied;
                        state.totalBytes = totalBytes != 0
                            ? totalBytes
                            : completedBytes + fileTotal;
                        SetProgress(state, true);
                    };
                BatchResult one = ImportOne(items[i], request.metadata,
                                            resources, tags, callback);
                total.imported += one.imported;
                total.skipped += one.skipped;
                total.failed += one.failed;
            }
            completedBytes += fileSizes[i];
            state.bytesCopied = completedBytes;
            SetProgress(state, true);
        }
        Progress finished;
        SetProgress(finished, true);
        // WPARAM = imported; LPARAM packs skipped and failed for the UI.
        if (notifyWindow_ != nullptr && IsWindow(notifyWindow_))
        {
            PostMessageW(notifyWindow_, doneMessage_,
                         static_cast<WPARAM>(total.imported),
                         static_cast<LPARAM>(
                             (static_cast<WORD>(total.skipped) << 16) |
                             static_cast<WORD>(total.failed)));
        }
    }

    if (comInitialized)
    {
        CoUninitialize();
    }
}

}  // namespace nexus::service

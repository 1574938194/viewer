#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "database/migration.h"
#include "database/sqlite_database.h"
#include "app/settings.h"
#include "domain/resource.h"
#include "repository/resource_repository.h"
#include "repository/tag_repository.h"
#include "repository/category_repository.h"
#include "repository/actor_repository.h"
#include "service/archive_service.h"
#include "service/delete_service.h"
#include "service/import_service.h"
#include "service/search_service.h"
#include "service/scan_service.h"
#include "util/content_hash.h"

namespace {

class TempDirectory {
public:
    TempDirectory()
    {
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        path_ = std::filesystem::temp_directory_path() /
                (L"nexus-phase2-" + std::to_wstring(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TempDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

bool Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
    }
    return condition;
}

}  // namespace

int main()
{
    TempDirectory temp;
    nexus::app::Settings settings;
    settings.set_videoPlayer(L"C:\\播放器\\video \"custom\".exe");
    settings.set_imageViewer(L"");
    const std::filesystem::path settingsPath = temp.path() / L"nexus.toml";
    if (!Check(settings.Save(settingsPath), "settings save failed"))
    {
        return 1;
    }
    nexus::app::Settings loadedSettings;
    if (!Check(loadedSettings.Load(settingsPath), "settings load failed") ||
        !Check(loadedSettings.videoPlayer() ==
                   L"C:\\播放器\\video \"custom\".exe",
               "settings video player roundtrip failed") ||
        !Check(loadedSettings.imageViewer().empty(),
               "settings empty viewer roundtrip failed"))
    {
        return 1;
    }
    if (!Check(!std::filesystem::exists(settingsPath.wstring() + L".tmp-" +
                                       std::to_wstring(GetCurrentProcessId())),
               "settings temporary file was not cleaned"))
    {
        return 1;
    }
    const std::filesystem::path source = temp.path() / L"source.jpg";
    {
        std::ofstream output(source, std::ios::binary);
        output << "nexus phase 2 integration test";
    }
    const auto sourceHash = nexus::util::ComputeFileSha256(source);
    if (!Check(sourceHash.has_value(), "content hash computation failed") ||
        !Check(*sourceHash ==
                   "849c77ca000d026749db207d152167b0475e10ecec35c95bb2125783492ba88c",
               "content hash digest mismatch"))
    {
        return 1;
    }

    nexus::db::Database database;
    if (!Check(database.Open(temp.path() / L"data" / L"nexus.db"),
               "database open failed") ||
        !Check(nexus::db::Migrate(database), "schema migration failed"))
    {
        return 1;
    }
    const std::filesystem::path backupPath = temp.path() / L"backup.db";
    if (!Check(database.BackupTo(backupPath), "database backup failed"))
    {
        return 1;
    }
    if (!Check(!std::filesystem::exists(
                   backupPath.wstring() + L".tmp-" +
                   std::to_wstring(GetCurrentProcessId())),
               "database backup temporary file was not cleaned"))
    {
        return 1;
    }
    nexus::db::Database backupDatabase;
    if (!Check(backupDatabase.Open(backupPath), "backup reopen failed") ||
        !Check(nexus::db::Migrate(backupDatabase),
               "backup migration check failed") ||
        !Check(backupDatabase.user_version() >= 7,
               "backup schema version mismatch"))
    {
        return 1;
    }
    backupDatabase.Close();

    // Exercise migration from a v1 database used by existing installations.
    const std::filesystem::path legacyPath = temp.path() / L"legacy.db";
    nexus::db::Database legacy;
    const char* const legacyCategories =
        "CREATE TABLE categories(id INTEGER PRIMARY KEY,"
        " name TEXT UNIQUE NOT NULL);";
    const char* const legacyResources =
        "CREATE TABLE resources(id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " resource_type INTEGER NOT NULL, path TEXT UNIQUE NOT NULL,"
        " original_filename TEXT NOT NULL, title TEXT, code TEXT,"
        " category_id INTEGER REFERENCES categories(id),"
        " file_size INTEGER DEFAULT 0, width INTEGER DEFAULT 0,"
        " height INTEGER DEFAULT 0, duration INTEGER DEFAULT 0,"
        " added_time INTEGER DEFAULT 0, modified_time INTEGER DEFAULT 0,"
        " last_open_time INTEGER DEFAULT 0, total_open_count INTEGER DEFAULT 0,"
        " hot_score REAL DEFAULT 0, favorite INTEGER DEFAULT 0,"
        " thumbnail_key TEXT, missing INTEGER DEFAULT 0);";
    if (!Check(legacy.Open(legacyPath), "legacy database open failed") ||
        !Check(legacy.Exec(legacyCategories),
               "legacy categories setup failed") ||
        !Check(legacy.Exec(legacyResources),
               "legacy resources setup failed") ||
        !Check(legacy.Exec("PRAGMA user_version=1;"),
               "legacy version setup failed"))
    {
        return 1;
    }
    legacy.Close();
    nexus::db::Database upgraded;
    if (!Check(upgraded.Open(legacyPath), "legacy reopen failed") ||
        !Check(nexus::db::Migrate(upgraded), "legacy migration failed") ||
        !Check(upgraded.user_version() >= 7,
               "legacy migration did not reach current version"))
    {
        return 1;
    }
    nexus::db::Statement indexCheck;
    if (!Check(indexCheck.Prepare(
                   upgraded.handle(),
                   "SELECT 1 FROM sqlite_master WHERE type='index' AND "
                   "name='idx_resources_duplicate';"),
               "duplicate index query prepare failed") ||
        !Check(indexCheck.StepRow(), "duplicate index missing after migration"))
    {
        return 1;
    }
    nexus::db::Statement pathIndexCheck;
    if (!Check(pathIndexCheck.Prepare(
                   upgraded.handle(),
                   "SELECT 1 FROM sqlite_master WHERE type='index' AND "
                   "name='idx_resources_path_nocase';"),
               "path index query prepare failed") ||
        !Check(pathIndexCheck.StepRow(),
               "case-insensitive path index missing after migration"))
    {
        return 1;
    }
    nexus::db::Statement logIndexCheck;
    if (!Check(logIndexCheck.Prepare(
                   upgraded.handle(),
                   "SELECT 1 FROM sqlite_master WHERE type='index' AND "
                   "name='idx_operation_log_status_time';"),
               "operation log index query prepare failed") ||
        !Check(logIndexCheck.StepRow(),
               "operation log index missing after migration"))
    {
        return 1;
    }
    if (!Check(sqlite3_compileoption_used("ENABLE_FTS5") != 0,
               "test SQLite build does not provide required FTS5"))
    {
        return 1;
    }
    {
        nexus::db::Statement ftsCheck;
        if (!Check(ftsCheck.Prepare(
                       upgraded.handle(),
                       "SELECT 1 FROM sqlite_master WHERE type='table' AND "
                       "name='resource_fts';"),
                   "fts table query prepare failed") ||
            !Check(ftsCheck.StepRow(), "fts table missing after migration"))
        {
            return 1;
        }
    }
    upgraded.Close();

    nexus::repo::ResourceRepository resources(database);
    nexus::repo::TagRepository tags(database);
    nexus::repo::CategoryRepository categories(database);
    nexus::repo::ActorRepository actors(database);
    nexus::service::ArchiveService archive(temp.path() / L"library");

    const std::filesystem::path rollbackSource = temp.path() / L"rollback.jpg";
    {
        std::ofstream output(rollbackSource, std::ios::binary);
        output << "rollback verification";
    }
    const nexus::service::ArchiveResult rollbackArchive = archive.Archive(
        rollbackSource, 999, nexus::ResourceType::kImage, false);
    if (!Check(rollbackArchive.ok, "rollback archive setup failed") ||
        !Check(archive.Rollback(rollbackSource, rollbackArchive),
               "archive rollback failed") ||
        !Check(std::filesystem::exists(rollbackSource),
               "rollback did not restore source") ||
        !Check(!std::filesystem::exists(rollbackArchive.finalPath),
               "rollback did not remove managed target"))
    {
        return 1;
    }

    nexus::Resource resource;
    resource.type = nexus::ResourceType::kImage;
    resource.path = source;
    resource.originalFilename = "source.jpg";
    resource.title = "source";
    resource.fileSize = std::filesystem::file_size(source);
    resource.contentHash = *sourceHash;
    resource.modifiedTime = 1;

    if (!Check(database.Exec("BEGIN;"), "transaction begin failed") ||
        !Check(resources.Insert(resource), "resource insert failed"))
    {
        return 1;
    }

    const nexus::service::ArchiveResult archived = archive.Archive(
        source, resource.id, resource.type, false);
    if (!Check(archived.ok, "archive failed") ||
        !Check(std::filesystem::exists(archived.finalPath),
               "managed file does not exist") ||
        !Check(!std::filesystem::exists(source),
               "same-volume source was not moved into managed storage") ||
        !Check(resources.UpdatePath(resource.id, archived.finalPath),
               "managed path update failed") ||
        !Check(database.Exec("COMMIT;"), "transaction commit failed"))
    {
        database.Exec("ROLLBACK;");
        return 1;
    }
    if (!Check(resources.NoteOpen(resource.id), "open history write failed"))
    {
        return 1;
    }
    std::int64_t openTime = 0;
    std::uint32_t openCount = 0;
    double openScore = 0.0;
    if (!Check(resources.LoadOpenStats(resource.id, &openTime, &openCount,
                                       &openScore),
               "open statistics read failed") ||
        !Check(openTime > 0 && openCount == 1 && openScore > 0.0,
               "open statistics values were not refreshed"))
    {
        return 1;
    }
    std::wstring caseVariant = archived.finalPath.wstring();
    for (wchar_t& character : caseVariant)
    {
        if (character >= L'a' && character <= L'z')
        {
            character -= L'a' - L'A';
        }
    }
    if (!Check(resources.ExistsPath(std::filesystem::path(caseVariant)),
               "path duplicate check is case sensitive"))
    {
        return 1;
    }
    if (!Check(resources.FindDuplicate(resource.fileSize, "renamed.jpg",
                                       resource.contentHash) == resource.id,
               "content hash duplicate check failed"))
    {
        return 1;
    }
    if (!Check(tags.Attach(resource.id, "uniform"), "tag attach failed") ||
        !Check(resources.LoadAll()[0].tags.size() == 1,
               "attached tag was not loaded"))
    {
        return 1;
    }
    if (!Check(tags.FindOrCreate("Uniform") == tags.FindOrCreate("uniform"),
               "tag case normalization failed"))
    {
        return 1;
    }
    const std::filesystem::path populatedBackup = temp.path() / L"populated.db";
    if (!Check(database.BackupTo(populatedBackup), "populated backup failed"))
    {
        return 1;
    }
    nexus::db::Database populated;
    if (!Check(populated.Open(populatedBackup), "populated backup reopen failed") ||
        !Check(populated.user_version() >= 7, "populated backup schema mismatch") ||
        !Check(populated.Exec("SELECT 1;"), "populated backup query failed"))
    {
        return 1;
    }
    populated.Close();
    const std::int64_t categoryId = categories.FindOrCreate("collection");
    if (!Check(categoryId != 0, "category create failed") ||
        !Check(categories.Assign(resource.id, categoryId),
               "category assign failed") ||
        !Check(actors.Attach(resource.id, "Alice"), "actor attach failed"))
    {
        return 1;
    }
    const std::vector<nexus::Resource> loaded = resources.LoadAll();
    if (!Check(loaded.size() == 1, "persisted resource was not loaded") ||
        !Check(loaded[0].totalOpenCount == 1,
               "cached open count was not updated") ||
        !Check(loaded[0].hotScore > 0.0, "hot score was not updated") ||
        !Check(loaded[0].modifiedTime == 1,
               "modified time was not persisted"))
    {
        return 1;
    }
    const std::filesystem::path managedPath = loaded[0].path;
    std::error_code missingError;
    const auto restorePath = temp.path() / L"restore-copy.jpg";
    std::filesystem::copy_file(managedPath, restorePath,
                               std::filesystem::copy_options::overwrite_existing,
                               missingError);
    if (!Check(!missingError, "missing-state fixture copy failed") ||
        !Check(std::filesystem::remove(managedPath, missingError) &&
                   !missingError,
               "missing-state fixture removal failed") ||
        !Check(resources.RefreshMissing(), "missing-state refresh failed") ||
        !Check(resources.LoadAll()[0].missing,
               "missing state was not persisted"))
    {
        return 1;
    }
    std::filesystem::copy_file(restorePath, managedPath,
                               std::filesystem::copy_options::overwrite_existing,
                               missingError);
    if (!Check(!missingError, "missing-state fixture restore failed") ||
        !Check(resources.RefreshMissing(), "missing-state restore refresh failed") ||
        !Check(!resources.LoadAll()[0].missing,
               "missing state was not cleared"))
    {
        return 1;
    }
    std::filesystem::remove(restorePath, missingError);
    if (!Check(resources.SetFavorite(resource.id, true),
               "favorite update failed") ||
        !Check(resources.LoadAll()[0].favorite,
               "favorite value was not persisted"))
    {
        return 1;
    }
    if (!Check(resources.UpdateMetadata(resource.id, "source", true),
               "unchanged metadata update failed"))
    {
        return 1;
    }
    if (!Check(tags.SetAlias("Costume", "Uniform"), "tag alias insert failed"))
    {
        return 1;
    }
    const std::vector<nexus::Resource> searchable = resources.LoadAll();
    const auto resolve = [&tags](const std::string& word) {
        return tags.ResolveWord(word);
    };
    if (!Check(nexus::service::SearchService::Search(
                   resources, searchable,
                   L"type:image favorite:true +costume", resolve)
                       .size() == 1,
               "advanced search did not resolve filters and alias") ||
        !Check(nexus::service::SearchService::Search(
                   resources, searchable, L"-uniform", resolve)
                       .empty(),
               "excluded search term did not reject resource") ||
        !Check(nexus::service::SearchService::Search(
                   resources, searchable, L"type:video", resolve)
                       .empty(),
               "type search filter did not reject image"))
    {
        return 1;
    }
    if (!Check(nexus::service::SearchService::Search(
                   resources, searchable, L"category:collection", resolve)
                       .size() == 1,
               "category field query failed") ||
        !Check(nexus::service::SearchService::Search(
                   resources, searchable, L"tag:uniform", resolve)
                       .size() == 1,
               "tag field query failed") ||
        !Check(nexus::service::SearchService::Search(
                   resources, searchable, L"actor:alice", resolve)
                       .size() == 1,
               "actor field query failed") ||
        !Check(nexus::service::SearchService::Search(
                   resources, searchable, L"-category:collection", resolve)
                       .empty(),
               "excluded category field query failed"))
    {
        return 1;
    }
    if (!Check(nexus::service::SearchService::Search(
                   resources, searchable, L"opened:<30d", resolve)
                       .size() == 1,
               "recent-open filter failed") ||
        !Check(nexus::service::SearchService::Search(
                   resources, searchable, L"opened:>30d", resolve)
                       .empty(),
               "old-open filter failed"))
    {
        return 1;
    }
    if (!Check(nexus::service::SearchService::Search(
                   resources, searchable, L"\"source\"", resolve)
                       .size() == 1,
               "quoted phrase search failed"))
    {
        return 1;
    }
    if (!Check(searchable[0].categoryName == "collection",
               "category name was not loaded") ||
        !Check(searchable[0].actors.size() == 1,
               "actor relation was not loaded") ||
        !Check(nexus::service::SearchService::Search(resources, searchable,
                                                     L"Alice", resolve)
                       .size() == 1,
               "actor search failed"))
    {
        return 1;
    }
    const std::vector<nexus::ResourceId> ftsMatches =
        resources.SearchFts("source");
    if (!Check(ftsMatches.size() == 1 && ftsMatches[0] == resource.id,
               "fts search did not return the resource"))
    {
        return 1;
    }
    if (!Check(nexus::service::SearchService::Search(
                   resources, searchable, L"-uniform", resolve)
                       .empty(),
               "exclude-only query returned incorrect resources") ||
        !Check(nexus::service::SearchService::Search(
                   resources, searchable, L"type:video type:image", resolve)
                       .empty(),
               "conflicting type filters were not rejected"))
    {
        return 1;
    }

    std::wstring error;
    if (!Check(nexus::service::DeleteResource(
                   nullptr, archived.finalPath,
                   nexus::service::DeleteMode::kPermanent, &error),
               "managed file delete failed") ||
        !Check(!std::filesystem::exists(archived.finalPath),
               "managed file still exists after delete") ||
        !Check(resources.Delete(resource.id), "resource delete failed") ||
        !Check(resources.Count() == 0, "resource row still exists"))
    {
        return 1;
    }

    const std::filesystem::path importRoot = temp.path() / L"import-worker";
    const std::filesystem::path importDatabasePath =
        importRoot / L"data" / L"nexus.db";
    nexus::db::Database importDatabase;
    if (!Check(importDatabase.Open(importDatabasePath),
               "import database open failed") ||
        !Check(nexus::db::Migrate(importDatabase),
               "import database migration failed"))
    {
        return 1;
    }
    nexus::repo::ResourceRepository importedResources(importDatabase);
    const std::filesystem::path importSource = importRoot / L"ABC-123-test.jpg";
    std::filesystem::create_directories(importSource.parent_path());
    {
        std::ofstream output(importSource, std::ios::binary);
        output << "import worker fixture";
    }
    nexus::service::ArchiveService importArchive(importRoot / L"library");
    nexus::service::ImportService importer(importDatabasePath, importArchive);
    nexus::service::ImportMetadata metadata;
    metadata.title = "custom title";
    metadata.category = "photos";
    metadata.tags = {"manual-tag"};
    metadata.actors = {"Bob"};
    metadata.favorite = true;
    metadata.overrideTitle = true;
    importer.Start(nullptr, 0);
    importer.Import({importSource}, metadata);
    for (int attempt = 0; attempt < 100 && importedResources.Count() == 0;
         ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    importer.Shutdown();
    const std::vector<nexus::Resource> imported = importedResources.LoadAll();
    if (!Check(imported.size() == 1, "worker import did not persist resource") ||
        !Check(imported[0].title == "custom title",
               "import title metadata was not persisted") ||
        !Check(imported[0].categoryName == "photos",
               "import category metadata was not persisted") ||
        !Check(imported[0].favorite,
               "import favorite metadata was not persisted") ||
        !Check(imported[0].actors.size() == 1 &&
                   imported[0].actors[0] == "Bob",
               "import actor metadata was not persisted") ||
        !Check(std::find(imported[0].tags.begin(), imported[0].tags.end(),
                         "manual-tag") != imported[0].tags.end(),
               "import tag metadata was not persisted") ||
        !Check(imported[0].contentHash ==
                   "4f13e67904755345923f3f22278a1e64893d2ff8b44a890164309af8a1f15b74",
               "worker import content hash was not persisted") ||
        !Check(std::filesystem::exists(imported[0].path),
               "worker import did not create managed file"))
    {
        return 1;
    }
    const std::filesystem::path duplicateSource =
        importRoot / L"duplicate" / L"different-name.jpg";
    std::filesystem::create_directories(duplicateSource.parent_path());
    {
        std::ofstream output(duplicateSource, std::ios::binary);
        output << "import worker fixture";
    }
    nexus::service::ImportService duplicateImporter(importDatabasePath,
                                                     importArchive);
    duplicateImporter.Start(nullptr, 0);
    duplicateImporter.Import({duplicateSource});
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    duplicateImporter.Shutdown();
    if (!Check(importedResources.Count() == 1,
               "duplicate import created an extra resource") ||
        !Check(std::filesystem::exists(duplicateSource),
               "duplicate import removed source file"))
    {
        return 1;
    }

    std::vector<nexus::service::DeleteItem> pendingDeletes;
    for (int index = 0; index < 2; ++index)
    {
        const std::filesystem::path path =
            importRoot / (L"delete-" + std::to_wstring(index) + L".jpg");
        {
            std::ofstream output(path, std::ios::binary);
            output << "delete fixture";
        }
        nexus::Resource deleteResource;
        deleteResource.type = nexus::ResourceType::kImage;
        deleteResource.path = path;
        deleteResource.originalFilename = "delete.jpg";
        deleteResource.title = "delete";
        if (!Check(importedResources.Insert(deleteResource),
                   "batch delete fixture insert failed"))
        {
            return 1;
        }
        pendingDeletes.push_back({deleteResource.id, path});
    }
    nexus::service::DeleteService deleteService(importDatabasePath);
    deleteService.Start(nullptr, 0);
    deleteService.Delete(pendingDeletes,
                         nexus::service::DeleteMode::kPermanent);
    std::vector<nexus::service::DeleteBatchResult> deleteResults;
    for (int attempt = 0; attempt < 100 && deleteResults.empty(); ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        deleteResults = deleteService.TakeCompleted();
    }
    deleteService.Shutdown();
    if (!Check(deleteResults.size() == 1,
               "batch delete did not publish completion") ||
        !Check(deleteResults[0].deleted.size() == 2 &&
                   deleteResults[0].failed == 0,
               "batch delete result counts are incorrect") ||
        !Check(importedResources.Count() == 1,
               "batch delete removed wrong database rows") ||
        !Check(!std::filesystem::exists(pendingDeletes[0].path) &&
                   !std::filesystem::exists(pendingDeletes[1].path),
               "batch delete left files on disk"))
    {
        return 1;
    }

    const std::filesystem::path scanRoot = temp.path() / L"scan-library";
    const std::filesystem::path scanDatabasePath =
        temp.path() / L"scan-data" / L"nexus.db";
    nexus::db::Database scanDatabase;
    if (!Check(scanDatabase.Open(scanDatabasePath),
               "scan database open failed") ||
        !Check(nexus::db::Migrate(scanDatabase),
               "scan database migration failed"))
    {
        return 1;
    }
    if (!Check(scanDatabase.user_version() >= 7,
               "schema migration did not install current version"))
    {
        return 1;
    }
    const std::filesystem::path orphan = scanRoot / L"image" / L"00" /
                                         L"000000000099.jpg";
    const std::filesystem::path incoming = scanRoot / L".incoming" /
                                           L"pending.jpg";
    const std::filesystem::path nestedIncoming =
        scanRoot / L".incoming" / L"nested" / L"pending-2.jpg";
    std::filesystem::create_directories(orphan.parent_path());
    std::filesystem::create_directories(incoming.parent_path());
    std::filesystem::create_directories(nestedIncoming.parent_path());
    {
        std::ofstream output(orphan, std::ios::binary);
        output << "orphan";
        std::ofstream pending(incoming, std::ios::binary);
        pending << "pending";
        std::ofstream pendingNested(nestedIncoming, std::ios::binary);
        pendingNested << "pending-2";
    }
    const nexus::service::ScanResult scan =
        nexus::service::ScanService::Run(scanDatabasePath, scanRoot);
    nexus::repo::ResourceRepository scannedResources(scanDatabase);
    const std::vector<nexus::Resource> scanned = scannedResources.LoadAll();
    if (!Check(scan.added == 1, "scan did not add orphan resource") ||
        !Check(scan.incoming == 2,
               "scan did not report pending incoming files") ||
        !Check(scanned.size() == 1 && scanned[0].originalFilename ==
                   "000000000099.jpg",
               "scan added incorrect resource") ||
        !Check(scanned.size() == 1 && scanned[0].contentHash ==
                   "88f6811ab5d8fc6d3177f9b7609ae0fcebfda187e5046b62d38bb539e88b74d7",
               "scan content hash was not persisted") ||
        !Check(std::filesystem::exists(incoming) &&
                   std::filesystem::exists(nestedIncoming),
               "scan modified incoming directory"))
    {
        return 1;
    }
    {
        std::ofstream output(orphan, std::ios::binary | std::ios::app);
        output << "-changed";
    }
    const nexus::service::ScanResult rescanned =
        nexus::service::ScanService::Run(scanDatabasePath, scanRoot);
    const std::vector<nexus::Resource> rescannedResources =
        scannedResources.LoadAll();
    if (!Check(rescanned.updated == 1,
               "incremental scan did not report changed resource") ||
        !Check(rescannedResources.size() == 1 &&
                   rescannedResources[0].fileSize > scanned[0].fileSize &&
                   rescannedResources[0].contentHash != scanned[0].contentHash,
               "incremental scan did not refresh file metadata"))
    {
        return 1;
    }
    std::filesystem::remove(orphan);
    if (!Check(scannedResources.RefreshMissing(),
               "scan missing refresh failed") ||
        !Check(scannedResources.LoadAll()[0].missing,
               "scan did not mark missing resource"))
    {
        return 1;
    }
    if (!Check(!scannedResources.Delete(999999),
               "delete reported success for missing resource"))
    {
        return 1;
    }
    const nexus::service::ScanResult emptyScan =
        nexus::service::ScanService::Run(
            temp.path() / L"empty-scan.db", temp.path() / L"not-created-library");
    if (!Check(emptyScan.failed == 0 && emptyScan.added == 0,
               "scan treated an absent library as an error"))
    {
        return 1;
    }

    std::cout << "archive/import/metadata/search/batch-delete integration passed\n";
    return 0;
}

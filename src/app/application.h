#ifndef NEXUS_SRC_APP_APPLICATION_H_
#define NEXUS_SRC_APP_APPLICATION_H_

#include <d2d1.h>

#include <memory>
#include <atomic>
#include <thread>
#include <string>
#include <unordered_set>
#include <vector>

#include "database/sqlite_database.h"
#include "app/settings.h"
#include "domain/resource.h"
#include "render/renderer.h"
#include "repository/resource_repository.h"
#include "repository/tag_repository.h"
#include "service/archive_service.h"
#include "service/delete_service.h"
#include "service/import_service.h"
#include "service/playback_service.h"
#include "service/thumbnail_service.h"
#include "ui/resource_grid.h"
#include "ui/search_bar.h"

namespace nexus::app {

// Owns the Win32 main window, the renderer and all phase-2 services. Data
// lives in SQLite; the in-memory vector is a read cache refreshed after
// imports, deletes and opens.
class Application
{
public:
    static int Run(const wchar_t* commandLine);

private:
    enum class SortMode
    {
        kRelevance,
        kRecentOpen,
        kHot,
        kAdded,
    };
    Application();
    ~Application() = default;
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    bool Init(const std::wstring& initialQuery);

    void Shutdown();

    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

    void DrawFrame();

    void ReloadFromDatabase();

    void SeedEmptyLibrary();

    void Refilter();

    void ClampScroll();

    int SelectedIndex() const;

    int ColumnCount() const;

    void MoveSelection(int delta);

    void OpenResource(Resource* item);

    void DeleteSelected(bool permanent);

    void ToggleSelectedFavorite();

    void EditSelectedMetadata();

    void MissingWorkerLoop();

    void ChooseSortMode();

    void ToggleMaximize();

    void SetHeaderHover(int index);

    void UpdateHeaderHover(POINT point);

    void DrawCaptionButtons(float width, float topHeight);

    void LoadSideTags();

    D2D1_RECT_F SidebarRect() const;

    D2D1_RECT_F SettingsRect() const;

    void DrawSidebar();

    int HitTestSideTag(POINT point) const;

    void ClampSideScroll();

    void ShowSettings();

    void Invalidate();

    D2D1_RECT_F ContentRect() const;

    D2D1_RECT_F SearchRect() const;

    D2D1_RECT_F SortRect() const;

    ui::GridView MakeView(D2D1_RECT_F content) const;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    static constexpr UINT kThumbReadyMsg = WM_APP + 1;
    static constexpr UINT kImportDoneMsg = WM_APP + 2;
    static constexpr UINT kImportProgressMsg = WM_APP + 5;
    static constexpr UINT_PTR kCaretTimer = 1;
    static constexpr UINT kMenuOpen = 40001;
    static constexpr UINT kMenuFavorite = 40002;
    static constexpr UINT kMenuDelete = 40003;
    static constexpr UINT kMenuEditMetadata = 40004;
    static constexpr UINT kMenuBrowse = 40005;
    static constexpr UINT kMissingUpdatedMsg = WM_APP + 3;
    static constexpr UINT kDeleteDoneMsg = WM_APP + 4;

    HWND hwnd_ = nullptr;
    bool comInitialized_ = false;
    float scale_ = 1.0f;
    float scrollY_ = 0.0f;
    ResourceId hoveredId_ = 0;
    ResourceId selectedId_ = 0;
    std::unordered_set<ResourceId> selectedIds_;
    SortMode sortMode_ = SortMode::kRelevance;
    int headerHover_ = -1;
    bool trackingMouse_ = false;
    std::vector<repo::TagCount> sideTags_;
    std::string tagFilter_;
    float sideScrollY_ = 0.0f;
    int sideHover_ = -1;
    std::thread missingWorker_;
    std::atomic<bool> stopMissingWorker_ = false;

    std::filesystem::path exeDir_;
    std::filesystem::path libraryRoot_;

    db::Database database_;
    Settings settings_;
    repo::ResourceRepository resources_;
    repo::TagRepository tags_;
    service::ArchiveService archive_;
    service::ImportService importer_;
    service::DeleteService deleter_;

    render::Renderer renderer_;
    ui::SearchBar searchBar_;
    service::ThumbnailService thumbnails_;
    service::PlaybackService playback_;

    std::vector<Resource> items_;
    std::vector<const Resource*> filtered_;
    service::ImportService::Progress importProgress_;
};

}  // namespace nexus::app

#endif  // NEXUS_SRC_APP_APPLICATION_H_

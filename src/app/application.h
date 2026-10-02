#ifndef NEXUS_SRC_APP_APPLICATION_H_
#define NEXUS_SRC_APP_APPLICATION_H_

#include <d2d1.h>

#include <array>
#include <memory>
#include <atomic>
#include <filesystem>
#include <optional>
#include <thread>
#include <string>
#include <unordered_set>
#include <unordered_map>
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
#include "ui/resource_order.h"
#include "ui/selection_drag.h"
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
    friend class ApplicationInteractionTest;

    using SortMode = ui::ResourceSort;

    enum class SidebarFilter
    {
        kAll,
        kVideo,
        kImage,
        kFavorite,
        kBrowse,
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

    void ApplyResourceOrder(bool reset = false);

    void UpdateDragSelection(POINT pointer);

    void EndDragSelection(bool cancel);

    void UpdateSelectionTimer();

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

    // Rebuilds the contents of the Browse tab from the current directory.
    // Only direct children are materialized; folders are represented as
    // navigable cards and media rows reuse their database metadata when it is
    // already indexed.
    void ReloadBrowseDirectory();

    void ClearCacheAndRebuild();

    void UpdateSidebarCounts();

    D2D1_RECT_F SidebarRect() const;

    D2D1_RECT_F SettingsRect() const;

    void DrawSidebar();

    int HitTestSidebar(POINT point) const;

    void ClampSideScroll();

    void ShowSettings();

    // Starts (or restarts) the non-blocking managed-library indexing and
    // cache warm-up pass.  The worker only handles bounded pages and reports
    // UI refreshes through the library-scan messages.
    void StartBackgroundBuild();

    void Invalidate();

    D2D1_RECT_F ContentRect() const;

    std::optional<D2D1_RECT_F> CardScrollThumbRect() const;

    void UpdateCardScrollFromPointer(float pointerY);

    D2D1_RECT_F SearchRect() const;

    D2D1_RECT_F SortRect() const;

    ui::GridView MakeView(D2D1_RECT_F content) const;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    static constexpr UINT kThumbReadyMsg = WM_APP + 1;
    static constexpr UINT kImportDoneMsg = WM_APP + 2;
    static constexpr UINT kImportProgressMsg = WM_APP + 5;
    static constexpr UINT_PTR kCaretTimer = 1;
    static constexpr UINT_PTR kSelectionTimer = 2;
    static constexpr UINT kMenuOpen = 40001;
    static constexpr UINT kMenuFavorite = 40002;
    static constexpr UINT kMenuDelete = 40003;
    static constexpr UINT kMenuEditMetadata = 40004;
    static constexpr UINT kMenuBrowse = 40005;
    static constexpr UINT kMenuNewFolder = 40006;
    static constexpr UINT kMissingUpdatedMsg = WM_APP + 3;
    static constexpr UINT kDeleteDoneMsg = WM_APP + 4;
    static constexpr UINT kSeedMsg = WM_APP + 6;
    static constexpr UINT kLibraryScanDoneMsg = WM_APP + 7;
    static constexpr UINT kLibraryScanReadyMsg = WM_APP + 8;

    HWND hwnd_ = nullptr;
    bool comInitialized_ = false;
    float scale_ = 1.0f;
    float scrollY_ = 0.0f;
    ResourceId hoveredId_ = 0;
    ResourceId selectedId_ = 0;
    std::unordered_set<ResourceId> selectedIds_;
    SortMode sortMode_ = SortMode::kHot;
    ui::ResourceOrder libraryOrder_;
    std::wstring orderQuery_;
    struct BrowseOrder {
        ui::ResourceOrder order;
        std::wstring query;
    };
    std::unordered_map<std::string, BrowseOrder> browseOrders_;
    int headerHover_ = -1;
    bool trackingMouse_ = false;
    ui::SelectionDrag selectionDrag_;
    bool selectionTimerRunning_ = false;
    bool draggingCardScroll_ = false;
    float cardScrollDragOffset_ = 0.0f;
    std::vector<repo::TagCount> sideTags_;
    std::array<std::size_t, 5> sidebarCounts_ = {};
    std::string tagFilter_;
    SidebarFilter sidebarFilter_ = SidebarFilter::kAll;
    float sideScrollY_ = 0.0f;
    int sideHover_ = -1;
    bool navigationExpanded_ = true;
    bool tagsExpanded_ = true;
    std::thread missingWorker_;
    std::thread seedWorker_;
    std::thread libraryScanWorker_;
    std::atomic<bool> stopMissingWorker_ = false;
    std::atomic<bool> stopLibraryScanWorker_ = false;

    std::filesystem::path exeDir_;
    std::filesystem::path libraryRoot_;
    std::filesystem::path browsePath_;
    std::vector<Resource> browseItems_;

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
    std::wstring statusMessage_;
};

}  // namespace nexus::app

#endif  // NEXUS_SRC_APP_APPLICATION_H_

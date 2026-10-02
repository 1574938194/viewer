#include "app/application.h"

#include <Windows.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

#include "app/icon_resource.h"
#include "database/migration.h"
#include "domain/sample_assets.h"
#include "render/theme.h"
#include "repository/category_repository.h"
#include "repository/actor_repository.h"
#include "service/delete_service.h"
#include "service/media_probe.h"
#include "service/search_service.h"
#include "service/scan_service.h"
#include "ui/import_dialog.h"
#include "ui/delete_dialog.h"
#include "ui/settings_dialog.h"
#include "util/string_util.h"
#include "util/tag_values.h"

namespace nexus::app {

namespace {

constexpr COLORREF kMenuBack = RGB(32, 32, 32);
constexpr COLORREF kMenuCard = RGB(37, 37, 37);
constexpr COLORREF kMenuHover = RGB(48, 48, 48);
constexpr COLORREF kMenuBorder = RGB(53, 53, 53);
constexpr COLORREF kMenuAccent = RGB(91, 141, 190);
constexpr COLORREF kMenuText = RGB(229, 229, 229);

HFONT MenuFont()
{
    static HFONT font = nullptr;
    if (font == nullptr)
    {
        NONCLIENTMETRICSW metrics = {sizeof(metrics)};
        LOGFONTW description = {};
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS,
                                   sizeof(metrics), &metrics, 0))
        {
            description = metrics.lfMenuFont;
        }
        else
        {
            description.lfHeight = -16;
            wcscpy_s(description.lfFaceName, L"Segoe UI");
        }
        description.lfHeight = description.lfHeight >= 0
            ? std::max<LONG>(18, description.lfHeight * 3 / 2)
            : std::min<LONG>(-18, description.lfHeight * 3 / 2);
        description.lfWeight = FW_NORMAL;
        font = CreateFontIndirectW(&description);
    }
    return font;
}

void FillMenuRoundRect(HDC dc, const RECT& rect, COLORREF color, int radius)
{
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2,
              radius * 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(brush);
}

void FrameMenuRoundRect(HDC dc, const RECT& rect, COLORREF color, int radius)
{
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2,
              radius * 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
}

void AddDarkMenuItem(HMENU menu, UINT id, const wchar_t* text)
{
    MENUITEMINFOW item = {sizeof(item)};
    item.fMask = MIIM_ID | MIIM_FTYPE | MIIM_DATA;
    item.fType = MFT_OWNERDRAW;
    item.wID = id;
    item.dwItemData = reinterpret_cast<ULONG_PTR>(text);
    InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &item);
}

void AddDarkMenuSeparator(HMENU menu)
{
    MENUITEMINFOW item = {sizeof(item)};
    item.fMask = MIIM_FTYPE | MIIM_DATA;
    item.fType = MFT_OWNERDRAW;
    item.dwItemData = 0;
    InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &item);
}

void BrowseInExplorer(const std::filesystem::path& path)
{
    PIDLIST_ABSOLUTE item = ILCreateFromPathW(path.c_str());
    if (item != nullptr)
    {
        PIDLIST_ABSOLUTE folder = ILClone(item);
        if (folder != nullptr && ILRemoveLastID(folder))
        {
            PCUITEMID_CHILD child = ILFindLastID(item);
            SHOpenFolderAndSelectItems(folder, 1, &child, 0);
            ILFree(folder);
            ILFree(item);
            return;
        }
        if (folder != nullptr)
        {
            ILFree(folder);
        }
        ILFree(item);
    }
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty())
    {
        ShellExecuteW(nullptr, L"open", parent.c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
    }
}

constexpr float kCaptionBarDp = 44.0f;
constexpr float kTopBarDp = 50.0f;
constexpr float kStatusBarDp = 26.0f;
constexpr float kGapYDp = 14.0f;
constexpr float kPaddingDp = 16.0f;
constexpr float kCaptionButtonDp = 46.0f;
constexpr float kSortWidthDp = 220.0f;
constexpr float kSidebarWidthDp = 236.0f;
constexpr float kSideItemHeightDp = 36.0f;
constexpr float kSideListTopDp = 44.0f;
constexpr float kSideSectionGapDp = 12.0f;
constexpr int kSidebarNavigationCount = 5;
constexpr int kBrowseNavigationIndex = 4;
constexpr int kSidebarNavigationHeader = -2;
constexpr int kSidebarTagsHeader = -3;

// `vr "白丝 3p"` arrives as a raw command line; the shell quotes are not
// part of the query itself.
std::wstring NormalizeQuery(const wchar_t* commandLine)
{
    std::wstring query = commandLine != nullptr ? commandLine : L"";
    auto trim = [](std::wstring& text) {
        while (!text.empty() && iswspace(text.front()))
        {
            text.erase(text.begin());
        }
        while (!text.empty() && iswspace(text.back()))
        {
            text.pop_back();
        }
    };
    trim(query);
    while (query.size() >= 2 && query.front() == L'"' && query.back() == L'"')
    {
        query = query.substr(1, query.size() - 2);
        trim(query);
    }
    return query;
}

std::string CsvField(const std::string& value)
{
    std::string escaped = "\"";
    for (char character : value)
    {
        if (character == '"')
        {
            escaped += "\"\"";
        }
        else
        {
            escaped.push_back(character);
        }
    }
    escaped.push_back('"');
    return escaped;
}

std::filesystem::path ModuleDir()
{
    wchar_t buffer[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path();
}

std::vector<std::string> BuildQuickTags(
    const Settings& settings, repo::TagRepository& tags)
{
    std::vector<std::string> quickTags = settings.presetTags();
    for (const std::string& tag : tags.RecentTags(10))
    {
        if (std::find(quickTags.begin(), quickTags.end(), tag) ==
            quickTags.end())
        {
            quickTags.push_back(tag);
        }
    }
    return UniqueTags(quickTags);
}

std::string BrowsePathKey(const std::filesystem::path& path)
{
    return AsciiLower(WideToUtf8(path.lexically_normal().wstring()));
}

bool BrowsePathsEqual(const std::filesystem::path& left,
                      const std::filesystem::path& right)
{
    return BrowsePathKey(left) == BrowsePathKey(right);
}

ResourceId BrowseSyntheticId(const std::filesystem::path& path)
{
    // Keep generated browse-only rows outside SQLite's AUTOINCREMENT range.
    // FNV-1a is deterministic so the same folder keeps its identity while
    // the user is navigating the current directory.
    const std::string key = BrowsePathKey(path);
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char character : key)
    {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    return (static_cast<ResourceId>(1) << 63) | (hash & 0x7fffffffffffffffULL);
}

bool BrowseTextMatches(const Resource& resource, const std::wstring& query)
{
    if (query.empty())
    {
        return true;
    }
    const std::string term = AsciiLower(WideToUtf8(query));
    if (term.empty())
    {
        return true;
    }
    const auto contains = [&term](const std::string& value) {
        return AsciiLower(value).find(term) != std::string::npos;
    };
    if (contains(resource.originalFilename) || contains(resource.title) ||
        contains(resource.code) || contains(resource.categoryName))
    {
        return true;
    }
    for (const std::string& tag : resource.tags)
    {
        if (contains(tag))
        {
            return true;
        }
    }
    return false;
}

int RunMaintenance(const std::wstring& command)
{
    const std::filesystem::path exeDir = ModuleDir();
    Settings settings;
    settings.Load(exeDir / L"nexus.toml");
    const std::filesystem::path libraryRoot =
        settings.managedLibrary().empty()
            ? exeDir / L"NexusLibrary"
            : std::filesystem::path(settings.managedLibrary());
    db::Database database;
    const std::filesystem::path databasePath =
        exeDir / L"data" / L"nexus.db";
    if (!database.Open(databasePath))
    {
        MessageBoxW(nullptr, L"无法打开数据库。", L"Viewer", MB_ICONERROR);
        return 1;
    }
    if (!db::Migrate(database))
    {
        MessageBoxW(nullptr,
                    L"数据库迁移失败。Viewer 需要带 FTS5 扩展的 SQLite。",
                    L"Viewer", MB_ICONERROR);
        return 1;
    }
    if (command == L"--backup")
    {
        const std::time_t now = std::time(nullptr);
        std::tm localTime = {};
        localtime_s(&localTime, &now);
        wchar_t name[64] = {};
        swprintf(name, 64, L"viewer-backup-%04d%02d%02d-%02d%02d%02d.db",
                 localTime.tm_year + 1900, localTime.tm_mon + 1,
                 localTime.tm_mday, localTime.tm_hour, localTime.tm_min,
                 localTime.tm_sec);
        const std::filesystem::path backupPath = exeDir / L"data" / name;
        if (!database.BackupTo(backupPath))
        {
            MessageBoxW(nullptr, L"数据库备份失败。", L"Viewer", MB_ICONERROR);
            return 1;
        }
        MessageBoxW(nullptr, (L"数据库已备份到：\n" +
                              backupPath.wstring())
                                 .c_str(),
                     L"Viewer", MB_ICONINFORMATION);
        return 0;
    }
    if (command == L"--scan")
    {
        const service::ScanResult scan = service::ScanService::Run(
            databasePath, libraryRoot);
        MessageBoxW(nullptr,
                    (L"扫描完成。新增 " + std::to_wstring(scan.added) +
                     L"，跳过 " + std::to_wstring(scan.skipped) +
                     L"，更新 " + std::to_wstring(scan.updated) +
                     L"，失败 " + std::to_wstring(scan.failed) +
                     L"，状态更新 " +
                     std::to_wstring(scan.missingUpdated) +
                     L"，待处理临时文件 " + std::to_wstring(scan.incoming))
                        .c_str(),
                    L"Viewer", scan.failed > 0 ? MB_ICONWARNING
                                               : MB_ICONINFORMATION);
        return scan.failed > 0 ? 1 : 0;
    }
    if (command == L"--recover")
    {
        const service::ScanResult scan = service::ScanService::Run(
            databasePath, libraryRoot);
        MessageBoxW(nullptr,
                    (L"恢复完成。新增 " + std::to_wstring(scan.added) +
                     L"，更新 " + std::to_wstring(scan.updated) +
                     L"，缺失状态更新 " +
                     std::to_wstring(scan.missingUpdated) +
                     L"，待处理临时文件 " +
                     std::to_wstring(scan.incoming) +
                     L"。\n临时文件已保留，需人工检查。")
                        .c_str(),
                    L"Viewer", scan.failed > 0 ? MB_ICONWARNING
                                               : MB_ICONINFORMATION);
        return scan.failed > 0 ? 1 : 0;
    }
    if (command == L"--export")
    {
        const std::filesystem::path exportPath = exeDir / L"viewer-export.csv";
        const std::filesystem::path temporary =
            exportPath.wstring() + L".tmp-" +
            std::to_wstring(GetCurrentProcessId());
        std::ofstream output(temporary,
                             std::ios::binary | std::ios::trunc);
        if (!output)
        {
            MessageBoxW(nullptr, L"Metadata 导出失败。", L"Viewer",
                        MB_ICONERROR);
            return 1;
        }
        repo::ResourceRepository resourcesRepository(database);
        output << "id,type,path,original_filename,title,code,category,tags,"
                  "actors,favorite,missing,added_time,modified_time,content_hash\n";
        for (const Resource& resource : resourcesRepository.LoadAll())
        {
            auto Join = [](const std::vector<std::string>& values) {
                std::string result;
                for (size_t i = 0; i < values.size(); ++i)
                {
                    if (i != 0)
                    {
                        result += "|";
                    }
                    result += values[i];
                }
                return result;
            };
            output << resource.id << ','
                   << (resource.type == ResourceType::kVideo ? "video"
                                                               : "image")
                   << ',' << CsvField(WideToUtf8(resource.path.wstring()))
                   << ',' << CsvField(resource.originalFilename) << ','
                   << CsvField(resource.title) << ',' << CsvField(resource.code)
                   << ',' << CsvField(resource.categoryName) << ','
                   << CsvField(Join(resource.tags)) << ','
                   << CsvField(Join(resource.actors)) << ','
                   << (resource.favorite ? 1 : 0) << ','
                   << (resource.missing ? 1 : 0) << ',' << resource.addedTime
                   << ',' << resource.modifiedTime << ','
                   << CsvField(resource.contentHash) << '\n';
        }
        if (!output.good())
        {
            output.close();
            std::error_code cleanupError;
            std::filesystem::remove(temporary, cleanupError);
            MessageBoxW(nullptr, L"Metadata 导出失败。", L"Viewer",
                        MB_ICONERROR);
            return 1;
        }
        output.close();
        if (!MoveFileExW(temporary.c_str(), exportPath.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            std::error_code cleanupError;
            std::filesystem::remove(temporary, cleanupError);
            MessageBoxW(nullptr, L"Metadata 导出失败。", L"Viewer",
                        MB_ICONERROR);
            return 1;
        }
        MessageBoxW(nullptr,
                    (L"Metadata 已导出到：\n" +
                     exportPath.wstring())
                        .c_str(),
                    L"Viewer", MB_ICONINFORMATION);
        return 0;
    }

    db::Statement statement;
    std::int64_t resources = 0;
    std::int64_t videos = 0;
    std::int64_t images = 0;
    std::int64_t missing = 0;
    std::int64_t tags = 0;
    std::int64_t actors = 0;
    std::int64_t categories = 0;
    if (statement.Prepare(
            database.handle(),
            "SELECT COUNT(*),"
            " SUM(CASE WHEN resource_type=0 THEN 1 ELSE 0 END),"
            " SUM(CASE WHEN resource_type=1 THEN 1 ELSE 0 END),"
            " SUM(CASE WHEN missing!=0 THEN 1 ELSE 0 END)"
            " FROM resources;") &&
        statement.StepRow())
    {
        resources = statement.ColumnInt(0);
        videos = statement.ColumnInt(1);
        images = statement.ColumnInt(2);
        missing = statement.ColumnInt(3);
    }
    auto CountTable = [&database](const char* table) {
        db::Statement count;
        std::string sql = "SELECT COUNT(*) FROM " + std::string(table) + ";";
        return count.Prepare(database.handle(), sql.c_str()) && count.StepRow()
                   ? count.ColumnInt(0)
                   : static_cast<std::int64_t>(0);
    };
    tags = CountTable("tags");
    actors = CountTable("actors");
    categories = CountTable("categories");
    MessageBoxW(nullptr,
                (L"资源数：" + std::to_wstring(resources) +
                 L"\n视频：" + std::to_wstring(videos) +
                 L"\n图片：" + std::to_wstring(images) +
                 L"\n标签：" + std::to_wstring(tags) +
                 L"\n演员：" + std::to_wstring(actors) +
                 L"\n分类：" + std::to_wstring(categories) +
                 L"\n缺失：" + std::to_wstring(missing) +
                 L"\n数据库：" + databasePath.wstring())
                    .c_str(),
                L"Viewer", MB_ICONINFORMATION);
    return 0;
}

}  // namespace

// Modal popup list drawn with the shared D2D renderer so the sort selector
// matches the main window palette instead of the system menu colors.
class SortPopup
{
public:
    static int Show(HWND owner, const POINT& anchor, float scale, int current)
    {
        SortPopup popup;
        return popup.Run(owner, anchor, scale, current);
    }

private:
    static constexpr int kItemCount = 6;

    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wp,
                                 LPARAM lp)
    {
        auto* popup = reinterpret_cast<SortPopup*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
            popup = static_cast<SortPopup*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(popup));
        }
        if (popup == nullptr)
        {
            return DefWindowProcW(hwnd, message, wp, lp);
        }
        return popup->Handle(hwnd, message, wp, lp);
    }

    int Run(HWND owner, const POINT& anchor, float scale, int current)
    {
        static bool registered = false;
        if (!registered)
        {
            WNDCLASSW windowClass = {};
            windowClass.lpfnWndProc = Proc;
            windowClass.hInstance = GetModuleHandleW(nullptr);
            windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            windowClass.hbrBackground = nullptr;
            windowClass.lpszClassName = L"NexusSortPopup";
            registered = RegisterClassW(&windowClass) != 0 ||
                         GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        }
        if (!registered)
        {
            return -1;
        }
        scale_ = scale;
        current_ = current;
        hover_ = -1;
        selected_ = -1;
        done_ = false;
        width_ = 240.0f * scale;
        rowHeight_ = 54.0f * scale;
        pad_ = 8.0f * scale;
        height_ = 2.0f * pad_ + kItemCount * rowHeight_;

        HMONITOR monitor =
            MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info = {sizeof(info)};
        GetMonitorInfoW(monitor, &info);
        const int x = std::clamp(anchor.x, info.rcWork.left + 4,
                                 std::max(info.rcWork.left + 4,
                                          info.rcWork.right -
                                              static_cast<int>(width_) - 4));
        const int y = std::clamp(anchor.y, info.rcWork.top + 4,
                                 std::max(info.rcWork.top + 4,
                                          info.rcWork.bottom -
                                              static_cast<int>(height_) - 4));

        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"NexusSortPopup", L"",
                                WS_POPUP, x, y, static_cast<int>(width_),
                                static_cast<int>(height_), owner, nullptr,
                                GetModuleHandleW(nullptr), this);
        if (hwnd_ == nullptr || !renderer_.Init(hwnd_, scale))
        {
            if (hwnd_ != nullptr)
            {
                DestroyWindow(hwnd_);
                hwnd_ = nullptr;
            }
            return -1;
        }
        // Activate the popup explicitly.  SW_SHOWNA leaves a WS_POPUP without
        // keyboard/mouse activation on some Windows builds, which causes the
        // first click to be delivered to the owner instead of selecting a
        // sort row.
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);
        MSG message = {};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        renderer_.Shutdown();
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        SetForegroundWindow(owner);
        SetFocus(owner);
        return selected_;
    }

    LRESULT Handle(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
    {
        switch (message)
        {
        case WM_PAINT:
        {
            PAINTSTRUCT ps = {};
            BeginPaint(hwnd, &ps);
            Draw();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_MOUSEMOVE:
        {
            const int row = RowFromPoint(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (row != hover_)
            {
                hover_ = row;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (!tracking_)
            {
                TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
                tracking_ = TrackMouseEvent(&track) != FALSE;
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_ = false;
            hover_ = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_ACTIVATE;
        case WM_LBUTTONDOWN:
        {
            const int row = RowFromPoint(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (row >= 0)
            {
                selected_ = row;
                done_ = true;
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE)
            {
                selected_ = -1;
                done_ = true;
            }
            else if (wp == VK_UP || wp == VK_DOWN)
            {
                const int direction = wp == VK_UP ? -1 : 1;
                const int base = hover_ >= 0 ? hover_ : current_;
                hover_ = std::clamp(base + direction, 0, kItemCount - 1);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            else if (wp == VK_RETURN)
            {
                selected_ = hover_ >= 0 ? hover_ : current_;
                done_ = true;
            }
            return 0;
        case WM_CLOSE:
            selected_ = -1;
            done_ = true;
            return 0;
        case WM_KILLFOCUS:
            selected_ = -1;
            done_ = true;
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wp, lp);
        }
    }

    int RowFromPoint(LONG x, LONG y) const
    {
        const float fx = static_cast<float>(x);
        const float fy = static_cast<float>(y);
        if (fx < 0.0f || fx > width_ || fy < pad_ || fy > height_ - pad_)
        {
            return -1;
        }
        const int row = static_cast<int>((fy - pad_) / rowHeight_);
        return row >= 0 && row < kItemCount ? row : -1;
    }

    void Draw()
    {
        static const wchar_t* kItems[kItemCount] = {
            L"综合排序", L"最近打开", L"热度排序", L"最近添加",
            L"创建时间", L"文件大小"};
        render::Renderer* painter = &renderer_;
        painter->Begin();
        painter->Clear(render::Theme::kPanel);
        painter->StrokeRect({0.5f * scale_, 0.5f * scale_,
                             width_ - 0.5f * scale_, height_ - 0.5f * scale_},
                            render::Theme::kBorder, 1.0f * scale_);
        for (int i = 0; i < kItemCount; ++i)
        {
            const float top = pad_ + i * rowHeight_;
            const D2D1_RECT_F rowRect = {pad_ * 0.5f, top,
                                         width_ - pad_ * 0.5f,
                                         top + rowHeight_};
            if (i == hover_)
            {
                painter->FillRect(rowRect, render::Theme::kCardHover);
            }
            if (i == current_)
            {
                const float midY = top + rowHeight_ / 2.0f;
                painter->FillRect(
                    {rowRect.left + 4.0f * scale_, midY - 9.0f * scale_,
                     rowRect.left + 7.0f * scale_, midY + 9.0f * scale_},
                    render::Theme::kAccent);
            }
            const D2D1_COLOR_F color = i == current_
                ? render::Theme::kAccent
                : render::Theme::kText;
            painter->DrawTextWide(
                kItems[i],
                {rowRect.left + 16.0f * scale_, top,
                 rowRect.right - 10.0f * scale_, top + rowHeight_},
                render::TextRole::kSearch, color);
        }
        painter->End();
    }

    HWND hwnd_ = nullptr;
    render::Renderer renderer_;
    float scale_ = 1.0f;
    float width_ = 0.0f;
    float height_ = 0.0f;
    float pad_ = 0.0f;
    float rowHeight_ = 0.0f;
    int current_ = 0;
    int hover_ = -1;
    int selected_ = -1;
    bool done_ = false;
    bool tracking_ = false;
};

int Application::Run(const wchar_t* commandLine)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const std::wstring normalized = NormalizeQuery(commandLine);
    if (normalized == L"--help" || normalized == L"-h")
    {
        MessageBoxW(nullptr,
                    L"Viewer (VR)\n\n"
                    L"vr [关键词]        搜索资源\n"
                    L"vr --db-info       查看数据库统计\n"
                    L"vr --backup        备份数据库\n"
                    L"vr --scan          扫描托管库并恢复索引\n"
                    L"vr --recover       恢复托管库索引并统计未完成导入\n"
                    L"vr --export        导出 Metadata CSV",
                    L"Viewer", MB_ICONINFORMATION);
        return 0;
    }
    if (normalized == L"--backup" || normalized == L"--db-info" ||
        normalized == L"--scan" || normalized == L"--recover" ||
        normalized == L"--export")
    {
        return RunMaintenance(normalized);
    }
    if (normalized.starts_with(L"--"))
    {
        MessageBoxW(nullptr, L"未知命令。使用 vr --help 查看可用命令。",
                    L"Viewer", MB_ICONWARNING);
        return 2;
    }

    Application app;
    if (!app.Init(normalized))
    {
        app.Shutdown();
        return 1;
    }
    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    app.Shutdown();
    return static_cast<int>(message.wParam);
}

Application::Application()
    : exeDir_(ModuleDir()),
      libraryRoot_(exeDir_ / L"NexusLibrary"),
      resources_(database_),
      tags_(database_),
      archive_(libraryRoot_),
      importer_(exeDir_ / L"data" / L"nexus.db", archive_),
      deleter_(exeDir_ / L"data" / L"nexus.db")
{
    playback_.SetSettings(&settings_);
}

bool Application::Init(const std::wstring& initialQuery)
{
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
    {
        return false;
    }
    comInitialized_ = true;
    settings_.Load(exeDir_ / L"nexus.toml");
    libraryRoot_ = settings_.managedLibrary().empty()
        ? exeDir_ / L"NexusLibrary"
        : std::filesystem::path(settings_.managedLibrary());
    browsePath_ = libraryRoot_;
    archive_.SetLibraryRoot(libraryRoot_);

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(windowClass.hInstance,
                                 MAKEINTRESOURCEW(IDI_VIEWER));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(
        windowClass.hInstance, MAKEINTRESOURCEW(IDI_VIEWER), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = L"NexusMainWindow";
    RegisterClassExW(&windowClass);

    hwnd_ = CreateWindowExW(0, windowClass.lpszClassName, L"Viewer",
                            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            1960, 1080, nullptr, nullptr, windowClass.hInstance,
                            nullptr);
    if (hwnd_ == nullptr)
    {
        return false;
    }
    {
        constexpr DWORD kCornerPreference = 33;
        constexpr DWORD kRound = 2;
        DwmSetWindowAttribute(hwnd_, kCornerPreference, &kRound,
                              sizeof(kRound));
    }
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(this));
    // WM_NCCALCSIZE already ran during CreateWindowExW with the default
    // handler; force a frame recalculation now that this is wired up so the
    // standard caption is removed.
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE);

    scale_ = static_cast<float>(GetDpiForWindow(hwnd_)) / 96.0f;
    if (!renderer_.Init(hwnd_, scale_))
    {
        return false;
    }

    const std::filesystem::path databasePath =
        exeDir_ / L"data" / L"nexus.db";
    if (!database_.Open(databasePath))
    {
        MessageBoxW(hwnd_, L"无法打开数据库。", L"Viewer", MB_ICONERROR);
        return false;
    }
    if (!db::Migrate(database_))
    {
        MessageBoxW(hwnd_,
                    L"数据库迁移失败。Viewer 需要带 FTS5 扩展的 SQLite。",
                    L"Viewer", MB_ICONERROR);
        return false;
    }

    ReloadFromDatabase();

    thumbnails_.Start(hwnd_, kThumbReadyMsg,
                      exeDir_ / L"cache" / L"thumbnails");
    importer_.Start(hwnd_, kImportDoneMsg, kImportProgressMsg);
    deleter_.Start(hwnd_, kDeleteDoneMsg);
    DragAcceptFiles(hwnd_, TRUE);

    searchBar_.set_on_text_changed([this] { Refilter(); });
    searchBar_.SetFocus(true);
    if (!initialQuery.empty())
    {
        searchBar_.SetText(initialQuery);
    }

    Refilter();
    ShowWindow(hwnd_, SW_SHOW);
    SetTimer(hwnd_, kCaretTimer, 530, nullptr);
    // Let the first frame paint before generating demo assets. Real
    // installations with an existing database do no seed work here.
    PostMessageW(hwnd_, kSeedMsg, 0, 0);
    return true;
}

void Application::Shutdown()
{
    EndDragSelection(true);
    if (hwnd_ != nullptr)
    {
        KillTimer(hwnd_, kCaretTimer);
        DragAcceptFiles(hwnd_, FALSE);
    }
    if (seedWorker_.joinable())
    {
        seedWorker_.join();
    }
    stopLibraryScanWorker_ = true;
    if (libraryScanWorker_.joinable())
    {
        libraryScanWorker_.join();
    }
    stopMissingWorker_ = true;
    if (missingWorker_.joinable())
    {
        missingWorker_.join();
    }
    importer_.Shutdown();
    deleter_.Shutdown();
    stopMissingWorker_ = true;
    if (missingWorker_.joinable())
    {
        missingWorker_.join();
    }
    thumbnails_.Shutdown();
    renderer_.Shutdown();
    database_.Close();
    settings_.Save(exeDir_ / L"nexus.toml");
    if (hwnd_ != nullptr && IsWindow(hwnd_))
    {
        DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
    if (comInitialized_)
    {
        CoUninitialize();
        comInitialized_ = false;
    }
}

LRESULT CALLBACK Application::WndProc(HWND hwnd, UINT msg, WPARAM wp,
                                      LPARAM lp)
{
    auto* app = reinterpret_cast<Application*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (app == nullptr)
    {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return app->HandleMessage(msg, wp, lp);
}

LRESULT Application::HandleMessage(UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        DrawFrame();
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCCALCSIZE:
        // Remove the whole non-client area; the header is drawn by the app.
        if (wp == FALSE)
        {
            break;
        }
        if (IsZoomed(hwnd_))
        {
            // Keep maximized content inside the visible work area: the
            // window rect extends past the monitor by the frame thickness.
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
            const int frameX = GetSystemMetrics(SM_CXSIZEFRAME) +
                               GetSystemMetrics(SM_CXPADDEDBORDER);
            const int frameY = GetSystemMetrics(SM_CYSIZEFRAME) +
                               GetSystemMetrics(SM_CXPADDEDBORDER);
            params->rgrc[0].left += frameX;
            params->rgrc[0].top += frameY;
            params->rgrc[0].right -= frameX;
            params->rgrc[0].bottom -= frameY;
        }
        return 0;
    case WM_NCACTIVATE:
        return DefWindowProcW(hwnd_, msg, wp, -1);
    case WM_NCHITTEST:
    {
        // Keep native resize borders; turn the custom header into a drag
        // caption so native dragging, double-click maximize and Aero Snap
        // (edge docking) keep working.
        const LRESULT hit = DefWindowProcW(hwnd_, msg, wp, lp);
        RECT client = {};
        GetClientRect(hwnd_, &client);
        POINT point = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &point);
        const float s = scale_;
        const float titleHeight = kCaptionBarDp * s;
        const float buttons = 3.0f * kCaptionButtonDp * s;
        if (point.y >= 0 && point.y < static_cast<LONG>(titleHeight) &&
            point.x >= client.right - static_cast<LONG>(buttons))
        {
            const float single = kCaptionButtonDp * s;
            const bool overMaximize =
                point.x >= client.right - static_cast<LONG>(2.0f * single) &&
                point.x < client.right - static_cast<LONG>(single);
            return overMaximize ? HTMAXBUTTON : HTCLIENT;
        }
        // The scrollbar is painted inside the right client edge. Keep its
        // generous mouse target in the client hit-test region instead of
        // letting the resize border consume the drag gesture.
        const D2D1_RECT_F content = ContentRect();
        const float scrollHitWidth = 14.0f * s;
        if (CardScrollThumbRect().has_value() &&
            point.x >= static_cast<LONG>(content.right - scrollHitWidth) &&
            point.x < static_cast<LONG>(content.right) &&
            point.y >= static_cast<LONG>(content.top) &&
            point.y < static_cast<LONG>(content.bottom))
        {
            return HTCLIENT;
        }
        if (hit != HTCLIENT && hit != HTNOWHERE && hit != HTTRANSPARENT)
        {
            return hit;
        }
        if (!IsZoomed(hwnd_))
        {
            const int border = std::max(6, static_cast<int>(8.0f * s));
            const bool left = point.x < border;
            const bool right = point.x >= client.right - border;
            const bool top = point.y < border;
            const bool bottom = point.y >= client.bottom - border;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
        }
        const float navigationBottom = (kCaptionBarDp + kTopBarDp) * s;
        if (point.y >= navigationBottom)
        {
            return HTCLIENT;
        }
        const D2D1_RECT_F search = SearchRect();
        if (point.x >= static_cast<LONG>(search.left) &&
            point.x <= static_cast<LONG>(search.right) &&
            point.y >= static_cast<LONG>(search.top) &&
            point.y <= static_cast<LONG>(search.bottom))
        {
            return HTCLIENT;
        }
        const D2D1_RECT_F sort = SortRect();
        if (point.x >= static_cast<LONG>(sort.left) &&
            point.x <= static_cast<LONG>(sort.right) &&
            point.y >= static_cast<LONG>(sort.top) &&
            point.y <= static_cast<LONG>(sort.bottom))
        {
            return HTCLIENT;
        }
        if (point.y >= static_cast<LONG>(titleHeight))
        {
            return HTCLIENT;
        }
        return HTCAPTION;
    }
    case WM_NCLBUTTONDOWN:
        // The maximize button reports HTMAXBUTTON for the Win11 snap
        // layouts flyout; clicks arrive here as non-client messages.
        if (wp == HTMAXBUTTON)
        {
            ToggleMaximize();
            return 0;
        }
        break;
    case WM_NCMOUSEMOVE:
        SetHeaderHover(wp == HTMAXBUTTON ? 1 : -1);
        return 0;
    case WM_MOUSELEAVE:
        trackingMouse_ = false;
        SetHeaderHover(-1);
        return 0;
    case WM_SIZE:
        EndDragSelection(true);
        if (wp != SIZE_MINIMIZED)
        {
            renderer_.Resize(LOWORD(lp), HIWORD(lp));
            ClampScroll();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    case WM_GETMINMAXINFO:
    {
        auto* info = reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMinTrackSize = {980, 640};
        return 0;
    }
    case WM_CHAR:
        if (searchBar_.OnChar(static_cast<wchar_t>(wp)))
        {
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && wp == 'F')
        {
            searchBar_.SetFocus(true);
            Invalidate();
            return 0;
        }
        if (searchBar_.OnKeyDown(
                wp, (GetKeyState(VK_CONTROL) & 0x8000) != 0,
                (GetKeyState(VK_SHIFT) & 0x8000) != 0))
        {
            Invalidate();
            return 0;
        }
        switch (wp)
        {
        case VK_ESCAPE:
            if (selectionDrag_.active())
            {
                EndDragSelection(true);
                return 0;
            }
            if (searchBar_.focused() && !searchBar_.text().empty())
            {
                searchBar_.Clear();
            }
            else
            {
                selectedId_ = 0;
                selectedIds_.clear();
                Invalidate();
            }
            return 0;
        case VK_RETURN:
        {
            Resource* item = nullptr;
            for (Resource& candidate : items_)
            {
                if (candidate.id == selectedId_)
                {
                    item = &candidate;
                }
            }
            if (item == nullptr && !filtered_.empty())
            {
                item = const_cast<Resource*>(filtered_.front());
            }
            OpenResource(item);
            return 0;
        }
        case VK_DELETE:
            DeleteSelected((GetKeyState(VK_SHIFT) & 0x8000) != 0);
            return 0;
        case VK_APPS:
            if (selectedId_ != 0)
            {
                PostMessageW(hwnd_, WM_CONTEXTMENU, 0,
                             MAKELPARAM(-1, -1));
            }
            return 0;
        case VK_F10:
            if ((GetKeyState(VK_SHIFT) & 0x8000) != 0 &&
                selectedId_ != 0)
            {
                PostMessageW(hwnd_, WM_CONTEXTMENU, 0,
                             MAKELPARAM(-1, -1));
                return 0;
            }
            break;
        case 'F':
            if (!searchBar_.focused())
            {
                ToggleSelectedFavorite();
                return 0;
            }
            break;
        case VK_LEFT:
            MoveSelection(-1);
            return 0;
        case VK_RIGHT:
            MoveSelection(1);
            return 0;
        case VK_UP:
            MoveSelection(-ColumnCount());
            return 0;
        case VK_DOWN:
            MoveSelection(ColumnCount());
            return 0;
        case VK_HOME:
            if (!filtered_.empty())
            {
                selectedId_ = filtered_.front()->id;
                selectedIds_.clear();
                selectedIds_.insert(selectedId_);
                scrollY_ = 0.0f;
                Invalidate();
            }
            return 0;
        case VK_END:
            if (!filtered_.empty())
            {
                selectedId_ = filtered_.back()->id;
                selectedIds_.clear();
                selectedIds_.insert(selectedId_);
                scrollY_ = std::numeric_limits<float>::max();
                ClampScroll();
                Invalidate();
            }
            return 0;
        default:
            break;
        }
        break;
    case WM_MOUSEMOVE:
    {
        POINT point = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (draggingCardScroll_)
        {
            UpdateCardScrollFromPointer(static_cast<float>(point.y));
            return 0;
        }
        if (selectionDrag_.active())
        {
            UpdateDragSelection(point);
            return 0;
        }
        UpdateHeaderHover(point);
        const int tag = HitTestSidebar(point);
        if (tag != sideHover_)
        {
            sideHover_ = tag;
            Invalidate();
        }
        auto id = ui::ResourceGrid::HitTest(MakeView(ContentRect()), point);
        ResourceId hovered = id.value_or(0);
        if (hovered != hoveredId_)
        {
            hoveredId_ = hovered;
            Invalidate();
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
    {
        POINT point = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        RECT client = {};
        GetClientRect(hwnd_, &client);
        const float s = scale_;
        const int sideItem = HitTestSidebar(point);
        if (sideItem == kSidebarNavigationHeader)
        {
            navigationExpanded_ = !navigationExpanded_;
            ClampSideScroll();
            Invalidate();
            return 0;
        }
        if (sideItem == kSidebarTagsHeader)
        {
            tagsExpanded_ = !tagsExpanded_;
            ClampSideScroll();
            Invalidate();
            return 0;
        }
        if (sideItem >= 0)
        {
            if (sideItem < kSidebarNavigationCount)
            {
                sidebarFilter_ = static_cast<SidebarFilter>(sideItem);
                tagFilter_.clear();
                if (sidebarFilter_ == SidebarFilter::kBrowse)
                {
                    browsePath_ = libraryRoot_;
                    ReloadBrowseDirectory();
                    searchBar_.Clear();
                    searchBar_.SetFocus(false);
                }
                Refilter();
                return 0;
            }
            const int tag = sideItem - kSidebarNavigationCount;
            if (tag < 0 || tag >= static_cast<int>(sideTags_.size()))
            {
                return 0;
            }
            sidebarFilter_ = SidebarFilter::kAll;
            tagFilter_ = tagFilter_ == sideTags_[tag].name
                ? std::string()
                : sideTags_[tag].name;
            searchBar_.SetFocus(false);
            Refilter();
            return 0;
        }
        const float single = kCaptionButtonDp * s;
        if (point.y < static_cast<LONG>(kCaptionBarDp * s) &&
            point.x >= client.right - static_cast<LONG>(3.0f * single))
        {
            // Minimize / close (maximize arrives as WM_NCLBUTTONDOWN).
            if (point.x < client.right - static_cast<LONG>(2.0f * single))
            {
                SendMessageW(hwnd_, WM_SYSCOMMAND, SC_MINIMIZE, 0);
            }
            else if (point.x >= client.right - static_cast<LONG>(single))
            {
                SendMessageW(hwnd_, WM_SYSCOMMAND, SC_CLOSE, 0);
            }
            return 0;
        }
        if (point.y < static_cast<LONG>(kCaptionBarDp * s))
        {
            ReleaseCapture();
            SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        }
        if (searchBar_.HitTest(point))
        {
            searchBar_.SetFocus(true);
            Invalidate();
            return 0;
        }
        const D2D1_RECT_F settingsRect = SettingsRect();
        if (point.x >= static_cast<LONG>(settingsRect.left) &&
            point.x <= static_cast<LONG>(settingsRect.right) &&
            point.y >= static_cast<LONG>(settingsRect.top) &&
            point.y <= static_cast<LONG>(settingsRect.bottom))
        {
            ShowSettings();
            return 0;
        }
        const D2D1_RECT_F sortRect = SortRect();
        if (point.y >= static_cast<LONG>(kCaptionBarDp * s) &&
            point.y < static_cast<LONG>((kCaptionBarDp + kTopBarDp) * s) &&
            point.x >= static_cast<LONG>(sortRect.left) &&
            point.x <= static_cast<LONG>(sortRect.right))
        {
            ChooseSortMode();
            return 0;
        }
        if (const auto thumb = CardScrollThumbRect(); thumb.has_value())
        {
            const D2D1_RECT_F content = ContentRect();
            const float hitWidth = 14.0f * s;
            const bool overTrack =
                point.x >= static_cast<LONG>(content.right - hitWidth) &&
                point.x <= static_cast<LONG>(content.right) &&
                point.y >= static_cast<LONG>(content.top) &&
                point.y < static_cast<LONG>(content.bottom);
            if (overTrack)
            {
                cardScrollDragOffset_ =
                    std::clamp(static_cast<float>(point.y) - thumb->top,
                               0.0f, thumb->bottom - thumb->top);
                draggingCardScroll_ = true;
                SetCapture(hwnd_);
                UpdateCardScrollFromPointer(static_cast<float>(point.y));
                return 0;
            }
        }
        searchBar_.SetFocus(false);
        if (const auto favoriteId =
                ui::ResourceGrid::HitTestFavorite(MakeView(ContentRect()), point);
            favoriteId.has_value())
        {
            selectedId_ = *favoriteId;
            selectedIds_.clear();
            selectedIds_.insert(*favoriteId);
            ToggleSelectedFavorite();
            return 0;
        }
        auto id = ui::ResourceGrid::HitTest(MakeView(ContentRect()), point);
        const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (!id.has_value())
        {
            const D2D1_RECT_F content = ContentRect();
            if (point.x >= static_cast<LONG>(content.left) &&
                point.x < static_cast<LONG>(content.right) &&
                point.y >= static_cast<LONG>(content.top) &&
                point.y < static_cast<LONG>(content.bottom))
            {
                selectionDrag_.Begin(point, content, scrollY_, control,
                                     selectedIds_, selectedId_);
                SetCapture(hwnd_);
                UpdateDragSelection(point);
                return 0;
            }
            if (!control && !shift)
            {
                selectedId_ = 0;
                selectedIds_.clear();
            }
        }
        else if (shift && selectedId_ != 0)
        {
            const int anchor = SelectedIndex();
            int target = -1;
            for (size_t i = 0; i < filtered_.size(); ++i)
            {
                if (filtered_[i]->id == *id)
                {
                    target = static_cast<int>(i);
                    break;
                }
            }
            if (!control)
            {
                selectedIds_.clear();
            }
            if (anchor >= 0 && target >= 0)
            {
                const int first = std::min(anchor, target);
                const int last = std::max(anchor, target);
                for (int i = first; i <= last; ++i)
                {
                    selectedIds_.insert(filtered_[i]->id);
                }
            }
        }
        else if (control)
        {
            if (selectedIds_.contains(*id))
            {
                selectedIds_.erase(*id);
                selectedId_ = selectedIds_.empty() ? 0
                                                   : *selectedIds_.begin();
            }
            else
            {
                selectedIds_.insert(*id);
                selectedId_ = *id;
            }
        }
        else
        {
            selectedId_ = *id;
            selectedIds_.clear();
            selectedIds_.insert(*id);
        }
        Invalidate();
        return 0;
    }
    case WM_LBUTTONUP:
    {
        if (draggingCardScroll_)
        {
            draggingCardScroll_ = false;
            cardScrollDragOffset_ = 0.0f;
            if (GetCapture() == hwnd_)
            {
                ReleaseCapture();
            }
            return 0;
        }
        if (!selectionDrag_.active())
        {
            break;
        }
        UpdateDragSelection({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        EndDragSelection(false);
        return 0;
    }
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        EndDragSelection(true);
        if (draggingCardScroll_)
        {
            draggingCardScroll_ = false;
            cardScrollDragOffset_ = 0.0f;
        }
        break;
    case WM_LBUTTONDBLCLK:
    {
        POINT point = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        auto id = ui::ResourceGrid::HitTest(MakeView(ContentRect()), point);
        if (id.has_value())
        {
            selectedId_ = *id;
            selectedIds_.clear();
            selectedIds_.insert(*id);
            if (sidebarFilter_ == SidebarFilter::kBrowse)
            {
                for (const Resource& item : browseItems_)
                {
                    if (item.id != *id)
                    {
                        continue;
                    }
                    if (item.type == ResourceType::kFolder)
                    {
                        browsePath_ = item.path;
                        ReloadBrowseDirectory();
                        selectedId_ = 0;
                        selectedIds_.clear();
                        scrollY_ = 0.0f;
                        Refilter();
                    }
                    else
                    {
                        // A browse-only media row may not have reached the
                        // database yet. It is still directly openable.
                        OpenResource(const_cast<Resource*>(&item));
                    }
                    break;
                }
            }
            else
            {
            for (Resource& candidate : items_)
            {
                if (candidate.id == *id)
                {
                    OpenResource(&candidate);
                }
            }
            }
        }
        return 0;
    }
    case WM_CONTEXTMENU:
    {
        POINT screenPoint = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        std::optional<ResourceId> id;
        if (screenPoint.x == -1 && screenPoint.y == -1 && selectedId_ != 0)
        {
            id = selectedId_;
            GetCursorPos(&screenPoint);
        }
        else
        {
            POINT clientPoint = screenPoint;
            ScreenToClient(hwnd_, &clientPoint);
            id = ui::ResourceGrid::HitTest(
                MakeView(ContentRect()), clientPoint);
        }
        const bool blankArea = !id.has_value();
        if (blankArea && sidebarFilter_ != SidebarFilter::kBrowse)
        {
            // Folder creation is intentionally scoped to the Browse view;
            // filtered media/tag views must remain read-only.
            return 0;
        }
        if (blankArea)
        {
            POINT clientPoint = screenPoint;
            ScreenToClient(hwnd_, &clientPoint);
            const D2D1_RECT_F content = ContentRect();
            if (clientPoint.x < static_cast<LONG>(content.left) ||
                clientPoint.x >= static_cast<LONG>(content.right) ||
                clientPoint.y < static_cast<LONG>(content.top) ||
                clientPoint.y >= static_cast<LONG>(content.bottom))
            {
                return 0;
            }
        }
        if (!blankArea)
        {
            selectedId_ = *id;
            if (!selectedIds_.contains(*id))
            {
                selectedIds_.clear();
                selectedIds_.insert(*id);
            }
            Invalidate();
        }
        HMENU menu = CreatePopupMenu();
        if (menu == nullptr)
        {
            return 0;
        }
        HBRUSH menuBrush = CreateSolidBrush(kMenuBack);
        MENUINFO menuInfo = {sizeof(menuInfo)};
        menuInfo.fMask = MIM_BACKGROUND | MIM_STYLE;
        menuInfo.hbrBack = menuBrush;
        menuInfo.dwStyle = MNS_AUTODISMISS;
        SetMenuInfo(menu, &menuInfo);
        bool folderCard = false;
        if (!blankArea && sidebarFilter_ == SidebarFilter::kBrowse)
        {
            for (const Resource& item : browseItems_)
            {
                if (item.id == *id)
                {
                    folderCard = item.type == ResourceType::kFolder;
                    break;
                }
            }
        }
        if (blankArea)
        {
            AddDarkMenuItem(menu, kMenuNewFolder, L"新建文件夹");
        }
        else if (folderCard)
        {
            // Synthetic Browse folders are not database resources. Expose
            // only actions that are meaningful for them, rather than showing
            // metadata/favorite/delete commands that cannot operate on their
            // synthetic IDs.
            AddDarkMenuItem(menu, kMenuOpen, L"打开");
            AddDarkMenuItem(menu, kMenuBrowse, L"在资源管理器中浏览");
        }
        else
        {
            AddDarkMenuItem(menu, kMenuOpen, L"打开");
            AddDarkMenuItem(menu, kMenuBrowse, L"在资源管理器中浏览");
            AddDarkMenuItem(menu, kMenuEditMetadata, L"编辑 Metadata");
            AddDarkMenuItem(menu, kMenuFavorite, L"切换收藏");
            AddDarkMenuSeparator(menu);
            AddDarkMenuItem(menu, kMenuDelete, L"移到回收站");
        }
        const UINT command = TrackPopupMenu(
            menu, TPM_RETURNCMD | TPM_NONOTIFY, screenPoint.x,
            screenPoint.y, 0, hwnd_, nullptr);
        DestroyMenu(menu);
        DeleteObject(menuBrush);
        switch (command)
        {
        case kMenuNewFolder:
        {
            if (sidebarFilter_ == SidebarFilter::kBrowse &&
                !BrowsePathsEqual(browsePath_, libraryRoot_))
            {
                statusMessage_ = L"只能在托管库根目录创建文件夹";
                Invalidate();
                break;
            }
            std::error_code error;
            const std::filesystem::path parent =
                sidebarFilter_ == SidebarFilter::kBrowse ? browsePath_
                                                          : libraryRoot_;
            std::filesystem::path folder = parent / L"新建文件夹";
            for (int suffix = 2; std::filesystem::exists(folder, error);
                 ++suffix)
            {
                error.clear();
                folder = parent /
                         (std::wstring(L"新建文件夹 ") +
                          std::to_wstring(suffix));
            }
            if (std::filesystem::create_directories(folder, error) && !error)
            {
                Resource resource;
                resource.type = ResourceType::kFolder;
                resource.path = folder;
                resource.originalFilename =
                    WideToUtf8(folder.filename().wstring());
                resource.title = resource.originalFilename;
                resource.addedTime = static_cast<std::int64_t>(std::time(nullptr));
                resource.modifiedTime = resource.addedTime;
                if (database_.Exec("BEGIN;") && resources_.Insert(resource) &&
                    database_.Exec("COMMIT;"))
                {
                    ReloadFromDatabase();
                    if (sidebarFilter_ == SidebarFilter::kBrowse)
                    {
                        ReloadBrowseDirectory();
                    }
                    Refilter();
                }
                statusMessage_ = L"已创建文件夹：" + folder.filename().wstring();
            }
            else
            {
                statusMessage_ = L"创建文件夹失败";
            }
            Invalidate();
            break;
        }
        case kMenuOpen:
            if (sidebarFilter_ == SidebarFilter::kBrowse)
            {
                for (const Resource& item : browseItems_)
                {
                    if (item.id != selectedId_)
                    {
                        continue;
                    }
                    if (item.type == ResourceType::kFolder)
                    {
                        browsePath_ = item.path;
                        ReloadBrowseDirectory();
                        selectedId_ = 0;
                        selectedIds_.clear();
                        scrollY_ = 0.0f;
                        Refilter();
                        return 0;
                    }
                    OpenResource(const_cast<Resource*>(&item));
                    return 0;
                }
            }
            for (Resource& item : items_)
            {
                if (item.id == selectedId_)
                {
                    OpenResource(&item);
                    break;
                }
            }
            break;
        case kMenuBrowse:
            if (sidebarFilter_ == SidebarFilter::kBrowse)
            {
                for (const Resource& item : browseItems_)
                {
                    if (item.id == selectedId_)
                    {
                        BrowseInExplorer(item.path);
                        return 0;
                    }
                }
            }
            for (const Resource& item : items_)
            {
                if (item.id == selectedId_)
                {
                    BrowseInExplorer(item.path);
                    break;
                }
            }
            break;
        case kMenuFavorite:
            ToggleSelectedFavorite();
            break;
        case kMenuEditMetadata:
            EditSelectedMetadata();
            break;
        case kMenuDelete:
            DeleteSelected(false);
            break;
        default:
            break;
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
    {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        POINT point = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &point);
        const D2D1_RECT_F sidebar = SidebarRect();
        if (!selectionDrag_.active() &&
            point.x >= static_cast<LONG>(sidebar.left) &&
            point.x < static_cast<LONG>(sidebar.right) &&
            point.y >= static_cast<LONG>(sidebar.top) &&
            point.y < static_cast<LONG>(sidebar.bottom))
        {
            sideScrollY_ -= static_cast<float>(delta) * 0.7f * scale_;
            ClampSideScroll();
        }
        else
        {
            scrollY_ -= static_cast<float>(delta) * 0.7f * scale_;
            ClampScroll();
            if (selectionDrag_.active())
            {
                UpdateDragSelection(point);
            }
        }
        Invalidate();
        return 0;
    }
    case WM_DROPFILES:
    {
        HDROP drop = reinterpret_cast<HDROP>(wp);
        UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::filesystem::path> files;
        for (UINT i = 0; i < count; ++i)
        {
            const UINT length = DragQueryFileW(drop, i, nullptr, 0);
            if (length == 0)
            {
                continue;
            }
            std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
            if (DragQueryFileW(drop, i, buffer.data(), length + 1) > 0)
            {
                buffer.resize(length);
                files.emplace_back(std::move(buffer));
            }
        }
        DragFinish(drop);
        if (!files.empty())
        {
            const std::vector<service::ImportItem> expanded =
                service::ImportService::ExpandInputs(files);
            std::vector<service::ImportItem> supportedItems;
            std::vector<std::filesystem::path> supported;
            for (const service::ImportItem& item : expanded)
            {
                if (service::ImportService::Classify(item.path).has_value())
                {
                    supported.push_back(item.path);
                    supportedItems.push_back(item);
                }
            }
            if (supported.empty())
            {
                MessageBoxW(hwnd_, L"未发现支持的视频或图片。", L"Viewer",
                            MB_ICONINFORMATION);
                return 0;
            }
            repo::CategoryRepository categoryRepository(database_);
            std::vector<std::string> categoryNames;
            for (const repo::Category& category : categoryRepository.LoadAll())
            {
                categoryNames.push_back(category.name);
            }
            std::optional<service::ImportMetadata> metadata;
            if (settings_.askMetadataOnDrop())
            {
                const std::vector<std::string> quickTags =
                    BuildQuickTags(settings_, tags_);
                metadata = ui::ImportDialog::Show(
                    hwnd_, supported, categoryNames, quickTags);
            }
            else
            {
                metadata = service::ImportMetadata{};
            }
            if (metadata.has_value())
            {
                metadata->deleteSource = settings_.deleteSourceAfterImport();
                importer_.ImportItems(std::move(supportedItems),
                                      std::move(*metadata));
            }
        }
        return 0;
    }
    case kImportDoneMsg:
    {
        const int imported = static_cast<int>(wp);
        const int skipped = static_cast<int>(HIWORD(lp));
        const int failed = static_cast<int>(LOWORD(lp));
        statusMessage_ = L"导入完成：成功 " + std::to_wstring(imported) +
                         L"，跳过 " + std::to_wstring(skipped) +
                         L"，失败 " + std::to_wstring(failed);
        importProgress_ = {};
        ReloadFromDatabase();
        Refilter();
        // Imported rows are already fully probed, but their thumbnails may
        // not be on disk yet.  Let the same segmented startup pass discover
        // and warm any newly added cache entries.
        StartBackgroundBuild();
        Invalidate();
        return 0;
    }
    case kImportProgressMsg:
        importProgress_ = importer_.GetProgress();
        Invalidate();
        return 0;
    case WM_TIMER:
        if (wp == kSelectionTimer && selectionDrag_.active())
        {
            const float previous = scrollY_;
            scrollY_ += selectionDrag_.ScrollStep(ContentRect(), scale_);
            ClampScroll();
            UpdateDragSelection(selectionDrag_.pointer());
            if (scrollY_ == previous)
            {
                KillTimer(hwnd_, kSelectionTimer);
                selectionTimerRunning_ = false;
            }
        }
        if (wp == kCaretTimer && searchBar_.TickCaret())
        {
            Invalidate();
        }
        return 0;
    case kMissingUpdatedMsg:
        ReloadFromDatabase();
        Refilter();
        return 0;
    case kDeleteDoneMsg:
    {
        int deleted = 0;
        int failed = 0;
        for (const service::DeleteBatchResult& result :
             deleter_.TakeCompleted())
        {
            failed += result.failed;
            deleted += static_cast<int>(result.deleted.size());
            for (ResourceId id : result.deleted)
            {
                thumbnails_.Evict(id);
                selectedIds_.erase(id);
            }
        }
        selectedId_ = selectedIds_.empty() ? 0 : *selectedIds_.begin();
        ReloadFromDatabase();
        Refilter();
        if (failed > 0)
        {
            const std::wstring message =
                std::to_wstring(deleted) + L" 个资源已删除，" +
                std::to_wstring(failed) + L" 个失败。";
            statusMessage_ = message;
            MessageBoxW(hwnd_, message.c_str(), L"Viewer", MB_ICONWARNING);
        }
        else
        {
            statusMessage_ = L"已删除 " + std::to_wstring(deleted) +
                             L" 个资源";
        }
        return 0;
    }
    case WM_DPICHANGED:
    {
        EndDragSelection(true);
        const UINT dpi = LOWORD(wp);
        if (dpi != 0)
        {
            scale_ = static_cast<float>(dpi) / 96.0f;
            renderer_.SetScale(scale_);
        }
        const RECT* suggested = reinterpret_cast<const RECT*>(lp);
        if (suggested != nullptr)
        {
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        ClampScroll();
        Invalidate();
        return 0;
    }
    case kThumbReadyMsg:
        Invalidate();
        return 0;
    case kSeedMsg:
        if (!missingWorker_.joinable())
        {
            stopMissingWorker_ = false;
            missingWorker_ = std::thread([this] { MissingWorkerLoop(); });
        }
        SeedEmptyLibrary();
        // Existing installations may already contain legacy rows with empty
        // derived fields.  Begin the incremental scan only after the first
        // frame has been shown so startup stays responsive.
        StartBackgroundBuild();
        return 0;
    case kLibraryScanDoneMsg:
        // The worker posts this message immediately before returning.  Do
        // not join here: an older completion message can remain queued while
        // a newer pass is starting, and joining would then freeze the UI on
        // the newer (potentially large) pass.  StartBackgroundBuild(),
        // Shutdown() and the next settings change perform the join safely.
        ReloadFromDatabase();
        Refilter();
        statusMessage_ = L"托管文件夹索引已更新";
        Invalidate();
        return 0;
    case kLibraryScanReadyMsg:
        // The lightweight pass has committed path/title/type rows.  Refresh
        // immediately so cards are visible while the worker continues with
        // hashes, dimensions, durations and thumbnails.
        ReloadFromDatabase();
        Refilter();
        statusMessage_ = L"文件已加载，正在后台构建缩略图和索引…";
        Invalidate();
        return 0;
    case WM_DESTROY:
        EndDragSelection(true);
        PostQuitMessage(0);
        return 0;
    case WM_MEASUREITEM:
    {
        auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        if (measure != nullptr && measure->CtlType == ODT_MENU)
        {
            measure->itemHeight = measure->itemData == 0 ? 12 : 54;
            measure->itemWidth = 300;
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM:
    {
        const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if (draw != nullptr && draw->CtlType == ODT_MENU)
        {
            if (draw->itemData == 0)
            {
                RECT separator = draw->rcItem;
                separator.left += 14;
                separator.right -= 14;
                separator.top += separator.bottom > separator.top ? 5 : 0;
                separator.bottom = separator.top + 1;
                HBRUSH brush = CreateSolidBrush(kMenuBorder);
                FillRect(draw->hDC, &separator, brush);
                DeleteObject(brush);
                return TRUE;
            }
            const bool selected = (draw->itemState & ODS_SELECTED) != 0;
            RECT card = draw->rcItem;
            InflateRect(&card, -4, -2);
            FillMenuRoundRect(draw->hDC, card,
                              selected ? kMenuHover : kMenuCard, 10);
            FrameMenuRoundRect(draw->hDC, card,
                               selected ? kMenuAccent : kMenuBorder, 10);
            SetBkMode(draw->hDC, TRANSPARENT);
            SetTextColor(draw->hDC, kMenuText);
            RECT text = card;
            text.left += 18;
            HFONT font = MenuFont();
            HFONT old = reinterpret_cast<HFONT>(SelectObject(draw->hDC, font));
            DrawTextW(draw->hDC,
                      reinterpret_cast<const wchar_t*>(draw->itemData), -1,
                      &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(draw->hDC, old);
            return TRUE;
        }
        break;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void Application::DrawFrame()
{
    renderer_.Begin();

    RECT clientRect = {};
    GetClientRect(hwnd_, &clientRect);
    const float width =
        static_cast<float>(std::max(0L, clientRect.right - clientRect.left));
    const float height =
        static_cast<float>(std::max(0L, clientRect.bottom - clientRect.top));
    const float s = scale_;

    renderer_.Clear(render::Theme::kWindow);

    // Custom chrome: title row, navigation row, then sidebar/content.
    const float titleHeight = kCaptionBarDp * s;
    const float navHeight = kTopBarDp * s;
    const float topHeight = titleHeight + navHeight;
    renderer_.FillRect({0.0f, 0.0f, width, topHeight}, render::Theme::kPanel);
    const float titleHalf = 9.0f * s;
    renderer_.DrawIcon(LoadIconW(GetModuleHandleW(nullptr),
                                  MAKEINTRESOURCEW(IDI_VIEWER)),
                       {14.0f * s, 10.0f * s, 38.0f * s, 34.0f * s});
    renderer_.DrawTextWide(
        L"Viewer",
        {48.0f * s, titleHeight / 2.0f - titleHalf, 172.0f * s,
         titleHeight / 2.0f + titleHalf},
        render::TextRole::kTitle, render::Theme::kText);
    searchBar_.Layout(SearchRect());
    searchBar_.Draw(&renderer_);
    const wchar_t* sortLabel = L"综合排序  ▾";
    if (sortMode_ == SortMode::kRecentOpen)
    {
        sortLabel = L"最近打开  ▾";
    }
    else if (sortMode_ == SortMode::kHot)
    {
        sortLabel = L"热度排序  ▾";
    }
    else if (sortMode_ == SortMode::kAdded)
    {
        sortLabel = L"最近添加  ▾";
    }
    else if (sortMode_ == SortMode::kCreated)
    {
        sortLabel = L"创建时间  ▾";
    }
    else if (sortMode_ == SortMode::kFileSize)
    {
        sortLabel = L"文件大小  ▾";
    }
    const float sortHalf = 8.0f * s;
    const D2D1_RECT_F sortRect = SortRect();
    renderer_.DrawTextWide(
        sortLabel,
        {sortRect.left, titleHeight + navHeight / 2.0f - sortHalf,
         sortRect.right, titleHeight + navHeight / 2.0f + sortHalf},
        render::TextRole::kSecondary, render::Theme::kTextSecondary);
    renderer_.FillRect({0.0f, titleHeight, width, titleHeight + 1.0f},
                       render::Theme::kBorder);
    renderer_.FillRect({0.0f, topHeight, width, topHeight + 1.0f},
                       render::Theme::kBorder);
    DrawCaptionButtons(width, titleHeight);

    DrawSidebar();

    const D2D1_RECT_F content = ContentRect();
    if (filtered_.empty())
    {
        const float centerY = (content.top + content.bottom) / 2.0f;
        const wchar_t* message =
            searchBar_.text().empty() && tagFilter_.empty() &&
                    sidebarFilter_ == SidebarFilter::kAll
                ? L"资源库为空"
                : L"没有匹配的资源";
        const float textWidth = renderer_.MeasureTextWide(
            message, render::TextRole::kSecondary);
        const float centerX = std::max(
            content.left, (content.left + content.right - textWidth) / 2.0f);
        renderer_.DrawTextWide(message,
                               {centerX, centerY - 12.0f * s,
                                std::min(width, centerX + textWidth + 8.0f * s),
                                centerY + 12.0f * s},
                               render::TextRole::kSecondary,
                               render::Theme::kTextSecondary);
    }
    else
    {
        const ui::GridView view = MakeView(content);
        renderer_.PushRectClip(content);
        ui::ResourceGrid::Draw(&renderer_, view, &thumbnails_);
        renderer_.PopRectClip();

        if (const auto thumb = CardScrollThumbRect(); thumb.has_value())
        {
            renderer_.FillRect(*thumb, render::Theme::kBorder);
        }
    }

    // Status bar.
    const float statusHeight = kStatusBarDp * s;
    renderer_.FillRect({0.0f, height - statusHeight, width, height},
                       render::Theme::kPanel);
    renderer_.FillRect(
        {0.0f, height - statusHeight, width, height - statusHeight + 1.0f},
        render::Theme::kBorder);
    if (importProgress_.active)
    {
        std::wstring fileName = importProgress_.currentPath;
        if (!fileName.empty())
        {
            fileName = std::filesystem::path(fileName).filename().wstring();
        }
        std::wstring label = L"正在导入 " +
            std::to_wstring(importProgress_.currentFile + 1) + L"/" +
            std::to_wstring(importProgress_.totalFiles);
        if (!fileName.empty())
        {
            label += L" · " + fileName;
        }
        const float barLeft = width * 0.42f;
        const float barRight = std::max(barLeft + 120.0f * s,
                                       width - 52.0f * s);
        const float barY = height - statusHeight + 9.0f * s;
        renderer_.DrawTextWide(label,
                               {8.0f * s, height - statusHeight, barLeft -
                                10.0f * s, height},
                               render::TextRole::kStatus,
                               render::Theme::kTextSecondary);
        renderer_.FillRoundRect({barLeft, barY, barRight,
                                 barY + 8.0f * s},
                                render::Theme::kInput, 4.0f * s);
        float fraction = 0.0f;
        if (importProgress_.totalBytes != 0)
        {
            fraction = static_cast<float>(importProgress_.bytesCopied) /
                       static_cast<float>(importProgress_.totalBytes);
        }
        else if (importProgress_.totalFiles != 0)
        {
            fraction = static_cast<float>(importProgress_.currentFile) /
                       static_cast<float>(importProgress_.totalFiles);
        }
        fraction = std::clamp(fraction, 0.0f, 1.0f);
        renderer_.FillRoundRect({barLeft, barY, barLeft +
                                 (barRight - barLeft) * fraction,
                                 barY + 8.0f * s},
                                render::Theme::kAccent, 4.0f * s);
        const int percent = static_cast<int>(fraction * 100.0f + 0.5f);
        renderer_.DrawTextWide(std::to_wstring(percent) + L"%",
                               {barRight + 8.0f * s,
                                height - statusHeight,
                                width - 8.0f * s, height},
                               render::TextRole::kStatus,
                               render::Theme::kTextSecondary);
    }
    else
    {
        if (!statusMessage_.empty())
        {
            renderer_.DrawTextWide(
                statusMessage_,
                {8.0f * s, height - statusHeight, width * 0.66f, height},
                render::TextRole::kStatus, render::Theme::kTextSecondary);
        }
        else
        {
            std::string status = std::to_string(filtered_.size()) +
                                  " 个资源";
            if (selectedIds_.size() > 1)
            {
                status += " · 已选择 " +
                          std::to_string(selectedIds_.size());
            }
            renderer_.DrawTextUtf8(
                status,
                {8.0f * s, height - statusHeight, width * 0.5f, height},
                render::TextRole::kStatus,
                render::Theme::kTextSecondary);
        }
    }

    if (!renderer_.End())
    {
        thumbnails_.ResetBitmaps();
        renderer_.Shutdown();
        renderer_.Init(hwnd_, scale_);
        Invalidate();
    }
}

void Application::ReloadFromDatabase()
{
    EndDragSelection(true);
    items_ = resources_.LoadAll();
    if (sidebarFilter_ == SidebarFilter::kBrowse)
    {
        ReloadBrowseDirectory();
    }
    UpdateSidebarCounts();
    LoadSideTags();
}

void Application::UpdateSidebarCounts()
{
    sidebarCounts_.fill(0);
    for (const Resource& item : items_)
    {
        ++sidebarCounts_[0];
        if (item.type == ResourceType::kVideo)
        {
            ++sidebarCounts_[1];
        }
        else
        {
            ++sidebarCounts_[2];
        }
        if (item.favorite)
        {
            ++sidebarCounts_[3];
        }
    }
    sidebarCounts_[kBrowseNavigationIndex] = browseItems_.size();
}

void Application::LoadSideTags()
{
    sideTags_ = tags_.LoadAllWithCounts();
    ClampSideScroll();
}

void Application::ReloadBrowseDirectory()
{
    EndDragSelection(true);
    if (browsePath_.empty())
    {
        browsePath_ = libraryRoot_;
    }
    browseItems_.clear();
    std::error_code rootError;
    if (!std::filesystem::is_directory(browsePath_, rootError) || rootError)
    {
        sidebarCounts_[kBrowseNavigationIndex] = 0;
        return;
    }

    struct Entry
    {
        std::filesystem::path path;
        bool directory = false;
    };
    std::vector<Entry> entries;
    std::error_code iterationError;
    for (std::filesystem::directory_iterator iterator(
             browsePath_, std::filesystem::directory_options::skip_permission_denied,
             iterationError),
         end;
         iterator != end; iterator.increment(iterationError))
    {
        if (iterationError)
        {
            iterationError.clear();
            continue;
        }
        const std::filesystem::path path = iterator->path();
        if (path.filename() == L".incoming")
        {
            continue;
        }
        std::error_code entryError;
        if (iterator->is_directory(entryError) && !entryError)
        {
            entries.push_back({path, true});
            continue;
        }
        if (entryError || !iterator->is_regular_file(entryError) ||
            entryError || !service::ImportService::Classify(path).has_value())
        {
            continue;
        }
        entries.push_back({path, false});
    }

    std::sort(entries.begin(), entries.end(), [](const Entry& left,
                                                 const Entry& right) {
        if (left.directory != right.directory)
        {
            return left.directory > right.directory;
        }
        return AsciiLower(WideToUtf8(left.path.filename().wstring())) <
               AsciiLower(WideToUtf8(right.path.filename().wstring()));
    });

    if (!BrowsePathsEqual(browsePath_, libraryRoot_))
    {
        Resource parent;
        parent.id = BrowseSyntheticId(browsePath_.parent_path());
        parent.type = ResourceType::kFolder;
        parent.path = browsePath_.parent_path();
        parent.originalFilename = "..";
        parent.title = "返回上一级";
        browseItems_.push_back(std::move(parent));
    }

    for (const Entry& entry : entries)
    {
        if (entry.directory)
        {
            Resource folder;
            folder.id = BrowseSyntheticId(entry.path);
            folder.type = ResourceType::kFolder;
            folder.path = entry.path;
            folder.originalFilename =
                WideToUtf8(entry.path.filename().wstring());
            folder.title = folder.originalFilename;
            folder.addedTime = static_cast<std::int64_t>(std::time(nullptr));
            folder.modifiedTime = folder.addedTime;
            browseItems_.push_back(std::move(folder));
            continue;
        }

        Resource media;
        bool indexed = false;
        for (const Resource& candidate : items_)
        {
            if (BrowsePathsEqual(candidate.path, entry.path))
            {
                media = candidate;
                indexed = true;
                break;
            }
        }
        if (!indexed)
        {
            const std::optional<ResourceType> type =
                service::ImportService::Classify(entry.path);
            if (!type.has_value())
            {
                continue;
            }
            media.id = BrowseSyntheticId(entry.path);
            media.type = *type;
            media.path = entry.path;
            media.originalFilename =
                WideToUtf8(entry.path.filename().wstring());
            media.title = WideToUtf8(entry.path.stem().wstring());
            std::error_code sizeError;
            media.fileSize = std::filesystem::file_size(entry.path, sizeError);
            media.addedTime = static_cast<std::int64_t>(std::time(nullptr));
            media.modifiedTime = media.addedTime;
        }
        browseItems_.push_back(std::move(media));
    }
    sidebarCounts_[kBrowseNavigationIndex] = browseItems_.size();
    statusMessage_ = L"浏览：" + browsePath_.wstring();
}

D2D1_RECT_F Application::SidebarRect() const
{
    RECT client = {};
    GetClientRect(hwnd_, &client);
    const float s = scale_;
    const float top = (kCaptionBarDp + kTopBarDp + 1.0f) * s;
    const float bottom = static_cast<float>(client.bottom) -
                         (kStatusBarDp + 1.0f) * s;
    return {0.0f, top, kSidebarWidthDp * s, std::max(top, bottom)};
}

D2D1_RECT_F Application::SettingsRect() const
{
    const D2D1_RECT_F panel = SidebarRect();
    const float s = scale_;
    const float size = 42.0f * s;
    return {12.0f * s, panel.bottom - size - 12.0f * s,
            12.0f * s + size, panel.bottom - 12.0f * s};
}

void Application::DrawSidebar()
{
    const float s = scale_;
    const D2D1_RECT_F panel = SidebarRect();
    renderer_.FillRect(panel, render::Theme::kPanel);
    renderer_.FillRect({panel.right, panel.top, panel.right + s, panel.bottom},
                       render::Theme::kBorder);
    renderer_.DrawTextWide(navigationExpanded_ ? L"资源库  ▾" : L"资源库  ▸",
                           {18.0f * s, panel.top + 12.0f * s,
                                         panel.right - 18.0f * s,
                                         panel.top + 38.0f * s},
                           render::TextRole::kSecondary,
                           render::Theme::kTextSecondary);
    const D2D1_RECT_F settings = SettingsRect();
    const float listBottom = settings.top - 8.0f * s;
    const float listTop = panel.top + kSideListTopDp * s;
    const float navBottom = navigationExpanded_
        ? listTop + kSidebarNavigationCount * kSideItemHeightDp * s
        : listTop;
    const float tagsHeaderTop = navBottom + kSideSectionGapDp * s;
    const float tagsTop = tagsHeaderTop + 30.0f * s;
    renderer_.PushRectClip({panel.left, panel.top, panel.right, listBottom});
    static const wchar_t* const kNavigationLabels[] = {
        L"全部", L"视频", L"图片", L"收藏", L"浏览"};
    float y = listTop;
    for (int i = 0; navigationExpanded_ && i < kSidebarNavigationCount; ++i)
    {
        const D2D1_RECT_F row = {10.0f * s, y, panel.right - 10.0f * s,
                                 y + kSideItemHeightDp * s - 4.0f * s};
        const bool active = static_cast<int>(sidebarFilter_) == i &&
                            tagFilter_.empty();
        if (active || sideHover_ == i)
        {
            renderer_.FillRoundRect(
                row, active ? render::Theme::kAccent
                            : render::Theme::kCardHover,
                8.0f * s);
        }
        renderer_.DrawTextWide(
            kNavigationLabels[i],
            {22.0f * s, y, panel.right - 58.0f * s,
             y + kSideItemHeightDp * s},
            render::TextRole::kSecondary, render::Theme::kText);
        renderer_.DrawTextUtf8(
            std::to_string(sidebarCounts_[static_cast<size_t>(i)]),
            {panel.right - 52.0f * s, y, panel.right - 18.0f * s,
             y + kSideItemHeightDp * s},
            render::TextRole::kStatus, render::Theme::kTextSecondary);
        y += kSideItemHeightDp * s;
    }
    renderer_.DrawTextWide(tagsExpanded_ ? L"标签  ▾" : L"标签  ▸",
                           {18.0f * s, tagsHeaderTop, panel.right - 18.0f * s,
                            tagsHeaderTop + 26.0f * s},
                           render::TextRole::kStatus,
                           render::Theme::kTextSecondary);
    y = tagsTop - sideScrollY_;
    for (size_t i = 0; tagsExpanded_ && i < sideTags_.size(); ++i)
    {
        if (y + kSideItemHeightDp * s < tagsTop || y > listBottom)
        {
            y += kSideItemHeightDp * s;
            continue;
        }
        const bool active = sidebarFilter_ == SidebarFilter::kAll &&
                            tagFilter_ == sideTags_[i].name;
        const int hoverIndex = kSidebarNavigationCount +
                               static_cast<int>(i);
        const D2D1_RECT_F row = {10.0f * s, y, panel.right - 10.0f * s,
                                 y + kSideItemHeightDp * s - 4.0f * s};
        if (active || hoverIndex == sideHover_)
        {
            renderer_.FillRoundRect(row, active ? render::Theme::kAccent
                                                : render::Theme::kCardHover,
                                    8.0f * s);
        }
        renderer_.DrawTextUtf8(sideTags_[i].name,
                               {22.0f * s, y, panel.right - 56.0f * s,
                                y + kSideItemHeightDp * s},
                               render::TextRole::kSecondary,
                               render::Theme::kText);
        renderer_.DrawTextUtf8(std::to_string(sideTags_[i].count),
                               {panel.right - 52.0f * s, y,
                                panel.right - 18.0f * s,
                                y + kSideItemHeightDp * s},
                               render::TextRole::kStatus,
                               render::Theme::kTextSecondary);
        y += kSideItemHeightDp * s;
    }
    renderer_.PopRectClip();
    // Settings is an unobtrusive icon-only entry; keep its hit target but do
    // not paint a button background or outline over the sidebar.
    renderer_.DrawTextWide(L"⚙", settings, render::TextRole::kTitle,
                           render::Theme::kTextSecondary);
}

void Application::ClampSideScroll()
{
    const D2D1_RECT_F panel = SidebarRect();
    const D2D1_RECT_F settings = SettingsRect();
    const float s = scale_;
    const float listTop = panel.top + kSideListTopDp * s;
    const float navBottom = navigationExpanded_
        ? listTop + kSidebarNavigationCount * kSideItemHeightDp * s
        : listTop;
    const float tagsTop = navBottom + (kSideSectionGapDp + 30.0f) * s;
    const float viewport = std::max(0.0f, settings.top - tagsTop - 8.0f * s);
    const float total = tagsExpanded_
        ? static_cast<float>(sideTags_.size()) * kSideItemHeightDp * s
        : 0.0f;
    sideScrollY_ = std::clamp(sideScrollY_, 0.0f,
                              std::max(0.0f, total - viewport));
}

int Application::HitTestSidebar(POINT point) const
{
    const D2D1_RECT_F panel = SidebarRect();
    const float s = scale_;
    const float listTop = panel.top + kSideListTopDp * s;
    const float navBottom = navigationExpanded_
        ? listTop + kSidebarNavigationCount * kSideItemHeightDp * s
        : listTop;
    const float tagsHeaderTop = navBottom + kSideSectionGapDp * s;
    const float tagsTop = tagsHeaderTop + 30.0f * s;
    if (point.x < static_cast<LONG>(panel.left) ||
        point.x >= static_cast<LONG>(panel.right) ||
        point.y < static_cast<LONG>(panel.top) ||
        point.y >= static_cast<LONG>(SettingsRect().top - 8.0f * s))
    {
        return -1;
    }
    if (point.y >= static_cast<LONG>(panel.top) &&
        point.y < static_cast<LONG>(listTop))
    {
        return kSidebarNavigationHeader;
    }
    if (point.y >= static_cast<LONG>(tagsHeaderTop) &&
        point.y < static_cast<LONG>(tagsTop))
    {
        return kSidebarTagsHeader;
    }
    if (!navigationExpanded_ && point.y < static_cast<LONG>(tagsHeaderTop))
    {
        return -1;
    }
    if (point.y < static_cast<LONG>(navBottom))
    {
        const int index = static_cast<int>(
            (static_cast<float>(point.y) - listTop) /
            (kSideItemHeightDp * s));
        return index >= 0 && index < kSidebarNavigationCount ? index : -1;
    }
    if (point.y < static_cast<LONG>(tagsTop))
    {
        return -1;
    }
    if (!tagsExpanded_)
    {
        return -1;
    }
    const float y = static_cast<float>(point.y) - tagsTop + sideScrollY_;
    const int index = static_cast<int>(y / (kSideItemHeightDp * s));
    return index >= 0 && index < static_cast<int>(sideTags_.size())
        ? kSidebarNavigationCount + index
        : -1;
}

void Application::SeedEmptyLibrary()
{
    if (resources_.Count() > 0)
    {
        return;
    }
    if (seedWorker_.joinable())
    {
        return;
    }
    // First run: generate demo images and run them through the real import
    // pipeline so the archive/database flow is exercised end to end. The
    // potentially expensive GDI+ work stays off the UI thread.
    const bool deleteSource = settings_.deleteSourceAfterImport();
    seedWorker_ = std::thread([this, deleteSource] {
        const std::filesystem::path sampleDir = SampleAssets::SampleDir();
        SampleAssets::EnsureSampleImages(sampleDir);

        std::vector<std::filesystem::path> files;
        for (int i = 0; i < 18; ++i)
        {
            wchar_t fileName[32] = {};
            swprintf(fileName, 32, L"sample-%02d.png", i);
            std::filesystem::path path = sampleDir / fileName;
            if (std::filesystem::exists(path))
            {
                files.push_back(std::move(path));
            }
        }
        if (!files.empty())
        {
            service::ImportMetadata metadata;
            metadata.deleteSource = deleteSource;
            importer_.Import(std::move(files), std::move(metadata));
        }
    });
}

void Application::Refilter()
{
    EndDragSelection(true);
    if (sidebarFilter_ == SidebarFilter::kBrowse)
    {
        filtered_.clear();
        const std::wstring query = searchBar_.text();
        for (const Resource& item : browseItems_)
        {
            if (BrowseTextMatches(item, query))
            {
                filtered_.push_back(&item);
            }
        }
        ApplyResourceOrder();
        if (selectedId_ != 0 && SelectedIndex() < 0)
        {
            selectedId_ = 0;
        }
        std::unordered_set<ResourceId> visibleSelection;
        for (const Resource* item : filtered_)
        {
            if (selectedIds_.contains(item->id))
            {
                visibleSelection.insert(item->id);
            }
        }
        selectedIds_ = std::move(visibleSelection);
        hoveredId_ = 0;
        ClampScroll();
        Invalidate();
        return;
    }
    std::wstring query = searchBar_.text();
    auto prepend = [&query](const std::wstring& token) {
        if (!query.empty())
        {
            query.insert(0, L" ");
        }
        query.insert(0, token);
    };
    switch (sidebarFilter_)
    {
    case SidebarFilter::kVideo:
        prepend(L"type:video");
        break;
    case SidebarFilter::kImage:
        prepend(L"type:image");
        break;
    case SidebarFilter::kFavorite:
        prepend(L"favorite:true");
        break;
    case SidebarFilter::kAll:
    case SidebarFilter::kBrowse:
        break;
    }
    if (!tagFilter_.empty())
    {
        const std::wstring tag = Utf8ToWide(tagFilter_);
        if (tag.find_first_of(L" \t") != std::wstring::npos)
        {
            prepend(L"+tag:\"" + tag + L"\"");
        }
        else
        {
            prepend(L"+tag:" + tag);
        }
    }
    filtered_ = service::SearchService::Search(
        resources_, items_, query,
        [this](const std::string& word) { return tags_.ResolveWord(word); });
    ApplyResourceOrder();

    if (selectedId_ != 0 && SelectedIndex() < 0)
    {
        selectedId_ = 0;
    }
    std::unordered_set<ResourceId> visibleSelection;
    for (const Resource* item : filtered_)
    {
        if (selectedIds_.contains(item->id))
        {
            visibleSelection.insert(item->id);
        }
    }
    selectedIds_ = std::move(visibleSelection);
    hoveredId_ = 0;
    ClampScroll();
    Invalidate();
}

void Application::ClampScroll()
{
    const ui::GridView view = MakeView(ContentRect());
    const float total = ui::ResourceGrid::ContentHeight(view);
    const float viewport = view.content.bottom - view.content.top;
    const float maxScroll = std::max(0.0f, total - viewport);
    scrollY_ = std::clamp(scrollY_, 0.0f, maxScroll);
}

void Application::ApplyResourceOrder(bool reset)
{
    const bool browse = sidebarFilter_ == SidebarFilter::kBrowse;
    ui::ResourceOrder* order = &libraryOrder_;
    std::wstring* previous_query = &orderQuery_;
    if (browse)
    {
        auto& snapshot = browseOrders_[BrowsePathKey(browsePath_)];
        order = &snapshot.order;
        previous_query = &snapshot.query;
    }
    const auto& source = browse ? browseItems_ : items_;
    std::vector<const Resource*> all;
    all.reserve(source.size());
    for (const Resource& item : source)
    {
        all.push_back(&item);
    }
    const std::wstring query = searchBar_.text();
    if (reset || !order->initialized() ||
        (sortMode_ == SortMode::kRelevance && *previous_query != query))
    {
        std::vector<const Resource*> ranked = all;
        if (sortMode_ == SortMode::kRelevance && !browse)
        {
            // Sidebar filters take a subset of this search-only snapshot.
            ranked = service::SearchService::Search(
                resources_, items_, query,
                [this](const std::string& word)
                {
                    return tags_.ResolveWord(word);
                });
        }
        ui::SortResources(&ranked, sortMode_, browse);
        order->Reset(ranked);
        *previous_query = query;
    }
    order->Reconcile(all);
    order->Apply(&filtered_);
    if (browse)
    {
        // New folders still belong before media, in alphabetical order.
        ui::SortResources(&filtered_, SortMode::kRelevance, true);
    }
}

void Application::UpdateDragSelection(POINT pointer)
{
    if (!selectionDrag_.active())
    {
        return;
    }
    const auto content = ContentRect();
    selectionDrag_.Update(pointer, content, scrollY_);
    selectedIds_ = selectionDrag_.Select(ui::ResourceGrid::HitTestRect(
        MakeView(content), selectionDrag_.content_rect()));
    selectedId_ = 0;
    for (const Resource* item : filtered_)
    {
        if (selectedIds_.contains(item->id))
        {
            selectedId_ = item->id;
            break;
        }
    }
    UpdateSelectionTimer();
    Invalidate();
}

void Application::EndDragSelection(bool cancel)
{
    if (selectionTimerRunning_)
    {
        KillTimer(hwnd_, kSelectionTimer);
        selectionTimerRunning_ = false;
    }
    if (!selectionDrag_.active())
    {
        return;
    }
    if (cancel)
    {
        selectedIds_.clear();
        for (const Resource* item : filtered_)
        {
            if (selectionDrag_.original().contains(item->id))
            {
                selectedIds_.insert(item->id);
            }
        }
        selectedId_ = selectedIds_.contains(selectionDrag_.original_primary())
            ? selectionDrag_.original_primary()
            : selectedIds_.empty() ? 0 : *selectedIds_.begin();
    }
    // End before ReleaseCapture: WM_CAPTURECHANGED must not undo a commit.
    selectionDrag_.End();
    if (GetCapture() == hwnd_)
    {
        ReleaseCapture();
    }
    Invalidate();
}

void Application::UpdateSelectionTimer()
{
    const float step = selectionDrag_.ScrollStep(ContentRect(), scale_);
    const auto content = ContentRect();
    const float maximum = std::max(0.0f,
        ui::ResourceGrid::ContentHeight(MakeView(content)) -
            (content.bottom - content.top));
    const bool needed = selectionDrag_.active() &&
        ((step < 0.0f && scrollY_ > 0.0f) ||
         (step > 0.0f && scrollY_ < maximum));
    if (needed && !selectionTimerRunning_)
    {
        selectionTimerRunning_ = SetTimer(hwnd_, kSelectionTimer, 30, nullptr) != 0;
    }
    else if (!needed && selectionTimerRunning_)
    {
        KillTimer(hwnd_, kSelectionTimer);
        selectionTimerRunning_ = false;
    }
}

int Application::SelectedIndex() const
{
    for (size_t i = 0; i < filtered_.size(); ++i)
    {
        if (filtered_[i]->id == selectedId_)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int Application::ColumnCount() const
{
    const D2D1_RECT_F content = ContentRect();
    return ui::ResourceGrid::Columns(content.right - content.left, scale_);
}

void Application::MoveSelection(int delta)
{
    if (filtered_.empty() || delta == 0)
    {
        return;
    }
    int index = SelectedIndex();
    index = index < 0 ? 0
                      : std::clamp(index + delta, 0,
                                   static_cast<int>(filtered_.size()) - 1);
    selectedId_ = filtered_[index]->id;
    selectedIds_.clear();
    selectedIds_.insert(selectedId_);

    const D2D1_RECT_F content = ContentRect();
    const float stepY =
        ui::ResourceGrid::CellHeight(scale_) + kGapYDp * scale_;
    const int columns = ColumnCount();
    const float padding = kPaddingDp * scale_;
    const float rowTop =
        padding + static_cast<float>(index / columns) * stepY;
    const float viewport = content.bottom - content.top - 2.0f * padding;
    if (rowTop - scrollY_ < 0.0f)
    {
        scrollY_ = rowTop;
    }
    else if (rowTop + stepY - scrollY_ > viewport)
    {
        scrollY_ = rowTop + stepY - viewport;
    }
    ClampScroll();
    Invalidate();
}

void Application::OpenResource(Resource* item)
{
    if (item == nullptr)
    {
        return;
    }
    if (item->type == ResourceType::kFolder)
    {
        ShellExecuteW(hwnd_, L"open", item->path.c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
        return;
    }
    std::error_code existenceError;
    if (!std::filesystem::exists(item->path, existenceError) ||
        existenceError)
    {
        item->missing = true;
        resources_.SetMissing(item->id, true);
        MessageBoxW(hwnd_, L"文件缺失，无法打开。", L"Viewer", MB_ICONWARNING);
        Invalidate();
        return;
    }
    bool opened = false;
    if (item->type == ResourceType::kVideo)
    {
        opened = playback_.OpenVideo(hwnd_, item->path);
    }
    else
    {
        opened = playback_.OpenImage(hwnd_, item->path);
    }
    if (!opened)
    {
        MessageBoxW(hwnd_, L"无法启动关联的打开程序。", L"Viewer",
                    MB_ICONWARNING);
        return;
    }
    // History is written after the launch succeeded (section 49).
    std::int64_t lastOpenTime = 0;
    std::uint32_t totalOpenCount = 0;
    double hotScore = 0.0;
    if (resources_.NoteOpen(item->id) &&
        resources_.LoadOpenStats(item->id, &lastOpenTime, &totalOpenCount,
                                 &hotScore))
    {
        item->lastOpenTime = lastOpenTime;
        item->totalOpenCount = totalOpenCount;
        item->hotScore = hotScore;
        UpdateSidebarCounts();
        // Keep the current snapshot and viewport stable until an explicit sort.
        Invalidate();
    }
}

void Application::DeleteSelected(bool permanent)
{
    std::vector<service::DeleteItem> deleteItems;
    int videos = 0;
    int images = 0;
    std::uint64_t totalBytes = 0;
    for (const Resource& item : items_)
    {
        if (selectedIds_.contains(item.id) ||
            (selectedIds_.empty() && item.id == selectedId_))
        {
            deleteItems.push_back({item.id, item.path});
            totalBytes += item.fileSize;
            if (item.type == ResourceType::kVideo)
            {
                ++videos;
            }
            else
            {
                ++images;
            }
        }
    }
    if (deleteItems.empty())
    {
        return;
    }
    std::wostringstream message;
    message << (permanent ? L"永久删除 " : L"移到回收站：")
            << deleteItems.size() << L" 个资源？\n\n"
            << videos << L" 个视频，" << images << L" 张图片\n"
            << L"总大小：" << (totalBytes / (1024 * 1024)) << L" MB";
    if (permanent)
    {
        message << L"\n\n此操作不可恢复。";
    }
    if (!ui::DeleteDialog::Show(hwnd_, message.str(), permanent))
    {
        return;
    }
    deleter_.Delete(std::move(deleteItems),
                    permanent ? service::DeleteMode::kPermanent
                              : service::DeleteMode::kRecycle);
}

void Application::ToggleSelectedFavorite()
{
    std::vector<Resource*> selected;
    for (Resource& item : items_)
    {
        if (selectedIds_.contains(item.id) ||
            (selectedIds_.empty() && item.id == selectedId_))
        {
            selected.push_back(&item);
        }
    }
    if (selected.empty())
    {
        return;
    }
    const bool favorite = !selected.front()->favorite;
    if (!database_.Exec("BEGIN;"))
    {
        return;
    }
    bool ok = true;
    for (Resource* item : selected)
    {
        ok = ok && resources_.SetFavorite(item->id, favorite);
    }
    if (!ok || !database_.Exec("COMMIT;"))
    {
        database_.Exec("ROLLBACK;");
        return;
    }
    for (Resource* item : selected)
    {
        item->favorite = favorite;
    }
    UpdateSidebarCounts();
    Refilter();
}

/*
 * Keep the selection operation above transactionally consistent. This
 * helper is intentionally not exposed as a separate service because it only
 * mutates the already loaded UI selection.
 */
void Application::EditSelectedMetadata()
{
    std::vector<Resource*> selected;
    for (Resource& candidate : items_)
    {
        if (selectedIds_.contains(candidate.id) ||
            (selectedIds_.empty() && candidate.id == selectedId_))
        {
            selected.push_back(&candidate);
        }
    }
    if (selected.empty())
    {
        return;
    }

    repo::CategoryRepository categories(database_);
    std::vector<std::string> categoryNames;
    for (const repo::Category& category : categories.LoadAll())
    {
        categoryNames.push_back(category.name);
    }
    service::ImportMetadata initial;
    const bool single = selected.size() == 1;
    if (single)
    {
        initial.title = selected.front()->title;
        initial.category = selected.front()->categoryName;
        initial.tags = selected.front()->tags;
        initial.actors = selected.front()->actors;
        initial.favorite = selected.front()->favorite;
        initial.overrideTitle = true;
    }
    std::vector<std::filesystem::path> files;
    for (const Resource* resource : selected)
    {
        files.push_back(resource->path);
    }
    auto edited = ui::ImportDialog::Show(
        hwnd_, files, categoryNames, BuildQuickTags(settings_, tags_),
        single ? &initial : nullptr, true);
    if (!edited.has_value())
    {
        return;
    }

    repo::ActorRepository actors(database_);
    bool ok = database_.Exec("BEGIN;");
    std::int64_t categoryId = 0;
    if (ok && !edited->category.empty())
    {
        categoryId = categories.FindOrCreate(edited->category);
        ok = categoryId != 0;
    }
    for (Resource* resource : selected)
    {
        const std::string& title = single ? edited->title : resource->title;
        ok = ok && resources_.UpdateMetadata(
                       resource->id, title, edited->favorite) &&
             tags_.ClearForResource(resource->id) &&
             actors.ClearForResource(resource->id);
        if (ok)
        {
            ok = categoryId == 0 ? categories.Clear(resource->id)
                                 : categories.Assign(resource->id, categoryId);
        }
        for (const std::string& tag : edited->tags)
        {
            ok = ok && tags_.Attach(resource->id, tag);
        }
        for (const std::string& actor : edited->actors)
        {
            ok = ok && actors.Attach(resource->id, actor);
        }
    }
    if (!ok || !database_.Exec("COMMIT;"))
    {
        database_.Exec("ROLLBACK;");
        MessageBoxW(hwnd_, L"Metadata 保存失败，原数据保持不变。", L"Viewer",
                    MB_ICONERROR);
        return;
    }
    ReloadFromDatabase();
    Refilter();
    Invalidate();
}

void Application::MissingWorkerLoop()
{
    db::Database database;
    if (!database.Open(exeDir_ / L"data" / L"nexus.db"))
    {
        return;
    }
    if (!db::Migrate(database))
    {
        return;
    }
    repo::ResourceRepository resources(database);

    // Older databases may contain video rows created before FFmpeg probing
    // was enabled.  Repair those durations once in the background so the
    // thumbnail badge appears without requiring a re-import or a manual scan.
    bool durationChanged = false;
    for (const Resource& resource : resources.LoadAll())
    {
        if (stopMissingWorker_ || resource.type != ResourceType::kVideo ||
            resource.duration > 0 || resource.missing)
        {
            continue;
        }
        std::error_code error;
        if (!std::filesystem::is_regular_file(resource.path, error) || error)
        {
            continue;
        }
        const std::int64_t duration =
            service::DetectMediaDuration(resource.path);
        if (duration > 0 && resources.UpdateDuration(resource.id, duration))
        {
            durationChanged = true;
        }
    }
    if (durationChanged && IsWindow(hwnd_))
    {
        PostMessageW(hwnd_, kMissingUpdatedMsg, 0, 0);
    }

    while (!stopMissingWorker_)
    {
        if (resources.RefreshMissing())
        {
            PostMessageW(hwnd_, kMissingUpdatedMsg, 0, 0);
        }
        for (int i = 0; i < 50 && !stopMissingWorker_; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void Application::ChooseSortMode()
{
    const D2D1_RECT_F sortRect = SortRect();
    POINT anchor = {static_cast<LONG>(sortRect.left),
                    static_cast<LONG>(sortRect.bottom + 4.0f * scale_)};
    ClientToScreen(hwnd_, &anchor);
    const int selected = SortPopup::Show(hwnd_, anchor, scale_,
                                         static_cast<int>(sortMode_));
    if (selected < 0)
    {
        return;
    }
    sortMode_ = static_cast<SortMode>(selected);
    libraryOrder_ = ui::ResourceOrder();
    browseOrders_.clear();
    ApplyResourceOrder(true);
    Refilter();
}

void Application::StartBackgroundBuild()
{
    // A previous directory pass may have finished but still own a joinable
    // thread.  Stop it before starting a new pass so two workers never race
    // to enrich the same rows or warm the same thumbnail queue.
    stopLibraryScanWorker_ = true;
    if (libraryScanWorker_.joinable())
    {
        libraryScanWorker_.join();
    }
    stopLibraryScanWorker_ = false;

    const std::filesystem::path scanRoot = libraryRoot_;
    const std::filesystem::path scanDatabase = exeDir_ / L"data" / L"nexus.db";
    statusMessage_ = L"正在后台构建缓存…";
    libraryScanWorker_ = std::thread([this, scanRoot, scanDatabase] {
        const HWND notifyWindow = hwnd_;
        const UINT readyMessage = kLibraryScanReadyMsg;
        const auto stopped = [this] {
            return stopLibraryScanWorker_.load();
        };
        const auto notifyReady = [notifyWindow, readyMessage, stopped] {
            if (!stopped() && IsWindow(notifyWindow))
            {
                PostMessageW(notifyWindow, readyMessage, 0, 0);
            }
        };

        // Discover newly copied files cheaply first.  Each inserted row is
        // immediately visible after the ready notification; expensive probes
        // are deferred to RunIncremental below.
        service::ScanService::RunLightweight(
            scanDatabase, scanRoot,
            [notifyReady] { notifyReady(); }, stopped);
        if (stopped())
        {
            return;
        }
        notifyReady();

        service::ScanService::RunIncremental(
            scanDatabase, scanRoot, 24,
            [notifyReady](const service::ScanResult&) { notifyReady(); },
            stopped);
        if (stopped())
        {
            return;
        }

        // Warm the disk thumbnail cache in small pages.  Request() is
        // thread-safe and only enqueues work; the dedicated thumbnail
        // workers perform WIC/Shell decoding without blocking this pass.
        db::Database cacheDatabase;
        if (cacheDatabase.Open(scanDatabase) && db::Migrate(cacheDatabase))
        {
            repo::ResourceRepository cacheResources(cacheDatabase);
            ResourceId afterId = 0;
            while (!stopped())
            {
                const std::vector<Resource> batch =
                    cacheResources.LoadMediaBatch(32, afterId);
                if (batch.empty())
                {
                    break;
                }
                for (const Resource& resource : batch)
                {
                    afterId = std::max(afterId, resource.id);
                    if (!stopped())
                    {
                        thumbnails_.Request(resource);
                    }
                }
                // Keep the queue from monopolising disk bandwidth while the
                // user is already browsing the first page.
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        if (!stopped() && IsWindow(notifyWindow))
        {
            PostMessageW(notifyWindow, kLibraryScanDoneMsg, 0, 0);
        }
    });
}

void Application::ClearCacheAndRebuild()
{
    EndDragSelection(true);
    libraryOrder_ = ui::ResourceOrder();
    browseOrders_.clear();
    // Quiesce workers before changing the shared database and cache
    // directories. Shutdown drains queued imports/deletes, so no source file
    // is removed by this action and no worker can recreate a thumbnail after
    // the directory has been cleared.
    stopLibraryScanWorker_ = true;
    if (libraryScanWorker_.joinable())
    {
        libraryScanWorker_.join();
    }
    // MissingWorkerLoop owns a separate SQLite connection and may still be
    // probing durations or refreshing missing flags.  Stop and join it
    // before clearing the shared index; assigning a new std::thread while the
    // old one is joinable would terminate the process in the CRT, and the
    // worker could otherwise race the DELETE statements below.
    stopMissingWorker_ = true;
    if (missingWorker_.joinable())
    {
        missingWorker_.join();
    }
    importer_.Shutdown();
    deleter_.Shutdown();
    thumbnails_.Shutdown();

    bool cleared = database_.Exec("BEGIN;") &&
        database_.Exec("DELETE FROM resource_fts;") &&
        database_.Exec("DELETE FROM resources;") &&
        database_.Exec("DELETE FROM resource_tags;") &&
        database_.Exec("DELETE FROM tag_alias;") &&
        database_.Exec("DELETE FROM tags;") &&
        database_.Exec("DELETE FROM resource_actors;") &&
        database_.Exec("DELETE FROM actors;") &&
        database_.Exec("DELETE FROM categories;") &&
        database_.Exec("DELETE FROM open_history;") &&
        database_.Exec("DELETE FROM operation_log;") &&
        database_.Exec("DELETE FROM sqlite_sequence WHERE name='resources';") &&
        database_.Exec("COMMIT;");
    if (!cleared)
    {
        database_.Exec("ROLLBACK;");
    }

    thumbnails_.ClearCache();
    const std::filesystem::path databasePath =
        exeDir_ / L"data" / L"nexus.db";
    importer_.Start(hwnd_, kImportDoneMsg, kImportProgressMsg);
    deleter_.Start(hwnd_, kDeleteDoneMsg);
    thumbnails_.Start(hwnd_, kThumbReadyMsg,
                      exeDir_ / L"cache" / L"thumbnails");
    stopMissingWorker_ = false;
    // The previous worker was joined above, so this assignment is always to
    // a non-joinable thread object.
    missingWorker_ = std::thread([this] { MissingWorkerLoop(); });
    selectedId_ = 0;
    selectedIds_.clear();
    browseItems_.clear();
    ReloadFromDatabase();
    Refilter();
    if (!cleared)
    {
        statusMessage_ = L"清空缓存失败，未开始重建";
        MessageBoxW(hwnd_, L"清空 SQLite 数据失败，原文件未受影响。",
                    L"Viewer", MB_ICONERROR);
        Invalidate();
        return;
    }
    statusMessage_ = L"缓存已清空，正在重建索引…";
    StartBackgroundBuild();
    (void)databasePath;
}

void Application::ShowSettings()
{
    bool clearCacheRequested = false;
    if (ui::SettingsDialog::Show(hwnd_, &settings_, &clearCacheRequested))
    {
        libraryRoot_ = settings_.managedLibrary().empty()
            ? exeDir_ / L"NexusLibrary"
            : std::filesystem::path(settings_.managedLibrary());
        browsePath_ = libraryRoot_;
        archive_.SetLibraryRoot(libraryRoot_);
        if (settings_.Save(exeDir_ / L"nexus.toml"))
        {
            if (clearCacheRequested)
            {
                ClearCacheAndRebuild();
            }
            else
            {
                // Re-index the selected managed folder in the same segmented
                // startup pipeline. Existing metadata is preserved and files
                // not yet present in SQLite become visible after the
                // lightweight page commits.
                StartBackgroundBuild();
            }
        }
        else
        {
            statusMessage_ = L"设置保存失败";
            MessageBoxW(hwnd_, L"设置保存失败，本次运行仍使用新设置。",
                        L"Viewer", MB_ICONWARNING);
        }
        Invalidate();
    }
}

void Application::Invalidate()
{
    InvalidateRect(hwnd_, nullptr, FALSE);
}

D2D1_RECT_F Application::ContentRect() const
{
    RECT clientRect = {};
    GetClientRect(hwnd_, &clientRect);
    const float width =
        static_cast<float>(std::max(0L, clientRect.right - clientRect.left));
    const float height =
        static_cast<float>(std::max(0L, clientRect.bottom - clientRect.top));
    const float s = scale_;
    const float top = (kCaptionBarDp + kTopBarDp + 1.0f) * s;
    const float bottom = height - (kStatusBarDp + 1.0f) * s;
    return {kSidebarWidthDp * scale_, top, width, std::max(top, bottom)};
}

std::optional<D2D1_RECT_F> Application::CardScrollThumbRect() const
{
    if (filtered_.empty())
    {
        return std::nullopt;
    }
    const D2D1_RECT_F content = ContentRect();
    const ui::GridView view = MakeView(content);
    const float total = ui::ResourceGrid::ContentHeight(view);
    const float viewport = content.bottom - content.top;
    if (total <= viewport || viewport <= 0.0f)
    {
        return std::nullopt;
    }

    const float thumbHeight = std::min(
        viewport, std::max(30.0f * scale_, viewport * viewport / total));
    const float maxScroll = total - viewport;
    const float travel = std::max(0.0f, viewport - thumbHeight);
    const float fraction = maxScroll > 0.0f
        ? std::clamp(scrollY_ / maxScroll, 0.0f, 1.0f)
        : 0.0f;
    const float y = content.top + travel * fraction;
    return D2D1_RECT_F{content.right - 5.0f * scale_, y,
                       content.right - 2.0f * scale_, y + thumbHeight};
}

void Application::UpdateCardScrollFromPointer(float pointerY)
{
    const auto thumb = CardScrollThumbRect();
    if (!thumb.has_value())
    {
        return;
    }
    const D2D1_RECT_F content = ContentRect();
    const float viewport = content.bottom - content.top;
    const float thumbHeight = thumb->bottom - thumb->top;
    const float travel = std::max(0.0f, viewport - thumbHeight);
    const ui::GridView view = MakeView(content);
    const float total = ui::ResourceGrid::ContentHeight(view);
    const float maxScroll = std::max(0.0f, total - viewport);
    if (travel <= 0.0f || maxScroll <= 0.0f)
    {
        scrollY_ = 0.0f;
    }
    else
    {
        const float top = std::clamp(
            pointerY - cardScrollDragOffset_, content.top,
            content.bottom - thumbHeight);
        scrollY_ = ((top - content.top) / travel) * maxScroll;
        ClampScroll();
    }
    Invalidate();
}

D2D1_RECT_F Application::SearchRect() const
{
    RECT clientRect = {};
    GetClientRect(hwnd_, &clientRect);
    const float width =
        static_cast<float>(std::max(0L, clientRect.right - clientRect.left));
    const float s = scale_;
    const float titleHeight = kCaptionBarDp * s;
    const float left = 16.0f * s;
    const float right = width - kSortWidthDp * s - 32.0f * s;
    return {left, titleHeight + 8.0f * s,
            std::max(left + 40.0f * s, right),
            titleHeight + kTopBarDp * s - 8.0f * s};
}

D2D1_RECT_F Application::SortRect() const
{
    RECT clientRect = {};
    GetClientRect(hwnd_, &clientRect);
    const float width =
        static_cast<float>(std::max(0L, clientRect.right - clientRect.left));
    const float s = scale_;
    const float titleHeight = kCaptionBarDp * s;
    const float right = width - 16.0f * s;
    return {right - kSortWidthDp * s, titleHeight + 4.0f * s, right,
            titleHeight + kTopBarDp * s - 4.0f * s};
}

void Application::ToggleMaximize()
{
    ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
    Invalidate();
}

void Application::SetHeaderHover(int index)
{
    if (headerHover_ == index)
    {
        return;
    }
    headerHover_ = index;
    Invalidate();
}

void Application::UpdateHeaderHover(POINT point)
{
    RECT client = {};
    GetClientRect(hwnd_, &client);
    const float s = scale_;
    const float single = kCaptionButtonDp * s;
    int hover = -1;
    if (point.y < static_cast<LONG>(kCaptionBarDp * s) &&
        point.x >= client.right - static_cast<LONG>(3.0f * single) &&
        point.x < client.right)
    {
        hover = point.x < client.right - static_cast<LONG>(2.0f * single)
            ? 0
            : point.x < client.right - static_cast<LONG>(single) ? 1 : 2;
    }
    SetHeaderHover(hover);
    if (!trackingMouse_)
    {
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd_, 0};
        trackingMouse_ = TrackMouseEvent(&track) != FALSE;
    }
}

void Application::DrawCaptionButtons(float width, float topHeight)
{
    const float s = scale_;
    const float single = kCaptionButtonDp * s;
    for (int i = 0; i < 3; ++i)
    {
        const float left = width - (3 - i) * single;
        const bool hovered = headerHover_ == i;
        const D2D1_RECT_F buttonRect = {left, 0.0f, left + single, topHeight};
        if (hovered)
        {
            renderer_.FillRect(buttonRect, i == 2 ? render::Theme::kDanger
                                                  : render::Theme::kCardHover);
        }
        const float centerX = left + single / 2.0f;
        const float centerY = topHeight / 2.0f;
        const D2D1_COLOR_F glyph = render::Theme::kText;
        if (i == 0)
        {
            renderer_.FillRect({centerX - 5.0f * s, centerY - 0.5f * s,
                                centerX + 5.0f * s, centerY + 1.0f * s},
                               glyph);
        }
        else if (i == 1)
        {
            if (IsZoomed(hwnd_))
            {
                // Restore glyph: back frame, then front frame with a mask
                // fill so the two frames do not merge.
                renderer_.StrokeRect(
                    {centerX - 2.5f * s, centerY - 4.5f * s,
                     centerX + 5.0f * s, centerY + 3.0f * s},
                    glyph, 1.0f * s);
                renderer_.FillRect(
                    {centerX - 5.0f * s, centerY - 2.0f * s,
                     centerX + 2.5f * s, centerY + 4.5f * s},
                    hovered ? render::Theme::kCardHover
                            : render::Theme::kPanel);
                renderer_.StrokeRect(
                    {centerX - 5.0f * s, centerY - 2.0f * s,
                     centerX + 2.5f * s, centerY + 4.5f * s},
                    glyph, 1.0f * s);
            }
            else
            {
                renderer_.StrokeRect(
                    {centerX - 4.5f * s, centerY - 4.0f * s,
                     centerX + 4.5f * s, centerY + 4.0f * s},
                    glyph, 1.0f * s);
            }
        }
        else
        {
            const std::wstring close = L"✕";
            const float glyphWidth =
                renderer_.MeasureTextWide(close, render::TextRole::kTitle);
            renderer_.DrawTextWide(
                close,
                {centerX - glyphWidth / 2.0f, centerY - 7.0f * s,
                 centerX + glyphWidth / 2.0f, centerY + 7.0f * s},
                render::TextRole::kTitle, glyph);
        }
    }
}

ui::GridView Application::MakeView(D2D1_RECT_F content) const
{
    ui::GridView view;
    view.items = &filtered_;
    view.hovered = hoveredId_;
    view.selected = selectedId_;
    view.selectedItems = &selectedIds_;
    view.content = content;
    view.scrollY = scrollY_;
    view.scale = scale_;
    if (selectionDrag_.active())
    {
        view.selectionRect = selectionDrag_.window_rect(content, scrollY_);
    }
    return view;
}

}  // namespace nexus::app

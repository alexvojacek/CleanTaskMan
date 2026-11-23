#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include "telemetry.hpp"
#include "settings.hpp"
#include "process_actions.hpp"
#include "integration.hpp"
#include "navigation.hpp"
#include "ui_theme.hpp"

namespace {
template <class T> struct Com {
    T* p = nullptr;
    ~Com() {
        reset();
    }
    void reset() {
        if (p) {
            p->Release();
            p = nullptr;
        }
    }
    T** put() {
        reset();
        return &p;
    }
    T* operator->() const {
        return p;
    }
    explicit operator bool() const {
        return p != nullptr;
    }
};
struct Box {
    float x, y, w, h;
    D2D1_RECT_F rect() const {
        return D2D1::RectF(x, y, x + w, y + h);
    }
    bool has(float px, float py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};
struct Hit {
    int id;
    Box box;
    std::wstring tip;
};
HICON nativeAppIcon() {
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = 32;
    header.bV5Height = -32;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000;
    header.bV5GreenMask = 0x0000ff00;
    header.bV5BlueMask = 0x000000ff;
    header.bV5AlphaMask = 0xff000000;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS,
                                      &pixels, nullptr, 0);
    if (!bitmap || !pixels) {
        if (bitmap)
            DeleteObject(bitmap);
        return nullptr;
    }
    const POINT wave[] = {{5, 19}, {10, 19}, {14, 9}, {18, 24}, {23, 14}, {27, 14}};
    auto* data = static_cast<DWORD*>(pixels);
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            bool inside = x >= 2 && x < 30 && y >= 4 && y < 28;
            double distance = 100;
            for (size_t i = 1; i < std::size(wave); i++) {
                double dx = wave[i].x - wave[i - 1].x, dy = wave[i].y - wave[i - 1].y;
                double t = std::clamp(((x + .5 - wave[i - 1].x) * dx + (y + .5 - wave[i - 1].y) * dy) /
                                          (dx * dx + dy * dy),
                                      0.0, 1.0);
                distance = std::min(
                    distance, std::hypot(x + .5 - wave[i - 1].x - t * dx, y + .5 - wave[i - 1].y - t * dy));
            }
            double blend = std::clamp(1.65 - distance, 0.0, 1.0);
            unsigned r = static_cast<unsigned>(0x34 + (0xc1 - 0x34) * blend),
                     g = static_cast<unsigned>(0x8b + (0xec - 0x8b) * blend),
                     b = static_cast<unsigned>(0xbf + (0xff - 0xbf) * blend);
            data[y * 32 + x] = inside ? (0xff000000 | (r << 16) | (g << 8) | b) : 0;
        }
    BYTE mask[128]{};
    HBITMAP monochrome = CreateBitmap(32, 32, 1, 1, mask);
    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmMask = monochrome;
    info.hbmColor = bitmap;
    HICON icon = monochrome ? CreateIconIndirect(&info) : nullptr;
    if (monochrome)
        DeleteObject(monochrome);
    DeleteObject(bitmap);
    return icon;
}
D2D1_COLOR_F color(unsigned rgb, float alpha = 1) {
    return D2D1::ColorF(rgb, alpha);
}
std::wstring number(double value, int decimals = 1, const wchar_t* suffix = L"") {
    if (!std::isfinite(value))
        return L"";
    wchar_t b[100];
    swprintf(b, 100, L"%.*f%ls", std::clamp(decimals, 0, 6), value, suffix);
    return b;
}
std::wstring capacity(double value) {
    if (!std::isfinite(value) || value < 0)
        return L"";
    if (value >= 1099511627776.0)
        return number(value / 1099511627776.0, 1, L" TB");
    if (value >= 1073741824.0)
        return number(value / 1073741824.0, 1, L" GB");
    if (value >= 1048576.0)
        return number(value / 1048576.0, 1, L" MB");
    return number(value / 1024.0, 0, L" KB");
}
std::wstring throughput(double value, bool bits = false) {
    if (!std::isfinite(value))
        return L"";
    if (bits)
        return value * 8 >= 1000000 ? number(value * 8 / 1000000, 1, L" Mbps")
                                    : number(value * 8 / 1000, 1, L" Kbps");
    return value >= 1048576 ? number(value / 1048576, 1, L" MB/s") : number(value / 1024, 1, L" KB/s");
}
std::wstring memoryFraction(double used, uint64_t total) {
    return std::isfinite(used) && total ? capacity(used) + L"/" + capacity(total) : L"";
}
std::wstring uptime(uint64_t ms) {
    auto sec = ms / 1000;
    wchar_t b[80];
    swprintf(b, 80, L"%llu:%02llu:%02llu:%02llu", sec / 86400, (sec / 3600) % 24, (sec / 60) % 60, sec % 60);
    return b;
}
std::wstring lower(std::wstring s) {
    if (!s.empty())
        CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}
struct Row {
    std::vector<std::wstring> cells;
    int icon = 0;
    std::wstring key;
};
struct Column {
    std::wstring label;
    float width;
    bool right = false;
};
struct Table {
    std::vector<Column> columns;
    std::vector<Row> rows;
};

Table makeTable(int page, const telemetry::CoreSnapshot& core, const telemetry::DeviceSnapshot& devices) {
    if (page == 2)
        return {{{L"Name", 260},
                 {L"CPU time", 110, true},
                 {L"Network", 110, true},
                 {L"Metered network", 130, true},
                 {L"Tile updates", 110, true}},
                {}};
    if (page == 3) {
        Table t{{{L"Name", 260}, {L"Publisher", 180}, {L"Status", 110}, {L"Startup impact", 130}}, {}};
        for (auto& s : devices.startup)
            t.rows.push_back({{s.name, L"", s.status, L""}, 0, s.key});
        return t;
    }
    if (page == 4) {
        Table t{{{L"User", 260},
                 {L"Status", 110},
                 {L"CPU", 80, true},
                 {L"Memory", 110, true},
                 {L"Disk", 100, true},
                 {L"Network", 100, true}},
                {}};
        for (auto& u : devices.users) {
            double cpu = 0, mem = 0;
            bool any = false, cpuKnown = true, memKnown = true;
            unsigned count = 0;
            for (auto& p : core.processes)
                if (p.session == u.session && p.pid) {
                    any = true;
                    count++;
                    if (std::isfinite(p.cpu))
                        cpu += p.cpu;
                    else
                        cpuKnown = false;
                    if (std::isfinite(p.memory))
                        mem += p.memory;
                    else
                        memKnown = false;
                }
            t.rows.push_back({{u.name + (core.ready ? L" (" + std::to_wstring(count) + L")" : L""), u.status,
                               number(any && cpuKnown ? cpu : telemetry::unknown, 1, L"%"),
                               number(any && memKnown ? mem : telemetry::unknown, 1, L" MB"), L"", L""},
                              6,
                              u.key});
        }
        return t;
    }
    if (page == 5) {
        Table t{{{L"Name", 240},
                 {L"PID", 70, true},
                 {L"Status", 90},
                 {L"User name", 120},
                 {L"CPU", 65, true},
                 {L"Memory (active private working set)", 180, true},
                 {L"Architecture", 100}},
                {}};
        for (auto& p : core.processes)
            t.rows.push_back({{p.name, std::to_wstring(p.pid), L"Running", p.user.empty() ? L"" : p.user,
                               number(p.cpu, 1, L"%"), number(p.memory, 1, L" MB"), p.architecture},
                              p.icon,
                              p.born ? std::to_wstring(p.pid) + L":" + std::to_wstring(p.born) : L""});
        return t;
    }
    Table t{{{L"Name", 180}, {L"PID", 75, true}, {L"Description", 350}, {L"Status", 100}, {L"Group", 140}},
            {}};
    for (auto& s : devices.services)
        t.rows.push_back(
            {{s.name, s.pid ? std::to_wstring(s.pid) : L"", s.description, s.status, L""}, 0, s.name});
    return t;
}
class App {
  public:
    HWND window = nullptr, search = nullptr, canvas = nullptr;
    float scale = 1, width = 1180, height = 780;
    int page = 0, device = 0, hover = -1, pressed = -1, selected = -1, sortColumn = -1, themeMode = 0;
    bool navExpanded = false, sortAscending = true, isDark = true, keyboard = false, tracking = false,
         firstDraw = false;
    bool draggingScroll = false;
    int focusArea = 1, navFocus = 0;
    float scroll = 0, hscroll = 0, contentHeight = 0, viewHeight = 0, contentWidth = 0;
    float dragStartY = 0, dragStartScroll = 0;
    std::wstring query, tooltip, benchmarkFile;
    std::set<std::wstring> expanded;
    std::array<bool, 3> collapsed{};
    std::array<int, 8> selections{};
    std::vector<int> visibleRows;
    std::vector<Hit> hits;
    std::unique_ptr<telemetry::Collector> collector = std::make_unique<telemetry::Collector>();
    std::shared_ptr<const telemetry::CoreSnapshot> core = std::make_shared<telemetry::CoreSnapshot>();
    std::shared_ptr<const telemetry::DeviceSnapshot> devices = std::make_shared<telemetry::DeviceSnapshot>();
    std::vector<telemetry::Process> processes;
    struct ProcessRow {
        telemetry::Process value;
        std::vector<telemetry::Process> members;
        std::wstring key, groupKey;
        int depth = 0;
    };
    std::vector<ProcessRow> processRows;
    settings::Values preferences;
    settings::Values persistedPreferences;
    settings::Writer settingsWriter;
    int defaultTab = 0, cpuGraphMode = 0;
    bool runAdmin = true;
    bool adminActive = false;
    unsigned updateInterval = 1000;
    bool closeToTray = false, trayHidden = false, trayPresent = false, forceExit = false;
    bool metricsPaused = false, collectorStopping = false;
    std::thread collectorJoin;
    std::atomic<uint64_t> collectorGeneration{0};
    UINT restoreMsg = 0, taskbarCreatedMsg = 0;
    bool loggedCpu = false, loggedClock = false;
    integration::TaskManager taskManager;
    integration::ReplacementState replacement;
    std::wstring ownedDebugger;
    bool desiredReplacement = false, replacementPending = false, preferencesDirty = false;
    bool placementQueued = false, handoffExit = false;
    settings::Writer placementWriter;
    std::optional<settings::Placement> placementBaseline;
    std::wstring saveStatus;
    int lastContentPage = 0;
    RECT normalRect{};
    bool savedMaximized = false;
    float hitOffsetY = 0;
    std::array<int, 8> pageSort{{-1, -1, -1, -1, -1, -1, -1, -1}};
    std::array<bool, 8> pageAscending{{true, true, true, true, true, true, true, true}};
    actions::Processes processActions;
    std::thread actionThread;
    std::atomic<bool> actionBusy{false};
    std::mutex actionMutex;
    std::wstring actionError;
    std::shared_ptr<std::atomic<bool>> notificationPending = std::make_shared<std::atomic<bool>>(false);
    std::atomic<bool> closing{false};
    unsigned sensorDelay = 0, exitAfter = 0;
    std::wstring diagnosticsFile, deviceKey = L"cpu";
    float deviceScroll = 0;
    bool collectorsStarted = false;
    uint64_t drawCount = 0;
    int pressedPage = -1;
    std::wstring pressedKey;
    uint64_t pressedCoreRevision = 0, pressedDeviceRevision = 0;
    std::array<std::wstring, 8> selectionKeys;
    Com<ID2D1Factory> factory;
    Com<IDWriteFactory> writer;
    Com<ID2D1HwndRenderTarget> target;
    Com<ID2D1SolidColorBrush> brush;
    std::map<int, IDWriteTextFormat*> fonts;
    HFONT editFont = nullptr;
    int editFontPixels = 0;
    float pointerX = 0, pointerY = 0;
    bool pointerKnown = false;
    HBRUSH editBrush = nullptr;
    HICON appIcon = nullptr;
    LARGE_INTEGER start{}, frequency{};
    ui::Palette palette = ui::dark;
    float navWidth() const {
        return navExpanded ? ui::expandedNav : ui::compactNav;
    }
    // Keep device and graph anchors stationary during a right-edge resize.
    float performanceRailWidth() const {
        return 222;
    }
    static constexpr float performanceRowPadding = 2, performanceRowHeight = 60 + performanceRowPadding * 2;
    static std::wstring performanceTitle(const telemetry::Device& d) {
        return d.kind == telemetry::Kind::Network && !d.type.empty() ? d.type : d.name;
    }
    float bodyTop() const {
        return ui::titleHeight + ui::commandHeight;
    }
    double totalCpu() const {
        return std::isfinite(core->cpu) ? core->cpu : devices->cpu;
    }
    double cpuSpeed() const {
        return devices->frequencyGHz;
    }
    ~App() {
        closing = true;
        collector->requestStop();
        if (collectorJoin.joinable())
            collectorJoin.join();
        collector->join();
        removeTray();
        if (actionThread.joinable())
            actionThread.join();
        if (placementQueued && !placementWriter.flush())
            MessageBoxW(nullptr, L"The window position could not be saved. Other settings were not changed.",
                        L"Clean Task Manager", MB_OK | MB_ICONWARNING);
        for (auto& f : fonts)
            f.second->Release();
        if (editFont)
            DeleteObject(editFont);
        if (editBrush)
            DeleteObject(editBrush);
        if (appIcon)
            DestroyIcon(appIcon);
    }
    std::wstring rowKey(int index) const {
        if (index < 0)
            return L"";
        if (page == 0 && static_cast<size_t>(index) < processRows.size())
            return processRows[index].key;
        if (page != 1 && page != 7) {
            auto t = makeTable(page, *core, *devices);
            if (static_cast<size_t>(index) < t.rows.size())
                return t.rows[index].key;
        }
        return L"";
    }
    std::wstring selectionKey() const {
        return rowKey(selected);
    }
    void rebuildProcessRows() {
        auto groups = telemetry::groupProcesses(processes);
        auto compare = [&](const telemetry::ProcessGroup& a, const telemetry::ProcessGroup& b) {
            auto& x = a.summary;
            auto& y = b.summary;
            if (x.group != y.group)
                return x.group < y.group;
            if (sortColumn == 0)
                return sortAscending ? lower(x.name) < lower(y.name) : lower(x.name) > lower(y.name);
            if (sortColumn == 1)
                return sortAscending ? x.efficient < y.efficient : x.efficient > y.efficient;
            if (sortColumn >= 2) {
                double av = sortColumn == 2   ? x.cpu
                            : sortColumn == 3 ? x.memory
                            : sortColumn == 4 ? x.disk
                                              : x.network;
                double bv = sortColumn == 2   ? y.cpu
                            : sortColumn == 3 ? y.memory
                            : sortColumn == 4 ? y.disk
                                              : y.network;
                if (telemetry::metricLess(av, bv, sortAscending))
                    return true;
                if (telemetry::metricLess(bv, av, sortAscending))
                    return false;
            }
            return a.key < b.key;
        };
        std::stable_sort(groups.begin(), groups.end(), compare);
        processRows.clear();
        for (auto& group : groups) {
            bool matched = query.empty();
            for (auto& member : group.members)
                if (matches(member.name) || matches(std::to_wstring(member.pid)))
                    matched = true;
            if (!matched)
                continue;
            auto key = L"group:" + group.key;
            processRows.push_back({group.summary, group.members, key, key, 0});
            if (group.members.size() > 1 && (expanded.count(key) || !query.empty())) {
                for (auto& member : group.members) {
                    if (!query.empty() && !matches(member.name) && !matches(std::to_wstring(member.pid)))
                        continue;
                    auto identity = member.born ? L"pid:" + std::to_wstring(member.pid) + L":" +
                                                      std::to_wstring(member.born)
                                                : L"";
                    processRows.push_back({member, {member}, identity, key, 1});
                }
            }
        }
    }
    void restoreSelection(const std::wstring& keep) {
        selected = -1;
        if (keep.empty())
            return;
        if (page == 0) {
            for (size_t i = 0; i < processRows.size(); i++)
                if (processRows[i].key == keep) {
                    selected = static_cast<int>(i);
                    break;
                }
        } else if (page != 1 && page != 7) {
            auto table = makeTable(page, *core, *devices);
            for (size_t i = 0; i < table.rows.size(); i++)
                if (table.rows[i].key == keep) {
                    selected = static_cast<int>(i);
                    break;
                }
        }
    }
    void loadPreferences(settings::Values values) {
        expanded.clear();
        preferences = std::move(values);
        themeMode = settings::integer(preferences, "Appearance.Theme", 0, 0, 2);
        navExpanded = settings::integer(preferences, "Appearance.NavigationExpanded", 0, 0, 1) != 0;
        defaultTab = settings::integer(preferences, "Startup.DefaultTab", 0, -1, 7);
        runAdmin = settings::integer(preferences, "Startup.RunAsAdministrator", 1, 0, 1) != 0;
        updateInterval = settings::integer(preferences, "Monitoring.UpdateIntervalMs", 1000, 500, 1500);
        if (updateInterval != 500 && updateInterval != 1000 && updateInterval != 1500)
            updateInterval = 1000;
        closeToTray = settings::integer(preferences, "Window.CloseToTray", 0, 0, 1) != 0;
        ownedDebugger = settings::wide(settings::get(preferences, "Integration.OwnedDebugger"));
        page = defaultTab < 0 ? settings::integer(preferences, "Startup.LastTab", 0, 0, 7) : defaultTab;
        navFocus = page;
        lastContentPage = settings::integer(preferences, "Startup.LastTab", page == 7 ? 0 : page, 0, 6);
        cpuGraphMode = settings::integer(preferences, "Performance.CpuGraph", 0, 0, 1);
        deviceKey = settings::wide(settings::get(preferences, "Performance.SelectedDevice", "cpu"));
        savedMaximized = settings::integer(preferences, "Window.Maximized", 0, 0, 1) != 0;
        for (int i = 0; i < 3; i++)
            collapsed[i] =
                settings::integer(preferences, "Processes.Collapsed" + std::to_string(i), 0, 0, 1) != 0;
        for (int i = 0; i < 8; i++) {
            pageSort[i] =
                settings::integer(preferences, "Table" + std::to_string(i) + ".SortColumn", -1, -1, 9);
            pageAscending[i] =
                settings::integer(preferences, "Table" + std::to_string(i) + ".Ascending", 1, 0, 1) != 0;
        }
        sortColumn = pageSort[page];
        sortAscending = pageAscending[page];
        for (auto& [key, value] : preferences)
            if (key.rfind("ExpandedGroups.", 0) == 0)
                expanded.insert(settings::wide(value));
    }
    void capturePreferences() {
        if (!window)
            return;
        if (!IsIconic(window) && !IsZoomed(window))
            GetWindowRect(window, &normalRect);
        if (!IsIconic(window))
            savedMaximized = IsZoomed(window) != FALSE;
        else {
            WINDOWPLACEMENT placement{};
            placement.length = sizeof(placement);
            if (GetWindowPlacement(window, &placement))
                savedMaximized = (placement.flags & WPF_RESTORETOMAXIMIZED) != 0;
        }
        preferences["Appearance.Theme"] = std::to_string(themeMode);
        preferences["Appearance.NavigationExpanded"] = navExpanded ? "1" : "0";
        preferences["Startup.DefaultTab"] = std::to_string(defaultTab);
        preferences["Startup.LastTab"] = std::to_string(lastContentPage);
        preferences["Startup.RunAsAdministrator"] = runAdmin ? "1" : "0";
        preferences["Monitoring.UpdateIntervalMs"] = std::to_string(updateInterval);
        preferences["Window.CloseToTray"] = closeToTray ? "1" : "0";
        preferences.erase("Window.AutoSavePlacement");
        if (ownedDebugger.empty())
            preferences.erase("Integration.OwnedDebugger");
        else
            preferences["Integration.OwnedDebugger"] = settings::utf8(ownedDebugger);
        preferences["Performance.CpuGraph"] = std::to_string(cpuGraphMode);
        preferences["Performance.SelectedDevice"] = settings::utf8(deviceKey);
        // Geometry is captured independently for exit/handoff, never by a
        // general preferences save. Preserve old INI bounds as a fallback.
        for (int i = 0; i < 3; i++)
            preferences["Processes.Collapsed" + std::to_string(i)] = collapsed[i] ? "1" : "0";
        pageSort[page] = sortColumn;
        pageAscending[page] = sortAscending;
        for (int i = 0; i < 8; i++) {
            preferences["Table" + std::to_string(i) + ".SortColumn"] = std::to_string(pageSort[i]);
            preferences["Table" + std::to_string(i) + ".Ascending"] = pageAscending[i] ? "1" : "0";
        }
        for (auto i = preferences.begin(); i != preferences.end();)
            if (i->first.rfind("ExpandedGroups.", 0) == 0)
                i = preferences.erase(i);
            else
                ++i;
        int index = 0;
        for (auto& key : expanded)
            preferences["ExpandedGroups.Group" + std::to_string(index++)] = settings::utf8(key);
        preferencesDirty = preferences != persistedPreferences || replacementPending;
        saveStatus.clear();
    }
    settings::Placement currentPlacement() {
        if (!IsIconic(window) && !IsZoomed(window))
            GetWindowRect(window, &normalRect);
        if (!IsIconic(window))
            savedMaximized = IsZoomed(window) != FALSE;
        else {
            WINDOWPLACEMENT p{};
            p.length = sizeof(p);
            if (GetWindowPlacement(window, &p))
                savedMaximized = (p.flags & WPF_RESTORETOMAXIMIZED) != 0;
        }
        return {normalRect.left,
                normalRect.top,
                normalRect.right - normalRect.left,
                normalRect.bottom - normalRect.top,
                static_cast<int>(GetDpiForWindow(window)),
                savedMaximized};
    }
    void queuePlacementOnExit() {
        if (handoffExit || !placementBaseline)
            return;
        auto position = currentPlacement();
        if (position == *placementBaseline)
            return;
        placementWriter.queueCompact(
            settings::placementPath(),
            settings::placementValues(position,
                                      settings::get(persistedPreferences, "Configuration.Revision")));
        placementQueued = true;
    }
    bool persistPreferences() {
        capturePreferences();
        refreshReplacement();
        // Old releases stored placement here. Retain that original fallback
        // until an actual move creates the independent placement file.
        for (auto key : {"Window.Left", "Window.Top", "Window.Width", "Window.Height", "Window.Dpi",
                         "Window.Maximized"}) {
            auto legacy = persistedPreferences.find(key);
            if (legacy != persistedPreferences.end())
                preferences[key] = legacy->second;
            else
                preferences.erase(key);
        }
        if (!replacementPending && preferences == persistedPreferences)
            return true;
        if (replacementPending && (replacement.kind == integration::ReplacementKind::Unavailable ||
                                   replacement.kind == integration::ReplacementKind::Other)) {
            errorMessage(L"Windows could not apply the pending Task Manager replacement. Any other "
                         L"replacement was preserved.",
                         replacement.error ? replacement.error : ERROR_ALREADY_EXISTS);
            return false;
        }
        auto savedOwned = ownedDebugger;
        bool changing = replacementPending;
        // Preference saves do not alter the placement generation or baseline.
        if (changing) {
            // Persist ownership before changing Windows, preserving rollback.
            settingsWriter.queue(settings::filePath(), preferences);
            if (!settingsWriter.flush()) {
                errorMessage(L"The settings file could not be saved. Windows Task Manager was not changed.",
                             settingsWriter.error());
                return false;
            }
            persistedPreferences = preferences;
            DWORD error = taskManager.set(desiredReplacement, ownedDebugger);
            refreshReplacement();
            if (error) {
                errorMessage(L"The settings file was saved, but Windows could not apply the Task Manager "
                             L"replacement. Restart as administrator to retry.",
                             error);
                return false;
            }
            ownedDebugger = desiredReplacement ? taskManager.command() : L"";
            replacementPending = false;
            capturePreferences();
        } else if (replacement.kind == integration::ReplacementKind::Owned) {
            ownedDebugger = replacement.debugger;
            capturePreferences();
        }
        settingsWriter.queue(settings::filePath(), preferences);
        if (!settingsWriter.flush()) {
            DWORD error = settingsWriter.error();
            if (changing && desiredReplacement) {
                DWORD undo = taskManager.set(false, ownedDebugger);
                refreshReplacement();
                desiredReplacement = true;
                if (!undo) {
                    ownedDebugger = savedOwned;
                    replacementPending = true;
                }
            }
            errorMessage(L"The configuration file could not be saved completely. Check folder write "
                         L"permission and retry the setting change.",
                         error);
            return false;
        }
        persistedPreferences = preferences;
        preferencesDirty = false;
        saveStatus = L"Settings are saved automatically.";
        invalidate();
        return true;
    }
    static std::wstring quoteArgument(const std::wstring& s) {
        std::wstring out = L"\"";
        size_t slashes = 0;
        for (wchar_t c : s) {
            if (c == L'\\') {
                slashes++;
                continue;
            }
            if (c == L'"') {
                out.append(slashes * 2 + 1, L'\\');
                out += c;
                slashes = 0;
            } else {
                out.append(slashes, L'\\');
                slashes = 0;
                out += c;
            }
        }
        out.append(slashes * 2, L'\\');
        out += L'"';
        return out;
    }
    void restartAdministrator() {
        runAdmin = true;
        if (!persistPreferences())
            return;
        auto state = preferences;
        for (auto& [key, value] : settings::placementValues(currentPlacement(), ""))
            if (key != "Window.ConfigurationRevision" && key != "Window.Format")
                state[key] = value;
        state["Transient.ActivePage"] = std::to_string(page);
        state["Transient.DesiredReplacement"] = desiredReplacement ? "1" : "0";
        if (placementBaseline) {
            auto baseline = settings::placementValues(*placementBaseline, "");
            for (auto& [key, value] : baseline)
                state["Transient.Baseline" + key.substr(7)] = value;
        }
        auto transfer = integration::SettingsHandoff::create(settings::serialize(state));
        if (!transfer) {
            errorMessage(L"The in-memory settings could not be prepared for restart.",
                         ERROR_NOT_ENOUGH_MEMORY);
            return;
        }
        FILETIME born{}, exited{}, k{}, u{};
        GetProcessTimes(GetCurrentProcess(), &born, &exited, &k, &u);
        uint64_t birth = (uint64_t(born.dwHighDateTime) << 32) | born.dwLowDateTime;
        wchar_t path[32768];
        GetModuleFileNameW(nullptr, path, 32768);
        auto args = L"--elevated --handoff-pid=" + std::to_wstring(GetCurrentProcessId()) +
                    L" --handoff-born=" + std::to_wstring(birth) + L" --handoff-state=" + transfer->token();
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.lpVerb = L"runas";
        info.lpFile = path;
        info.lpParameters = args.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&info)) {
            if (!transfer->waitReady(5000)) {
                errorMessage(
                    L"The new session did not accept the in-memory settings. This window will stay open.",
                    ERROR_TIMEOUT);
                return;
            }
            transfer->commit();
            handoffExit = true;
            forceExit = true;
            PostMessageW(window, WM_CLOSE, 0, 0);
        } else if (GetLastError() != ERROR_CANCELLED)
            errorMessage(L"Windows could not restart the administrator session.", GetLastError());
    }
    void startCollectors() {
        if (collectorsStarted || collectorStopping || metricsPaused || closing || !IsWindowVisible(window) ||
            IsIconic(window))
            return;
        if (search && IsWindowVisible(search))
            RedrawWindow(search, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
        HRESULT presented = DwmFlush();
        record(SUCCEEDED(presented) ? "first_presentation_flush" : "presentation_flush_failed");
        collectorsStarted = true;
        notificationPending = std::make_shared<std::atomic<bool>>(false);
        auto pending = notificationPending;
        uint64_t generation = ++collectorGeneration;
        collector->setUpdateInterval(updateInterval);
        collector->start(
            [this, pending, generation] {
                if (!closing && collectorGeneration == generation && !pending->exchange(true))
                    if (!PostMessageW(window, WM_APP + 2, static_cast<WPARAM>(generation), 0))
                        *pending = false;
            },
            sensorDelay);
        record("collector_threads_scheduled");
    }
    void pauseMetrics() {
        if (metricsPaused)
            return;
        metricsPaused = true;
        collectorGeneration++;
        record("metrics_pause_requested");
        if (!collectorsStarted)
            return;
        collectorsStarted = false;
        collectorStopping = true;
        collector->requestStop();
        collectorJoin = std::thread([this] {
            collector->join();
            if (!closing)
                PostMessageW(window, WM_APP + 4, 0, 0);
        });
    }
    void finishCollectorStop() {
        if (collectorJoin.joinable())
            collectorJoin.join();
        collector = std::make_unique<telemetry::Collector>();
        collectorStopping = false;
        record("metrics_paused");
        if (!closing && !metricsPaused && IsWindowVisible(window) && !IsIconic(window)) {
            firstDraw = false;
            invalidate();
            present();
        }
    }
    void prepareResume() {
        if (!metricsPaused)
            return;
        metricsPaused = false;
        loggedCpu = false;
        loggedClock = false;
        auto emptyCore = std::make_shared<telemetry::CoreSnapshot>();
        emptyCore->info = core->info;
        core = emptyCore;
        auto emptyDevices = std::make_shared<telemetry::DeviceSnapshot>(*devices);
        emptyDevices->cpu = emptyDevices->frequencyGHz = emptyDevices->networkUsage =
            emptyDevices->diskUsage = telemetry::unknown;
        emptyDevices->cpuHistory.clear();
        for (auto& cpu : emptyDevices->logicalCpu) {
            cpu.usage = telemetry::unknown;
            cpu.history.clear();
            cpu.parkingKnown = cpu.parked = false;
        }
        for (auto& d : emptyDevices->devices) {
            d.usage = d.read = d.write = d.responseMs = d.temperature = d.dedicatedUsed = d.sharedUsed =
                telemetry::unknown;
            d.history.clear();
            d.dedicatedHistory.clear();
            d.sharedHistory.clear();
            for (auto& engine : d.gpuEngines) {
                engine.usage = telemetry::unknown;
                engine.history.clear();
            }
        }
        devices = emptyDevices;
        processes.clear();
        processRows.clear();
        selected = -1;
        firstDraw = false;
        invalidate();
        record("metrics_resume_requested");
    }
    bool addTray() {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window;
        data.uID = 1;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = WM_APP + 5;
        data.hIcon = appIcon ? appIcon : LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(data.szTip, L"Clean Task Manager");
        if (!Shell_NotifyIconW(NIM_ADD, &data))
            return false;
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
        trayPresent = true;
        return true;
    }
    void removeTray() {
        if (trayPresent) {
            NOTIFYICONDATAW data{};
            data.cbSize = sizeof(data);
            data.hWnd = window;
            data.uID = 1;
            Shell_NotifyIconW(NIM_DELETE, &data);
            trayPresent = false;
        }
    }
    void hideToTray() {
        if (!trayPresent && !addTray()) {
            errorMessage(L"Windows could not create the tray icon. The window will stay open.",
                         ERROR_GEN_FAILURE);
            return;
        }
        capturePreferences();
        trayHidden = true;
        pauseMetrics();
        ShowWindow(window, SW_HIDE);
    }
    void restoreWindow() {
        prepareResume();
        trayHidden = false;
        removeTray();
        ShowWindow(window, IsIconic(window) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(window);
        invalidate();
        present();
    }
    void trayMenu() {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 980, L"Open Clean Task Manager");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 981, L"Exit");
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(window);
        int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, window, nullptr);
        DestroyMenu(menu);
        PostMessageW(window, WM_NULL, 0, 0);
        if (command == 980)
            restoreWindow();
        if (command == 981) {
            forceExit = true;
            PostMessageW(window, WM_CLOSE, 0, 0);
        }
    }
    void refreshReplacement(bool syncDesired = false) {
        replacement = taskManager.state(ownedDebugger);
        if (syncDesired || !replacementPending)
            desiredReplacement = replacement.kind == integration::ReplacementKind::Owned;
    }
    bool changeReplacement(bool enabled) {
        refreshReplacement();
        if (replacement.kind == integration::ReplacementKind::Other ||
            replacement.kind == integration::ReplacementKind::Unavailable) {
            errorMessage(
                L"The existing Task Manager replacement could not be changed. Its setting was preserved.",
                replacement.error ? replacement.error : ERROR_ALREADY_EXISTS);
            return false;
        }
        desiredReplacement = enabled;
        replacementPending = enabled != (replacement.kind == integration::ReplacementKind::Owned);
        bool ok = persistPreferences();
        if (!ok) {
            refreshReplacement(true);
            replacementPending = false;
        }
        invalidate();
        return ok;
    }
    void resetSettings() {
        if (MessageBoxW(window,
                        L"Reset settings to their defaults?\n\nThis saves the default settings and restores "
                        L"Windows Task Manager if this copy replaced it.",
                        L"Reset settings", MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES)
            return;
        applyDefaultSettings();
    }
    void applyDefaultSettings() {
        refreshReplacement();
        if (replacement.kind == integration::ReplacementKind::Unavailable) {
            errorMessage(L"The Task Manager replacement state could not be checked. Settings were preserved.",
                         replacement.error);
            return;
        }
        auto priorOwned = ownedDebugger;
        auto revision = settings::get(preferences, "Configuration.Revision");
        expanded.clear();
        selectionKeys.fill(L"");
        loadPreferences({});
        ownedDebugger = priorOwned;
        desiredReplacement = false;
        replacementPending = replacement.kind == integration::ReplacementKind::Owned;
        if (!revision.empty())
            preferences["Configuration.Revision"] = revision;
        selected = -1;
        query.clear();
        SetWindowTextW(search, L"");
        scroll = hscroll = deviceScroll = 0;
        device = 0;
        collector->setUpdateInterval(updateInterval);
        updateTheme();
        rebuildProcessRows();
        if (IsZoomed(window))
            ShowWindow(window, SW_RESTORE);
        MONITORINFO mi{sizeof(mi), {}, {}, 0};
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &mi);
        int w = std::min<int>(static_cast<int>(1180 * scale), mi.rcWork.right - mi.rcWork.left),
            h = std::min<int>(static_cast<int>(780 * scale), mi.rcWork.bottom - mi.rcWork.top);
        SetWindowPos(window, nullptr, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2,
                     mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2, w, h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        persistPreferences();
        layout();
    }
    std::wstring inputKey(int id) const {
        if (id >= 10000 && id < 20000)
            return rowKey(id - 10000);
        if (id >= 20000 && id < 30000 && static_cast<size_t>(id - 20000) < processRows.size())
            return processRows[id - 20000].groupKey;
        if (id >= 200 && id < 300) {
            auto list = performanceDevices();
            if (static_cast<size_t>(id - 200) < list.size())
                return list[id - 200].key;
        }
        return L"";
    }
    void record(const char* event) {
        if (diagnosticsFile.empty())
            return;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        HANDLE f = CreateFileW(diagnosticsFile.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            char b[512];
            int len = snprintf(b, sizeof(b),
                               "{\"event\":\"%s\",\"entry_ms\":%.3f,\"core_ready\":%s,\"devices_ready\":%s,"
                               "\"processes\":%zu,\"devices\":%zu,\"page\":%d,\"draws\":%llu}\n",
                               event, (now.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart,
                               core->ready ? "true" : "false", devices->ready ? "true" : "false",
                               processes.size(), devices->devices.size(), page, drawCount);
            DWORD wrote;
            WriteFile(f, b, static_cast<DWORD>(len), &wrote, nullptr);
            CloseHandle(f);
        }
    }
    void acceptSnapshots() {
        *notificationPending = false;
        std::wstring keep = selectionKey();
        auto c = collector->core();
        auto d = collector->devices();
        bool firstCore = !core->ready && c->ready, firstDevices = !devices->ready && d->ready;
        core = std::move(c);
        devices = std::move(d);
        processes = core->processes;
        rebuildProcessRows();
        restoreSelection(keep);
        auto list = performanceDevices();
        device = 0;
        for (size_t i = 0; i < list.size(); i++)
            if (list[i].key == deviceKey) {
                device = static_cast<int>(i);
                break;
            }
        tooltip.clear();
        invalidate();
        // Publish the new hit rectangles before processing queued input.
        // A click in progress is checked by identity on mouse-up, not discarded.
        present();
        if (firstCore)
            record("first_core_snapshot");
        if (firstDevices)
            record("first_device_snapshot");
        if (!loggedCpu && std::isfinite(totalCpu())) {
            loggedCpu = true;
            record("first_cpu_reading");
        }
        if (!loggedClock && std::isfinite(cpuSpeed())) {
            loggedClock = true;
            record("first_cpu_speed");
        }
    }
    HWND drawingWindow() const {
        return canvas ? canvas : window;
    }
    void invalidate() {
        if (auto surface = drawingWindow())
            InvalidateRect(surface, nullptr, FALSE);
    }
    void present() {
        if (auto surface = drawingWindow())
            UpdateWindow(surface);
    }
    void synchronizeHover() {
        if (!pointerKnown)
            return;
        int id = hitAt(pointerX, pointerY);
        if (id != hover) {
            hover = id;
            tooltip.clear();
            invalidate();
        }
    }
    static COLORREF gdiColor(unsigned c) {
        return RGB((c >> 16) & 255, (c >> 8) & 255, c & 255);
    }
    void eraseOpaque(HDC dc) {
        RECT r;
        GetClientRect(window, &r);
        SetDCBrushColor(dc, gdiColor(palette.window));
        FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
    IDWriteTextFormat* font(float size, int weight = 400, bool icons = false) {
        int key = static_cast<int>(size * 10) + weight * 1000 + (icons ? 10000000 : 0);
        auto it = fonts.find(key);
        if (it != fonts.end())
            return it->second;
        IDWriteTextFormat* f = nullptr;
        if (FAILED(writer->CreateTextFormat(icons ? ui::iconFont : ui::font, nullptr,
                                            static_cast<DWRITE_FONT_WEIGHT>(weight), DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, size, L"en-US", &f)))
            return nullptr;
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        Com<IDWriteInlineObject> ellipsis;
        writer->CreateEllipsisTrimmingSign(f, ellipsis.put());
        f->SetTrimming(&trim, ellipsis.p);
        fonts[key] = f;
        return f;
    }
    void fill(Box b, unsigned c, float alpha = 1, float radius = 0) {
        brush->SetColor(color(c, alpha));
        if (radius > 0)
            target->FillRoundedRectangle(D2D1::RoundedRect(b.rect(), radius, radius), brush.p);
        else
            target->FillRectangle(b.rect(), brush.p);
    }
    void stroke(Box b, unsigned c, float thickness = 1, float radius = 0) {
        brush->SetColor(color(c));
        if (radius > 0)
            target->DrawRoundedRectangle(D2D1::RoundedRect(b.rect(), radius, radius), brush.p, thickness);
        else
            target->DrawRectangle(b.rect(), brush.p, thickness);
    }
    void line(float x1, float y1, float x2, float y2, unsigned c, float thickness = 1) {
        brush->SetColor(color(c));
        target->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush.p, thickness);
    }
    void text(const std::wstring& s, Box b, float size = ui::bodyFont, unsigned c = 0, int weight = 400,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, bool icons = false) {
        auto* f = font(size, weight, icons);
        if (!f || b.w <= 0 || b.h <= 0)
            return;
        f->SetTextAlignment(align);
        brush->SetColor(color(c ? c : palette.text));
        target->DrawText(s.c_str(), static_cast<UINT32>(s.size()), f, b.rect(), brush.p,
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    void glyph(const wchar_t* s, Box b, float size = 16, unsigned c = 0) {
        text(s, b, size, c, 400, DWRITE_TEXT_ALIGNMENT_CENTER, true);
    }
    void hit(int id, Box b, const std::wstring& tip = L"") {
        b.y += hitOffsetY;
        if (id >= 400) {
            float right = std::min(b.x + b.w, width), bottom = std::min(b.y + b.h, height);
            b.x = std::max(b.x, navWidth());
            b.y = std::max(b.y, bodyTop());
            b.w = right - b.x;
            b.h = bottom - b.y;
        }
        if (b.w > 0 && b.h > 0)
            hits.push_back({id, b, tip});
    }
    int hitAt(float x, float y) const {
        for (auto i = hits.rbegin(); i != hits.rend(); ++i)
            if (i->box.has(x, y))
                return i->id;
        return -1;
    }
    void focusBox(Box b) {
        if (keyboard)
            stroke({b.x + 2, b.y + 2, b.w - 4, b.h - 4}, palette.text, 1, 3);
    }
    void button(int id, Box b, const wchar_t* icon, const std::wstring& label, bool enabled = true) {
        if (hover == id && enabled)
            fill({b.x, b.y + 7, b.w, b.h - 14}, palette.hover, 1, 4);
        unsigned ink = enabled ? palette.text : palette.disabled;
        if (icon && *icon)
            glyph(icon, {b.x + 8, b.y, 20, b.h}, 16, ink);
        text(label, {b.x + (icon && *icon ? 34 : 10), b.y, b.w - (icon && *icon ? 40 : 20), b.h},
             ui::commandFont, ink);
        hit(id, b, enabled ? label : L"Select a process");
    }
    void icon(int kind, Box b) {
        if (kind == 2) {
            auto mid = D2D1::Point2F(b.x + b.w / 2, b.y + b.h / 2);
            brush->SetColor(color(0xE8B343));
            target->FillEllipse(D2D1::Ellipse(mid, b.w / 2, b.h / 2), brush.p);
            fill({b.x, b.y, b.w, b.h / 3}, 0xE4574E);
            Com<ID2D1PathGeometry> shape;
            Com<ID2D1GeometrySink> sink;
            if (SUCCEEDED(factory->CreatePathGeometry(shape.put())) && SUCCEEDED(shape->Open(sink.put()))) {
                sink->BeginFigure(D2D1::Point2F(b.x, b.y + b.h / 3), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(b.x + b.w / 2, b.y + b.h / 2));
                sink->AddLine(D2D1::Point2F(b.x + b.w * 0.26f, b.y + b.h));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                brush->SetColor(color(0x55A468));
                target->FillGeometry(shape.p, brush.p);
            }
            brush->SetColor(color(0x65B9F0));
            target->FillEllipse(D2D1::Ellipse(mid, b.w * .24f, b.h * .24f), brush.p);
            return;
        }
        if (kind == 1) {
            fill({b.x, b.y + 2, b.w * .45f, 4}, 0xEFBB48, 1, 1);
            fill({b.x, b.y + 5, b.w, b.h - 5}, 0xF6C64C, 1, 1);
            fill({b.x, b.y + 7, b.w, b.h - 7}, 0xFAD66F, 1, 1);
            return;
        }
        if (kind == 3) {
            brush->SetColor(color(0x21A6C7));
            target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(b.x + b.w / 2, b.y + b.h / 2), b.w / 2, b.h / 2),
                                brush.p);
            brush->SetColor(color(0x297CD3));
            target->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(b.x + b.w * .52f, b.y + b.h * .68f), b.w * .45f, b.h * .3f),
                brush.p);
            fill({b.x + b.w * .4f, b.y + b.h * .4f, b.w * .6f, b.h * .2f}, 0x47D0C1, 1, 2);
            return;
        }
        if (kind == 9) {
            fill(b, 0x76B900, 1, 2);
            glyph(L"\xE7B3", b, b.w * .75f, 0xFFFFFF);
            return;
        }
        if (kind == 8) {
            glyph(L"\xE753", b, b.w, 0x59A5EE);
            return;
        }
        if (kind == 6) {
            glyph(L"\xE713", b, b.w, palette.muted);
            return;
        }
        if (kind == 7) {
            fill(b, isDark ? 0x333333 : 0x262626, 1, 2);
            text(L">_", b, 10, 0xFFFFFF, 600, DWRITE_TEXT_ALIGNMENT_CENTER);
            return;
        }
        if (kind == 5) {
            fill({b.x + 2, b.y, b.w - 4, b.h}, 0xC2DEF2, 1, 1);
            for (int i = 0; i < 3; i++)
                line(b.x + 5, b.y + 5 + i * 3, b.x + b.w - 4, b.y + 5 + i * 3, 0x568CB5);
            return;
        }
        if (kind == 4) {
            fill({b.x + 3, b.y, b.w - 6, b.h}, 0xA7BAC8, 1, 2);
            fill({b.x + 5, b.y + 3, b.w - 10, 4}, 0x263C4A);
            for (int i = 0; i < 2; i++)
                for (int j = 0; j < 2; j++)
                    fill({b.x + 5 + j * 4, b.y + 10 + i * 4, 2, 2}, 0x345F7B);
            return;
        }
        fill({b.x, b.y + 2, b.w, b.h - 4}, 0x3288C0, 1, 1);
        line(b.x + 2, b.y + b.h * .65f, b.x + 5, b.y + b.h * .65f, 0xA5EBFF);
        line(b.x + 5, b.y + b.h * .65f, b.x + 7, b.y + b.h * .32f, 0xA5EBFF);
        line(b.x + 7, b.y + b.h * .32f, b.x + 10, b.y + b.h * .75f, 0xA5EBFF);
        line(b.x + 10, b.y + b.h * .75f, b.x + b.w - 2, b.y + b.h * .45f, 0xA5EBFF);
    }
    void updateTheme() {
        DWORD value = 0, size = sizeof(value);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
        isDark = themeMode == 1 || (themeMode == 0 && value == 0);
        palette = isDark ? ui::dark : ui::light;
        BOOL dark = isDark;
        DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
        COLORREF caption = isDark ? RGB(32, 32, 32) : RGB(243, 243, 243);
        DwmSetWindowAttribute(window, 35, &caption, sizeof(caption));
        if (editBrush)
            DeleteObject(editBrush);
        unsigned c = palette.search;
        editBrush = CreateSolidBrush(RGB((c >> 16) & 255, (c >> 8) & 255, c & 255));
        if (search)
            InvalidateRect(search, nullptr, TRUE);
        invalidate();
    }
    bool createGraphics() {
        if (!factory && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.put())))
            return false;
        if (!writer && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                  reinterpret_cast<IUnknown**>(writer.put()))))
            return false;
        if (!target) {
            RECT r;
            GetClientRect(drawingWindow(), &r);
            auto properties = D2D1::RenderTargetProperties();
            properties.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
            HRESULT hr = factory->CreateHwndRenderTarget(
                properties, D2D1::HwndRenderTargetProperties(drawingWindow(), D2D1::SizeU(r.right, r.bottom)),
                target.put());
            if (FAILED(hr))
                return false;
            target->SetDpi(96 * scale, 96 * scale);
            target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
            if (FAILED(target->CreateSolidColorBrush(color(palette.text), brush.put()))) {
                target.reset();
                return false;
            }
        }
        return true;
    }
    void layout() {
        RECT r;
        GetClientRect(window, &r);
        scale = GetDpiForWindow(window) / 96.0f;
        width = r.right / scale;
        height = r.bottom / scale;
        if (canvas)
            SetWindowPos(canvas, HWND_BOTTOM, 0, 0, r.right, r.bottom, SWP_NOACTIVATE);
        RECT surfaceRect = r;
        if (canvas)
            GetClientRect(canvas, &surfaceRect);
        if (target) {
            auto old = target->GetPixelSize();
            if (old.width != static_cast<UINT>(surfaceRect.right) ||
                old.height != static_cast<UINT>(surfaceRect.bottom)) {
                if (FAILED(target->Resize(D2D1::SizeU(surfaceRect.right, surfaceRect.bottom)))) {
                    brush.reset();
                    target.reset();
                }
            }
            if (target)
                target->SetDpi(96 * scale, 96 * scale);
        }
        float searchW = std::clamp(width - 600.0f, 230.0f, 420.0f);
        float searchX = std::max(242.0f, (width - searchW) / 2);
        searchX = std::min(searchX, width - 145 - searchW);
        SetWindowPos(search, nullptr, static_cast<int>((searchX + 40) * scale), static_cast<int>(14 * scale),
                     static_cast<int>((searchW - 50) * scale), static_cast<int>(22 * scale),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        int pixels = static_cast<int>(13 * scale);
        if (!editFont || editFontPixels != pixels) {
            HFONT next = CreateFontW(-pixels, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH, ui::font);
            if (next) {
                SendMessageW(search, WM_SETFONT, reinterpret_cast<WPARAM>(next), FALSE);
                if (editFont)
                    DeleteObject(editFont);
                editFont = next;
                editFontPixels = pixels;
            }
        }
        ShowWindow(search, page == 7 ? SW_HIDE : SW_SHOWNA);
        invalidate();
        if (IsWindowVisible(window) && !IsIconic(window)) {
            present();
            if (page != 7)
                RedrawWindow(search, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
        }
    }
    void drawTitle() {
        fill({0, 0, width, height}, palette.window);
        icon(0, {15, 15, 16, 16});
        text(L"Clean Task Manager", {55, 0, 174, 48}, 12);
        if (page != 7) {
            float sw = std::clamp(width - 600.0f, 230.0f, 420.0f);
            float sx = std::min(std::max(242.0f, (width - sw) / 2), width - 145 - sw);
            fill({sx, 9, sw, 32}, palette.search, 1, 4);
            line(sx + 3, 40, sx + sw - 3, 40, palette.searchLine);
            glyph(L"\xE721", {sx + 9, 9, 22, 32}, 12);
            hit(14, {sx, 9, sw, 32}, L"Search (Ctrl+F)");
        }
        for (int i = 0; i < 3; i++) {
            Box b{width - 138 + i * 46.0f, 0, 46, 32};
            int id = 10 + i;
            if (hover == id)
                fill(b, id == 12 ? 0xC42B1C : palette.hover);
            unsigned ink = hover == 12 ? 0xFFFFFF : palette.text;
            if (i == 0)
                line(b.x + 18, 16, b.x + 28, 16, ink);
            if (i == 1) {
                if (IsZoomed(window)) {
                    stroke({b.x + 18, 10, 8, 8}, ink);
                    line(b.x + 21, 8, b.x + 29, 8, ink);
                    line(b.x + 29, 8, b.x + 29, 16, ink);
                } else
                    stroke({b.x + 18, 10, 9, 9}, ink);
            }
            if (i == 2) {
                line(b.x + 18, 10, b.x + 27, 19, ink);
                line(b.x + 27, 10, b.x + 18, 19, ink);
            }
            hit(id, b, i == 0 ? L"Minimize" : i == 1 ? L"Maximize / Restore" : L"Close");
        }
    }
    void drawNav() {
        const float nw = navWidth();
        Box hamburger{4, 52, 40, 36};
        if (hover == 90)
            fill(hamburger, palette.hover, 1, 4);
        glyph(L"\xE700", hamburger, 16);
        hit(90, hamburger, L"Open navigation");
        for (int i = 0; i < 8; i++) {
            float y = i == 7 ? height - 48 : 96 + i * 40.0f;
            Box b{4, y, nw - 8, 36};
            int id = 100 + i;
            if (page == i)
                fill(b, palette.selected, 1, 4);
            else if (hover == id)
                fill(b, palette.hover, 1, 4);
            if (page == i)
                fill({4, y + 10, 3, 16}, palette.accent, 1, 1.5f);
            glyph(navigation::navGlyphs[i], {7, y, 32, 36}, 16);
            if (navExpanded)
                text(navigation::pages[i], {50, y, nw - 58, 36}, 13);
            if (focusArea == 1 && navFocus == i)
                focusBox(b);
            hit(id, b, navigation::pages[i]);
        }
    }
    void drawCommands() {
        float nw = navWidth();
        fill({nw, 48, width - nw, height - 48}, palette.surface, 1, 7);
        fill({nw, 65, width - nw, height - 65}, palette.surface);
        text(navigation::pages[page], {nw + 16, 48, 190, 52}, ui::pageFont, 0, 600);
        if (page != 7) {
            button(300, {width - 392, 48, 124, 52}, L"\xE8A7", L"Run new task");
            if (page == 0) {
                line(width - 267, 66, width - 267, 82, palette.border);
                button(301, {width - 259, 48, 95, 52}, L"\xE711", L"End task", selected >= 0);
                button(302, {width - 159, 48, 119, 52}, L"\xE945", L"Efficiency mode", selected >= 0);
            } else if (page == 3)
                button(301, {width - 155, 48, 110, 52}, L"\xE8D8", L"Disable", selected >= 0);
            else if (page == 6)
                button(301, {width - 155, 48, 110, 52}, L"\xE768", L"Start", selected >= 0);
            else if (page == 5)
                button(301, {width - 155, 48, 110, 52}, L"\xE711", L"End task", selected >= 0);
            button(303, {width - 40, 48, 36, 52}, L"", L"\x2026");
        }
        line(nw, 99.5f, width, 99.5f, palette.border);
    }
    std::vector<float> processEdges() const {
        float left = navWidth(), available = width - left - 12;
        float name = std::max(240.0f, available - 458);
        return {left,
                left + name,
                left + name + 110,
                left + name + 188,
                left + name + 288,
                left + name + 373,
                left + name + 458};
    }
    void drawProcessHeader(const std::vector<float>& e) {
        float y = bodyTop();
        text(L"Name", {e[0] + 16, y + 24, e[1] - e[0] - 30, 34});
        text(L"Status", {e[1] + 12, y + 24, e[2] - e[1] - 18, 34});
        const wchar_t* labels[] = {L"CPU", L"Memory", L"Disk", L"Network"};
        std::wstring vals[] = {
            number(totalCpu(), 0, L"%"),
            number(core->memory.valid
                       ? 100.0 * (core->memory.total - core->memory.available) / core->memory.total
                       : telemetry::unknown,
                   0, L"%"),
            number(devices->diskUsage, 0, L"%"), number(devices->networkUsage, 0, L"%")};
        for (int i = 0; i < 4; i++) {
            float l = e[i + 2], w = e[i + 3] - l;
            if (sortColumn == i + 2)
                fill({l, y, w, 60}, palette.hover);
            text(vals[i], {l + 8, y + 3, w - 18, 28}, 20, 0, 400, DWRITE_TEXT_ALIGNMENT_TRAILING);
            text(labels[i], {l + 7, y + 29, w - 17, 27}, 12, 0, 400, DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        for (int i = 0; i < 6; i++) {
            if (i > 0)
                line(e[i], y, e[i], y + 60, palette.border);
            hit(500 + i, {e[i], y, e[i + 1] - e[i], 60}, L"Sort by column");
            if (sortColumn == i)
                glyph(sortAscending ? L"\xE70E" : L"\xE70D", {e[i + 1] - 20, y + 1, 12, 12}, 9,
                      palette.muted);
        }
        line(e[0], y + 59.5f, width, y + 59.5f, palette.border);
    }
    bool matches(const std::wstring& value) const {
        return query.empty() || lower(value).find(lower(query)) != std::wstring::npos;
    }
    std::vector<int> processOrder() const {
        std::vector<int> out;
        for (size_t i = 0; i < processRows.size(); i++)
            out.push_back(static_cast<int>(i));
        return out;
    }
    void drawScrollbar(float top, float viewport, float total) {
        viewHeight = viewport;
        contentHeight = total;
        scroll = std::clamp(scroll, 0.0f, std::max(0.0f, total - viewport));
        if (total <= viewport)
            return;
        float thumb = std::max(28.0f, viewport * viewport / total);
        float travel = viewport - thumb;
        float y = top + (total > viewport ? scroll / (total - viewport) * travel : 0);
        fill({width - 8, top, 7, viewport}, palette.surface);
        fill({width - 6, y, hover == 800 ? 4.0f : 3.0f, thumb}, isDark ? 0x999999 : 0x858585, 1, 2);
        hit(800, {width - 12, y, 12, thumb}, L"Scroll");
        hit(801, {width - 12, top, 12, std::max(0.0f, y - top)});
        hit(802, {width - 12, y + thumb, 12, std::max(0.0f, top + viewport - y - thumb)});
    }
    void drawProcesses() {
        auto edges = processEdges();
        float offset = hscroll;
        for (auto& x : edges)
            x -= offset;
        target->PushAxisAlignedClip(D2D1::RectF(navWidth(), 100, width, height), D2D1_ANTIALIAS_MODE_ALIASED);
        drawProcessHeader(edges);
        std::array<int, 3> counts{};
        for (auto& row : processRows)
            if (row.depth == 0)
                counts[row.value.group]++;
        float top = 160, viewport = height - top - 1, total = 0;
        for (int g = 0; g < 3; g++) {
            if (counts[g])
                total += 36;
            if (!collapsed[g])
                for (auto& row : processRows)
                    if (row.value.group == g)
                        total += 32;
        }
        scroll = std::clamp(scroll, 0.0f, std::max(0.0f, total - viewport));
        float y = top - scroll;
        target->PushAxisAlignedClip(D2D1::RectF(navWidth(), top, width, height), D2D1_ANTIALIAS_MODE_ALIASED);
        visibleRows.clear();
        const wchar_t* categories[] = {L"Apps", L"Background processes", L"Windows processes"};
        for (int g = 0; g < 3; g++) {
            if (!counts[g])
                continue;
            if (y + 36 > top && y < height) {
                Box b{edges[0] + 6, y, width - edges[0] - 18, 36};
                if (hover == 400 + g)
                    fill(b, palette.hover);
                glyph(collapsed[g] ? L"\xE76C" : L"\xE70D", {edges[0] + 10, y, 16, 36}, 10, palette.muted);
                text(std::wstring(categories[g]) + L" (" + std::to_wstring(counts[g]) + L")",
                     {edges[0] + 32, y, width - edges[0] - 44, 36}, 14, palette.accent);
                hit(400 + g, b, L"Expand or collapse category");
            }
            y += 36;
            if (collapsed[g])
                continue;
            for (size_t i = 0; i < processRows.size(); i++) {
                const auto& row = processRows[i];
                const auto& p = row.value;
                if (p.group != g)
                    continue;
                int index = static_cast<int>(i);
                visibleRows.push_back(index);
                if (y + 32 > top && y < height) {
                    Box b{edges[0], y, edges.back() - edges[0], 32};
                    if (selected == index)
                        fill(b, palette.selected);
                    else if (hover == 10000 + index || hover == 20000 + index)
                        fill(b, palette.hover);
                    for (int c = 2; c < 6; c++) {
                        fill({edges[c] + 1, y, edges[c + 1] - edges[c] - 1, 32}, palette.heat,
                             selected == index ? .6f : .42f);
                        line(edges[c], y, edges[c], y + 32, palette.border);
                    }
                    hit(10000 + index,
                        {b.x, std::max(y, top), b.w, std::min(height, y + 32) - std::max(y, top)}, p.name);
                    if (row.depth == 0 && row.members.size() > 1) {
                        glyph(expanded.count(row.groupKey) || !query.empty() ? L"\xE70D" : L"\xE76C",
                              {edges[0] + 10, y, 16, 32}, 10, palette.muted);
                        hit(20000 + index,
                            {edges[0] + 6, std::max(y, top), 24, std::min(height, y + 32) - std::max(y, top)},
                            L"Expand or collapse instances");
                    }
                    float indent = row.depth * 26.0f;
                    icon(p.icon, {edges[0] + 34 + indent, y + 8, 16, 16});
                    std::wstring name = p.name;
                    if (row.depth)
                        name += L" (PID " + std::to_wstring(p.pid) + L")";
                    else if (row.members.size() > 1)
                        name += L" (" + std::to_wstring(row.members.size()) + L")";
                    text(name, {edges[0] + 58 + indent, y, edges[1] - edges[0] - 70 - indent, 32});
                    if (p.efficient)
                        glyph(L"\xE8BE", {edges[1] + 12, y, 20, 32}, 16, isDark ? 0x6CCB5F : 0x0F7B0F);
                    std::wstring values[] = {number(p.cpu, 1, L"%"), number(p.memory, 1, L" MB"),
                                             number(p.disk, 1, L" MB/s"), number(p.network, 1, L" Mbps")};
                    for (int c = 0; c < 4; c++)
                        text(values[c], {edges[c + 2] + 6, y, edges[c + 3] - edges[c + 2] - 16, 32}, 12,
                             c == 0 && p.cpu == 0 ? palette.muted : palette.text, 400,
                             DWRITE_TEXT_ALIGNMENT_TRAILING);
                    if (focusArea == 2 && selected == index)
                        focusBox(b);
                }
                y += 32;
            }
        }
        if (processRows.empty() && !query.empty() && core->ready)
            text(L"No results found", {navWidth() + 24, 196, width - navWidth() - 48, 40}, 16, palette.muted);
        target->PopAxisAlignedClip();
        drawScrollbar(top, viewport, total);
        target->PopAxisAlignedClip();
        contentWidth = std::max(width - navWidth(), edges.back() + offset - navWidth());
    }
    void graph(Box b, unsigned ink, const std::vector<telemetry::Point>& history, bool mini = false,
               double maximum = 1.0, float gridSpacing = 0) {
        if (b.w <= 0 || b.h <= 0)
            return;
        fill(b, ink, isDark ? .035f : .025f);
        if (!mini) {
            int columns =
                gridSpacing > 0 ? std::clamp(static_cast<int>(std::ceil(b.w / gridSpacing)), 2, 120) : 10;
            int rows = gridSpacing > 0 ? std::clamp(static_cast<int>(std::ceil(b.h / 38)), 2, 60) : 6;
            for (int i = 1; i < columns; i++)
                line(b.x + b.w * i / columns, b.y, b.x + b.w * i / columns, b.y + b.h, palette.grid);
            for (int i = 1; i < rows; i++)
                line(b.x, b.y + b.h * i / rows, b.x + b.w, b.y + b.h * i / rows, palette.grid);
        }
        if (!history.empty() && maximum > 0) {
            const double end = history.back().time;
            std::vector<D2D1_POINT_2F> points;
            auto segment = [&]() {
                if (points.empty())
                    return;
                if (points.size() > 1) {
                    Com<ID2D1PathGeometry> area;
                    Com<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(factory->CreatePathGeometry(area.put())) &&
                        SUCCEEDED(area->Open(sink.put()))) {
                        sink->BeginFigure(D2D1::Point2F(points.front().x, b.y + b.h),
                                          D2D1_FIGURE_BEGIN_FILLED);
                        sink->AddLines(points.data(), static_cast<UINT32>(points.size()));
                        sink->AddLine(D2D1::Point2F(points.back().x, b.y + b.h));
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                        sink->Close();
                        brush->SetColor(color(ink, isDark ? .14f : .1f));
                        target->FillGeometry(area.p, brush.p);
                    }
                    for (size_t i = 1; i < points.size(); i++)
                        line(points[i - 1].x, points[i - 1].y, points[i].x, points[i].y, ink,
                             mini ? 1 : 1.2f);
                } else {
                    brush->SetColor(color(ink));
                    target->FillEllipse(D2D1::Ellipse(points[0], 1, 1), brush.p);
                }
                points.clear();
            };
            for (auto& p : history) {
                if (!std::isfinite(p.value)) {
                    segment();
                    continue;
                }
                points.push_back(D2D1::Point2F(
                    b.x + b.w * static_cast<float>(std::clamp((p.time - end + 60) / 60, 0.0, 1.0)),
                    b.y + b.h - b.h * static_cast<float>(std::clamp(p.value / maximum, 0.0, 1.0))));
            }
            segment();
        }
        stroke(b, ink, 1);
    }
    void stat(float x, float y, float w, const wchar_t* label, const std::wstring& value, float size = 26) {
        text(label, {x, y, w, 18}, 12, palette.muted);
        text(value, {x, y + 17, w, 32}, size - 1, 0, 400);
    }
    std::map<std::pair<std::wstring, float>, float> detailWidths;
    float detailTextWidth(const std::wstring& value, float size) {
        if (value.empty())
            return 0;
        auto key = std::make_pair(value, size);
        auto found = detailWidths.find(key);
        if (found == detailWidths.end()) {
            Com<IDWriteTextLayout> layout;
            DWRITE_TEXT_METRICS metrics{};
            if (FAILED(writer->CreateTextLayout(value.c_str(), static_cast<UINT32>(value.size()), font(size),
                                                16384, 64, layout.put())) ||
                FAILED(layout->GetMetrics(&metrics)))
                return 0;
            if (detailWidths.size() >= 256)
                detailWidths.clear();
            found =
                detailWidths.emplace(std::move(key), std::ceil(metrics.widthIncludingTrailingWhitespace) + 2)
                    .first;
        }
        return found->second;
    }
    struct ReadoutLayout {
        std::array<Box, 4> fields;
        float detailsX = 0, detailsY = 0, labelWidth = 0, valueWidth = 0, bottom = 0;
        bool alongside = false;
    };
    ReadoutLayout compactReadouts(float available, const std::array<float, 4>& fields, float labelWidth,
                                  float valueWidth, unsigned detailRows, bool gpu) {
        constexpr float fieldGap = 12, panelGap = 14, rowStep = 50;
        ReadoutLayout out;
        out.labelWidth = labelWidth;
        out.valueWidth = valueWidth;
        float left = gpu ? std::max({fields[1], fields[2], fields[0] + fieldGap + fields[3]})
                         : std::max(fields[0], fields[2]) + fieldGap + std::max(fields[1], fields[3]);
        float metadata = labelWidth + 8 + valueWidth, rows = 2;
        if (gpu) {
            // This reading order is invariant across every resize. Only the
            // adjacent metadata column may move when space is insufficient.
            rows = 3;
            out.fields = {Box{0, 0, fields[0], 49}, Box{0, rowStep, fields[1], 49},
                          Box{0, 2 * rowStep, fields[2], 49}, Box{fields[0] + fieldGap, 0, fields[3], 49}};
        } else {
            float second = std::max(fields[0], fields[2]) + fieldGap;
            out.fields = {Box{0, 0, fields[0], 49}, Box{second, 0, fields[1], 49},
                          Box{0, rowStep, fields[2], 49}, Box{second, rowStep, fields[3], 49}};
        }
        out.alongside = available >= left + panelGap + metadata;
        out.detailsX = out.alongside ? left + panelGap : 0;
        out.detailsY = out.alongside ? 0 : (rows - 1) * rowStep + 49 + panelGap;
        out.bottom = std::max((rows - 1) * rowStep + 49, out.detailsY + detailRows * 19.0f) + 16;
        return out;
    }
    float readingWidth(const wchar_t* label, const std::wstring& value, float size,
                       const std::wstring& reserve = L"") {
        return std::max(
            {detailTextWidth(label, 12), detailTextWidth(value, size), detailTextWidth(reserve, size)});
    }
    float memoryReadingWidth(const wchar_t* label, double used, uint64_t total) {
        float result = readingWidth(label, memoryFraction(used, total), 21);
        // Reserve every possible unit transition for this device so normal
        // usage changes do not shift its columns or trigger needless reflow.
        if (total) {
            result = std::max(result, detailTextWidth(memoryFraction(static_cast<double>(total), total), 21));
            for (double boundary : {1024.0, 1048576.0, 1073741824.0, 1099511627776.0})
                if (total >= boundary)
                    result = std::max(result, detailTextWidth(memoryFraction(boundary - 1, total), 21));
        }
        return result;
    }
    ReadoutLayout gpuReadouts(const telemetry::Device& gpu, float available) {
        float label = detailTextWidth(L"Physical location:", 12);
        float value =
            std::max({detailTextWidth(L"PCIe 255, Dev 31, Fun 7", 11), detailTextWidth(gpu.driverVersion, 11),
                      detailTextWidth(gpu.driverDate, 11), detailTextWidth(gpu.physicalLocation, 11)});
        return compactReadouts(available,
                               {readingWidth(L"Utilization", number(gpu.usage, 0, L"%"), 24, L"100%"),
                                memoryReadingWidth(L"Dedicated GPU memory", gpu.dedicatedUsed, gpu.capacity),
                                memoryReadingWidth(L"Shared GPU memory", gpu.sharedUsed, gpu.sharedCapacity),
                                readingWidth(L"Temperature",
                                             number(gpu.temperature, 0,
                                                    L" \x00B0"
                                                    "C"),
                                             21,
                                             L"100 \x00B0"
                                             "C")},
                               label, value, 3, true);
    }
    ReadoutLayout diskReadouts(const telemetry::Device& disk, float available) {
        float label = std::max(detailTextWidth(L"System disk:", 12), detailTextWidth(L"Formatted:", 12));
        float value = std::max({detailTextWidth(L"SSD (NVMe)", 11), detailTextWidth(disk.type, 11),
                                detailTextWidth(disk.capacity ? capacity(disk.capacity) : L"", 11),
                                detailTextWidth(capacity(disk.storage.formattedCapacity), 11)});
        return compactReadouts(
            available,
            {readingWidth(L"Active time", number(disk.usage, 0, L"%"), 24, L"100%"),
             readingWidth(L"Average response time", number(disk.responseMs, 1, L" ms"), 24, L"9999.9 ms"),
             readingWidth(L"Read speed", throughput(disk.read), 21, L"0.0 MB/s"),
             readingWidth(L"Write speed", throughput(disk.write), 21, L"0.0 MB/s")},
            label, value, 5, false);
    }
    std::map<std::wstring, float> secondaryWidths;
    float secondarySize(const std::wstring& value, float available) {
        if (value.empty())
            return 12;
        auto found = secondaryWidths.find(value);
        if (found == secondaryWidths.end()) {
            Com<IDWriteTextLayout> layout;
            DWRITE_TEXT_METRICS metrics{};
            if (FAILED(writer->CreateTextLayout(value.c_str(), static_cast<UINT32>(value.size()), font(12),
                                                16384, 32, layout.put())) ||
                FAILED(layout->GetMetrics(&metrics)))
                return 11;
            if (secondaryWidths.size() >= 128)
                secondaryWidths.clear();
            found = secondaryWidths.emplace(value, metrics.widthIncludingTrailingWhitespace).first;
        }
        return found->second <= available ? 12 : 11;
    }
    void drawGpu(const telemetry::Device& gpu, float x, float right) {
        float w = right - x;
        auto readings = gpuReadouts(gpu, w);
        // Preserve useful chart heights on short windows; the GPU content
        // scrolls independently of the device rail instead of enlarging it.
        float extraDetails = std::max(0.0f, readings.bottom - 115);
        float engineH = std::max(68.0f, (height - 420 - extraDetails) * .30f);
        float memoryH = std::max(45.0f, (height - 420 - extraDetails) * .20f);
        float firstY = 145, secondY = firstY + 24 + engineH + 8;
        float dedicatedY = secondY + 24 + engineH + 10, sharedY = dedicatedY + 24 + memoryH + 8;
        float statsY = sharedY + 24 + memoryH + 24;
        float fullBottom = statsY + readings.bottom;
        float viewport = height - 100, total = fullBottom - 100;
        scroll = std::clamp(scroll, 0.0f, std::max(0.0f, total - viewport));
        target->PushAxisAlignedClip(D2D1::RectF(x - 4, 100, width, height), D2D1_ANTIALIAS_MODE_ALIASED);
        target->SetTransform(D2D1::Matrix3x2F::Translation(0, -scroll));
        text(gpu.name, {x, 107, w * .35f, 44}, 28, 0, 600);
        text(gpu.description, {x + w * .28f, 116, w * .72f, 32}, 15, 0, 400, DWRITE_TEXT_ALIGNMENT_TRAILING);
        const wchar_t* labels[] = {L"3D", L"Copy", L"Video Encode", L"Video Decode"};
        float gap = 12, cellW = (w - gap) * .5f;
        for (size_t i = 0; i < gpu.gpuEngines.size(); i++) {
            float bx = x + (i % 2) * (cellW + gap), by = i < 2 ? firstY : secondY;
            auto& series = gpu.gpuEngines[i];
            text(labels[i], {bx, by, cellW - 48, 22}, 12, palette.muted);
            text(number(series.usage, 0, L"%"), {bx + cellW - 48, by, 48, 22}, 11, palette.muted, 400,
                 DWRITE_TEXT_ALIGNMENT_TRAILING);
            graph({bx, by + 24, cellW, engineH}, gpu.color, series.history);
        }
        text(L"Dedicated GPU memory", {x, dedicatedY, w, 22}, 12, palette.muted);
        text(gpu.capacity ? capacity(gpu.capacity) : L"", {right - 100, dedicatedY, 100, 22}, 11,
             palette.muted, 400, DWRITE_TEXT_ALIGNMENT_TRAILING);
        graph({x, dedicatedY + 24, w, memoryH}, gpu.color, gpu.dedicatedHistory, false,
              static_cast<double>(gpu.capacity));
        text(L"Shared GPU memory", {x, sharedY, w, 22}, 12, palette.muted);
        text(gpu.sharedCapacity ? capacity(gpu.sharedCapacity) : L"", {right - 100, sharedY, 100, 22}, 11,
             palette.muted, 400, DWRITE_TEXT_ALIGNMENT_TRAILING);
        graph({x, sharedY + 24, w, memoryH}, gpu.color, gpu.sharedHistory, false,
              static_cast<double>(gpu.sharedCapacity));
        text(L"60 seconds", {x, sharedY + 24 + memoryH, w, 22}, 11, palette.muted);
        text(L"0", {right - 20, sharedY + 24 + memoryH, 20, 22}, 11, palette.muted, 400,
             DWRITE_TEXT_ALIGNMENT_TRAILING);
        const wchar_t* readingLabels[] = {L"Utilization", L"Dedicated GPU memory", L"Shared GPU memory",
                                          L"Temperature"};
        const std::wstring readingValues[] = {number(gpu.usage, 0, L"%"),
                                              memoryFraction(gpu.dedicatedUsed, gpu.capacity),
                                              memoryFraction(gpu.sharedUsed, gpu.sharedCapacity),
                                              number(gpu.temperature, 0,
                                                     L" \x00B0"
                                                     "C")};
        for (int i = 0; i < 4; i++) {
            auto b = readings.fields[i];
            stat(x + b.x, statsY + b.y, b.w, readingLabels[i], readingValues[i], i == 0 ? 25 : 22);
        }
        float dx = x + readings.detailsX, dy = statsY + readings.detailsY;
        const wchar_t* details[] = {L"Driver version:", L"Driver date:", L"Physical location:"};
        const std::wstring values[] = {gpu.driverVersion, gpu.driverDate, gpu.physicalLocation};
        for (int i = 0; i < 3; i++) {
            text(details[i], {dx, dy + i * 19.0f, readings.labelWidth, 18}, 12, palette.muted);
            text(values[i],
                 {dx + readings.labelWidth + 8, dy + i * 19.0f,
                  std::max(0.0f, right - dx - readings.labelWidth - 8), 18},
                 11, 0, 400);
        }
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        target->PopAxisAlignedClip();
        drawScrollbar(100, viewport, total);
    }
    std::vector<telemetry::Device> performanceDevices() const {
        std::vector<telemetry::Device> out;
        telemetry::Device cpu;
        cpu.key = L"cpu";
        cpu.name = L"CPU";
        cpu.description = core->info.name;
        cpu.color = 0x60CDFF;
        cpu.usage = totalCpu();
        cpu.history = std::isfinite(core->cpu) ? core->cpuHistory : devices->cpuHistory;
        out.push_back(cpu);
        telemetry::Device mem;
        mem.key = L"memory";
        mem.name = L"Memory";
        mem.color = 0xC79AF4;
        mem.capacity = core->memory.total;
        mem.usage = core->memory.valid
                        ? 100.0 * (core->memory.total - core->memory.available) / core->memory.total
                        : telemetry::unknown;
        mem.description =
            core->memory.valid
                ? capacity(core->memory.installed ? core->memory.installed : core->memory.total) +
                      (devices->memoryHardware.type.empty() ? L"" : L" " + devices->memoryHardware.type)
                : L"";
        mem.history = core->memoryHistory;
        out.push_back(mem);
        if (devices->ready)
            out.insert(out.end(), devices->devices.begin(), devices->devices.end());
        else {
            for (int i = 0; i < 3; i++) {
                telemetry::Device p;
                p.key = i == 0 ? L"pending:disk" : i == 1 ? L"pending:network" : L"pending:gpu";
                p.name = i == 0 ? L"Disk" : i == 1 ? L"Network" : L"GPU";
                p.kind = i == 0   ? telemetry::Kind::Disk
                         : i == 1 ? telemetry::Kind::Network
                                  : telemetry::Kind::Gpu;
                p.description = L"";
                p.color = i == 0 ? 0x87C879 : i == 1 ? 0xD9AB75 : 0x97A8F4;
                out.push_back(p);
            }
        }
        return out;
    }
    void drawPerformance() {
        auto list = performanceDevices();
        device = std::clamp(device, 0, static_cast<int>(list.size()) - 1);
        const auto& current = list[device];
        float left = navWidth(), side = performanceRailWidth();
        const float railHeight = height - 108;
        deviceScroll =
            std::clamp(deviceScroll, 0.0f,
                       std::max(0.0f, static_cast<float>(list.size()) * performanceRowHeight - railHeight));
        target->PushAxisAlignedClip(D2D1::RectF(left, 100, left + side + 4, height),
                                    D2D1_ANTIALIAS_MODE_ALIASED);
        for (size_t i = 0; i < list.size(); i++) {
            float y = 108 + static_cast<float>(i) * performanceRowHeight - deviceScroll;
            if (y + performanceRowHeight <= 100 || y >= height)
                continue;
            const auto& d = list[i];
            Box b{left + 4, y, side - 1, performanceRowHeight};
            if (static_cast<int>(i) == device)
                fill(b, palette.selected, 1, 4);
            else if (hover == 200 + static_cast<int>(i))
                fill(b, palette.hover, 1, 4);
            unsigned ink = d.color;
            if (!isDark && i == 0)
                ink = 0x117DBB;
            double maximum = 1;
            if (d.kind == telemetry::Kind::Network && d.key != L"cpu" && d.key != L"memory") {
                maximum = 1000;
                for (auto& p : d.history)
                    if (std::isfinite(p.value))
                        maximum = std::max(maximum, p.value * 1.15);
            }
            float contentY = y + performanceRowPadding;
            graph({left + 14, contentY + 10, 60, 40}, ink, d.history, true, maximum);
            text(performanceTitle(d), {left + 84, contentY + 3, side - 92, 22}, 15);
            std::wstring detail, reading;
            if (d.key == L"cpu")
                detail = number(totalCpu(), 0, L"%") + L"  " + number(cpuSpeed(), 2, L" GHz");
            else if (d.key == L"memory")
                detail = core->memory.valid
                             ? number((core->memory.total - core->memory.available) / 1073741824.0, 1) +
                                   L"/" + number(core->memory.total / 1073741824.0, 1, L" GB") + L" (" +
                                   number(d.usage, 0, L"%") + L")"
                             : L"";
            else if (d.kind == telemetry::Kind::Network) {
                detail = d.name;
                reading = L"S: " + throughput(d.write, true) + L"  R: " + throughput(d.read, true);
            } else if (d.kind == telemetry::Kind::Disk) {
                detail = d.type;
                reading = number(d.usage, 0, L"%");
            } else {
                detail = d.description;
                reading =
                    number(d.usage, 0, L"%") + (std::isfinite(d.temperature) ? L"  (" +
                                                                                   number(d.temperature, 0,
                                                                                          L" \x00B0"
                                                                                          "C") +
                                                                                   L")"
                                                                             : L"");
            }
            // Shared three-line geometry, with equal two-DIP top/bottom padding.
            text(detail, {left + 84, contentY + 25, side - 92, 17}, secondarySize(detail, side - 92),
                 palette.muted);
            text(reading, {left + 84, contentY + 42, side - 92, 17}, 11, palette.muted);
            Box hb = b;
            if (hb.y < 100) {
                hb.h -= 100 - hb.y;
                hb.y = 100;
            }
            hb.h = std::min(hb.h, height - hb.y);
            hit(200 + static_cast<int>(i), hb, d.description);
            if (focusArea == 3 && device == static_cast<int>(i))
                focusBox(b);
        }
        target->PopAxisAlignedClip();
        float x = left + side + 30, right = width - 32, w = right - x;
        if (device >= 2 && current.kind == telemetry::Kind::Gpu) {
            drawGpu(current, x, right);
            return;
        }
        text(performanceTitle(current), {x, 107, w * .5f, 44}, 28, 0, 600);
        text(current.description, {x + w * .35f, 116, w * .65f, 32}, 15, 0, 400,
             DWRITE_TEXT_ALIGNMENT_TRAILING);
        unsigned ink = current.color;
        if (!isDark && device == 0)
            ink = 0x117DBB;
        bool cpu = current.key == L"cpu", mem = current.key == L"memory",
             net = !cpu && !mem && current.kind == telemetry::Kind::Network;
        bool disk = !cpu && !mem && current.kind == telemetry::Kind::Disk;
        const wchar_t* axis = cpu                                     ? L"% Utilization"
                              : mem                                   ? L"Memory usage"
                              : net                                   ? L"Throughput"
                              : current.kind == telemetry::Kind::Disk ? L"Active time"
                                                                      : L"GPU utilization";
        double maximum = 1;
        if (net) {
            maximum = 1000;
            for (auto& p : current.history)
                if (std::isfinite(p.value))
                    maximum = std::max(maximum, p.value * 1.15);
        }
        text(axis, {x, 145, w, 22}, 12, palette.muted);
        std::wstring limit = mem   ? (core->memory.valid ? capacity(core->memory.total) : L"")
                             : net ? throughput(maximum / 8, true)
                                   : L"100%";
        text(limit, {right - 110, 145, 110, 22}, 11, palette.muted, 400, DWRITE_TEXT_ALIGNMENT_TRAILING);
        ReadoutLayout storageLayout;
        if (disk)
            storageLayout = diskReadouts(current, w);
        bool stackedStorage = disk && !storageLayout.alongside;
        float graphHeight = std::max(stackedStorage ? 80.0f : 110.0f, height - (stackedStorage ? 430 : 388));
        if (cpu && cpuGraphMode == 1 && core->info.logical) {
            unsigned count = core->info.logical, columns = 1;
            double best = 1e9;
            for (unsigned c = 1; c <= count; c++)
                if (count % c == 0) {
                    double ratio = (w / c) / (graphHeight / (count / c));
                    double score = std::abs(std::log(ratio / .85));
                    if (score < best) {
                        best = score;
                        columns = c;
                    }
                }
            unsigned rows = (count + columns - 1) / columns;
            float gap = 4, cellW = (w - gap * (columns - 1)) / columns,
                  cellH = (graphHeight - gap * (rows - 1)) / rows;
            const std::vector<telemetry::Point> empty;
            for (unsigned i = 0; i < count; i++) {
                Box b{x + (i % columns) * (cellW + gap), 170 + (i / columns) * (cellH + gap), cellW, cellH};
                unsigned group = 0, index = i;
                while (group + 1 < GetActiveProcessorGroupCount() &&
                       index >= GetActiveProcessorCount(group)) {
                    index -= GetActiveProcessorCount(group);
                    group++;
                }
                const telemetry::LogicalCpu* data = nullptr;
                for (auto& logical : devices->logicalCpu)
                    if (logical.group == group && logical.index == index) {
                        data = &logical;
                        break;
                    }
                graph(b, ink, data ? data->history : empty, false);
                if (data && data->parkingKnown && data->parked && b.w >= 40 && b.h >= 18) {
                    Box badge{b.x + b.w - 36, b.y + b.h - 15, 33, 12};
                    fill(badge, palette.surface, .9f, 2);
                    glyph(L"\xE8BE", {badge.x, badge.y, 10, 12}, 9, isDark ? 0x6CCB5F : 0x0F7B0F);
                    text(L"park", {badge.x + 11, badge.y, 22, 12}, 8, isDark ? 0x6CCB5F : 0x0F7B0F);
                }
                hit(3000 + static_cast<int>(i), b,
                    L"CPU " + std::to_wstring(i) +
                        (data && std::isfinite(data->usage) ? L" \x00B7 " + number(data->usage, 1, L"%")
                                                            : L"") +
                        (data && data->parkingKnown && data->parked ? L" \x00B7 Parked" : L""));
            }
        } else
            graph({x, 170, w, graphHeight}, ink, current.history, false, maximum,
                  mem    ? 36.0f
                  : disk ? 20.0f
                         : 0.0f);
        text(L"60 seconds", {x, 171 + graphHeight, w, 23}, 11, palette.muted);
        text(L"0", {right - 20, 171 + graphHeight, 20, 23}, 11, palette.muted, 400,
             DWRITE_TEXT_ALIGNMENT_TRAILING);
        float y = graphHeight + 198;
        if (cpu) {
            stat(x, y, 70, L"Utilization", number(totalCpu(), 0, L"%"), 25);
            stat(x + 74, y, 140, L"Speed", number(cpuSpeed(), 2, L" GHz"), 25);
            stat(x, y + 54, 70, L"Processes",
                 core->memory.performanceValid ? std::to_wstring(core->totalProcesses) : L"", 20);
            stat(x + 74, y + 54, 64, L"Threads",
                 core->memory.performanceValid ? std::to_wstring(core->totalThreads) : L"", 20);
            stat(x + 140, y + 54, 86, L"Handles",
                 core->memory.performanceValid ? std::to_wstring(core->totalHandles) : L"", 20);
            stat(x, y + 108, 250, L"Up time", core->ready ? uptime(core->upMilliseconds) : L"", 23);
            float fx = x + std::max(220.0f, w * .4f);
            const wchar_t* labels[] = {L"Base speed:",     L"Sockets:",  L"Cores:",    L"Logical processors:",
                                       L"Virtualization:", L"L1 cache:", L"L2 cache:", L"L3 cache:"};
            auto& info = core->info;
            std::wstring values[] = {number(info.baseGHz, 2, L" GHz"),
                                     info.sockets ? std::to_wstring(info.sockets) : L"",
                                     info.cores ? std::to_wstring(info.cores) : L"",
                                     info.logical ? std::to_wstring(info.logical) : L"",
                                     core->ready ? (info.virtualization ? L"Enabled" : L"Disabled") : L"",
                                     info.l1 ? capacity(info.l1) : L"",
                                     info.l2 ? capacity(info.l2) : L"",
                                     info.l3 ? capacity(info.l3) : L""};
            for (int i = 0; i < 8; i++) {
                text(labels[i], {fx, y + 1 + i * 19.0f, 108, 19}, 12, palette.muted);
                text(values[i], {fx + 113, y + 1 + i * 19.0f, std::max(0.0f, right - fx - 113), 19}, 11, 0,
                     400);
            }
        } else if (mem) {
            auto& m = core->memory;
            stat(x, y, 145, L"In use", m.valid ? capacity(m.total - m.available) : L"", 22);
            stat(x + 152, y, 140, L"Available", m.valid ? capacity(m.available) : L"", 22);
            stat(x, y + 54, 145, L"Committed",
                 m.performanceValid && m.commitLimit ? number(m.commit / 1073741824.0, 1) + L"/" +
                                                           number(m.commitLimit / 1073741824.0, 1, L" GB")
                                                     : L"",
                 20);
            stat(x + 152, y + 54, 140, L"Cached", m.performanceValid ? capacity(m.cache) : L"", 20);
            stat(x, y + 108, 145, L"Paged pool", m.performanceValid ? capacity(m.paged) : L"", 20);
            stat(x + 152, y + 108, 140, L"Non-paged pool", m.performanceValid ? capacity(m.nonpaged) : L"",
                 20);
            auto& hardware = devices->memoryHardware;
            float fx = x + std::max(270.0f, w * .48f);
            std::vector<unsigned> occupiedSlots;
            for (size_t i = 0; i < hardware.modules.size(); i++)
                if (hardware.modules[i].populated)
                    occupiedSlots.push_back(static_cast<unsigned>(i + 1));
            std::wstring slotNumbers;
            for (size_t i = 0; i < occupiedSlots.size(); i++) {
                if (i)
                    slotNumbers += i + 1 == occupiedSlots.size() ? L" and " : L", ";
                slotNumbers += std::to_wstring(occupiedSlots[i]);
            }
            const wchar_t* labels[] = {L"Speed:", L"Slots used:", L"Slots:", L"Form factor:",
                                       L"Hardware reserved:"};
            std::wstring values[] = {
                hardware.speed ? std::to_wstring(hardware.speed) + L" MT/s" : L"",
                hardware.valid ? std::to_wstring(hardware.used) + L" of " + std::to_wstring(hardware.slots)
                               : L"",
                slotNumbers, hardware.formFactor,
                m.valid && m.installed >= m.total ? capacity(m.installed - m.total) : L""};
            for (int i = 0; i < 5; i++) {
                text(labels[i], {fx, y + 1 + i * 19.0f, 108, 19}, 12, palette.muted);
                text(values[i], {fx + 113, y + 1 + i * 19.0f, std::max(0.0f, right - fx - 113), 19}, 11, 0,
                     400);
            }
            std::wstring slots;
            for (auto& module : hardware.modules) {
                if (!slots.empty())
                    slots += L"\n";
                slots +=
                    module.locator + L": " +
                    (module.populated
                         ? (module.capacityKnown ? capacity(module.capacity) : L"Installed") + L" " +
                               module.type +
                               (module.speed ? L" \x00B7 " + std::to_wstring(module.speed) + L" MT/s" : L"")
                         : L"Empty");
            }
            if (hardware.valid)
                hit(700, {fx, y + 19, std::max(0.0f, right - fx), 40}, slots);
        } else if (disk) {
            const wchar_t* readingLabels[] = {L"Active time", L"Average response time", L"Read speed",
                                              L"Write speed"};
            const std::wstring readingValues[] = {number(current.usage, 0, L"%"),
                                                  number(current.responseMs, 1, L" ms"),
                                                  throughput(current.read), throughput(current.write)};
            for (int i = 0; i < 4; i++) {
                auto b = storageLayout.fields[i];
                stat(x + b.x, y + b.y, b.w, readingLabels[i], readingValues[i], i < 2 ? 25 : 22);
            }
            float fx = x + storageLayout.detailsX, fy = y + storageLayout.detailsY;
            auto yesNo = [](std::optional<bool> value) -> std::wstring {
                return value ? (*value ? L"Yes" : L"No") : L"";
            };
            const wchar_t* labels[] = {L"Capacity:", L"Formatted:", L"System disk:", L"Page file:", L"Type:"};
            const std::wstring values[] = {current.capacity ? capacity(current.capacity) : L"",
                                           capacity(current.storage.formattedCapacity),
                                           yesNo(current.storage.systemDisk), yesNo(current.storage.pageFile),
                                           current.type};
            for (int i = 0; i < 5; i++) {
                text(labels[i], {fx, fy + i * 19.0f, storageLayout.labelWidth, 18}, 12, palette.muted);
                text(values[i],
                     {fx + storageLayout.labelWidth + 8, fy + i * 19.0f,
                      std::max(0.0f, right - fx - storageLayout.labelWidth - 8), 18},
                     11, 0, 400);
            }
        } else if (net) {
            stat(x, y, 180, L"Send", throughput(current.write, true), 25);
            stat(x + 200, y, 180, L"Receive", throughput(current.read, true), 25);
            const wchar_t* labels[] = {L"Adapter name:", L"Connection type:", L"IPv4 address:",
                                       L"IPv6 address:"};
            const std::wstring values[] = {current.name, current.type, current.ipv4, current.ipv6};
            for (int i = 0; i < 4; i++) {
                text(labels[i], {x, y + 60 + i * 22.0f, 110, 22}, 12, palette.muted);
                text(values[i], {x + 115, y + 60 + i * 22.0f, w - 115, 22}, 11, 0, 400);
            }
        }
        contentHeight = height - bodyTop();
        viewHeight = contentHeight;
        scroll = 0;
    }
    void drawGeneric() {
        Table table = makeTable(page, *core, *devices);
        float x = navWidth(), y = 100;
        float infoHeight = page == 2 ? 76 : page == 3 ? 40 : 0;
        if (page == 2) {
            text(L"Historical app usage is not collected in this version.", {x + 16, 110, width - x - 32, 26},
                 12, palette.muted);
            text(L"Current usage is available in Processes.", {x + 16, 139, width - x - 32, 25}, 12,
                 palette.muted);
        }
        if (page == 3)
            text(L"Last BIOS time:", {x + 16, 104, width - x - 40, 32}, 12, palette.muted, 400,
                 DWRITE_TEXT_ALIGNMENT_TRAILING);
        y += infoHeight;
        float totalW = 0;
        for (auto& c : table.columns)
            totalW += c.width;
        table.columns[0].width += std::max(0.0f, width - x - 12 - totalW);
        float cx = x - hscroll;
        std::vector<float> e{cx};
        for (auto& c : table.columns)
            e.push_back(e.back() + c.width);
        target->PushAxisAlignedClip(D2D1::RectF(x, 100, width, height), D2D1_ANTIALIAS_MODE_ALIASED);
        for (size_t i = 0; i < table.columns.size(); i++) {
            if (sortColumn == static_cast<int>(i))
                fill({e[i], y, table.columns[i].width, 40}, palette.hover);
            text(table.columns[i].label, {e[i] + 12, y, table.columns[i].width - 24, 40}, 12, 0, 400,
                 table.columns[i].right ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
            line(e[i], y, e[i], y + 40, palette.border);
            hit(500 + static_cast<int>(i), {e[i], y, table.columns[i].width, 40}, L"Sort by column");
            if (sortColumn == static_cast<int>(i))
                glyph(sortAscending ? L"\xE70E" : L"\xE70D", {e[i + 1] - 18, y, 12, 10}, 8, palette.muted);
        }
        line(x, y + 39.5f, width, y + 39.5f, palette.border);
        y += 40;
        float top = y, viewport = height - y - (e.back() + hscroll > width ? 12 : 0);
        visibleRows.clear();
        for (size_t i = 0; i < table.rows.size(); i++) {
            bool match = query.empty();
            for (auto& cell : table.rows[i].cells)
                if (matches(cell))
                    match = true;
            if (match)
                visibleRows.push_back(static_cast<int>(i));
        }
        if (sortColumn >= 0 && sortColumn < static_cast<int>(table.columns.size())) {
            int c = sortColumn;
            std::stable_sort(visibleRows.begin(), visibleRows.end(), [&](int a, int b) {
                auto av = lower(table.rows[a].cells[c]), bv = lower(table.rows[b].cells[c]);
                if (av.empty() != bv.empty())
                    return !av.empty();
                if (table.columns[c].right) {
                    auto clean = [](std::wstring s) {
                        s.erase(std::remove(s.begin(), s.end(), L','), s.end());
                        return wcstod(s.c_str(), nullptr);
                    };
                    double an = clean(av), bn = clean(bv);
                    if (an != bn)
                        return sortAscending ? an < bn : an > bn;
                }
                return sortAscending ? av < bv : av > bv;
            });
        }
        float total = static_cast<float>(visibleRows.size()) * 32;
        scroll = std::clamp(scroll, 0.0f, std::max(0.0f, total - viewport));
        y -= scroll;
        target->PushAxisAlignedClip(D2D1::RectF(x, top, width, top + viewport), D2D1_ANTIALIAS_MODE_ALIASED);
        for (int idx : visibleRows) {
            const auto& row = table.rows[idx];
            Box b{x, y, width - x - 10, 32};
            if (y + 32 > top && y < top + viewport) {
                if (selected == idx)
                    fill(b, palette.selected);
                else if (hover == 10000 + idx)
                    fill(b, palette.hover);
                hit(10000 + idx,
                    {x, std::max(top, y), b.w, std::min(top + viewport, y + 32) - std::max(top, y)},
                    row.cells[0]);
                for (size_t i = 0; i < table.columns.size(); i++) {
                    float indent = i == 0 && page != 6 ? 38 : 12;
                    if (i == 0 && page != 6)
                        icon(row.icon, {e[i] + 14, y + 8, 16, 16});
                    text(row.cells[i], {e[i] + indent, y, table.columns[i].width - indent - 12, 32}, 12, 0,
                         400,
                         table.columns[i].right ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                : DWRITE_TEXT_ALIGNMENT_LEADING);
                }
                if (focusArea == 2 && selected == idx)
                    focusBox(b);
            }
            y += 32;
        }
        if (visibleRows.empty())
            text(page == 2                                    ? L"No history collected"
                 : (page == 5 ? core->ready : devices->ready) ? L"No results found"
                                                              : L"",
                 {x + 24, top + 24, width - x - 48, 40}, 16, palette.muted);
        target->PopAxisAlignedClip();
        drawScrollbar(top, viewport, total);
        contentWidth = e.back() + hscroll - x;
        if (contentWidth > width - x) {
            float vw = width - x - 12, tw = std::max(30.0f, vw * vw / contentWidth);
            float thumbX = x + (contentWidth > vw ? hscroll / (contentWidth - vw) * (vw - tw) : 0);
            fill({x, height - 12, vw, 12}, palette.surface);
            fill({thumbX, height - 6, tw, 3}, isDark ? 0x999999 : 0x858585, 1, 2);
        }
        target->PopAxisAlignedClip();
    }
    void toggle(int id, Box b, bool on, const std::wstring& tip) {
        fill(b, on ? palette.accent : palette.hover, 1, b.h / 2);
        brush->SetColor(color(on ? (isDark ? 0x191919 : 0xffffff) : palette.text));
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(b.x + (on ? b.w - b.h / 2 : b.h / 2), b.y + b.h / 2),
                                          b.h / 2 - 4, b.h / 2 - 4),
                            brush.p);
        hit(id, b, tip + (on ? L": On" : L": Off"));
    }
    void drawSettings() {
        float x = navWidth() + 30, w = width - x - 30;
        const float fullHeight = 1387;
        scroll = std::clamp(scroll, 0.0f, std::max(0.0f, fullHeight - height));
        target->PushAxisAlignedClip(D2D1::RectF(navWidth(), 100, width, height), D2D1_ANTIALIAS_MODE_ALIASED);
        target->SetTransform(D2D1::Matrix3x2F::Translation(0, -scroll));
        hitOffsetY = -scroll;
        text(L"Personalization", {x, 120, w, 35}, 20, 0, 600);
        fill({x, 167, w, 174}, palette.window, 1, 5);
        glyph(L"\xE790", {x + 16, 183, 30, 30}, 20);
        text(L"App theme", {x + 62, 179, w - 80, 28}, 14, 0, 600);
        text(L"Choose which app theme to display", {x + 62, 206, w - 80, 25}, 12, palette.muted);
        const wchar_t* themes[] = {L"Use system setting", L"Dark", L"Light"};
        for (int i = 0; i < 3; i++) {
            float bx = x + 64 + i * 190.0f;
            Box b{bx, 260, 180, 46};
            if (hover == 600 + i)
                fill(b, palette.hover, 1, 4);
            brush->SetColor(color(themeMode == i ? palette.accent : palette.muted));
            target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(bx + 14, 283), 9, 9), brush.p,
                                themeMode == i ? 5.0f : 1.0f);
            text(themes[i], {bx + 34, 260, 144, 46}, 13);
            hit(600 + i, b, themes[i]);
        }
        text(L"General", {x, 358, w, 35}, 20, 0, 600);
        fill({x, 405, w, 85}, palette.window, 1, 5);
        glyph(L"\xE80F", {x + 16, 423, 30, 30}, 20);
        text(L"Default start page", {x + 62, 415, w - 300, 28}, 14, 0, 600);
        text(L"Choose the page shown when the app opens", {x + 62, 443, w - 300, 25}, 12, palette.muted);
        Box choice{x + w - 205, 425, 185, 42};
        fill(choice, palette.search, 1, 4);
        stroke(choice, palette.border, 1, 4);
        text(defaultTab < 0 ? L"Last saved page" : navigation::pages[defaultTab],
             {choice.x + 12, choice.y, choice.w - 42, 42}, 13);
        glyph(L"\xE70D", {choice.x + choice.w - 28, choice.y, 20, 42}, 10);
        hit(610, choice, L"Default start page");
        fill({x, 506, w, 85}, palette.window, 1, 5);
        glyph(L"\xE916", {x + 16, 524, 30, 30}, 20);
        text(L"Real time update speed", {x + 62, 516, w - 300, 28}, 14, 0, 600);
        text(L"Choose how often resource measurements refresh", {x + 62, 544, w - 300, 25}, 12,
             palette.muted);
        Box speed{x + w - 205, 526, 185, 42};
        fill(speed, palette.search, 1, 4);
        stroke(speed, palette.border, 1, 4);
        text(updateInterval == 500    ? L"High"
             : updateInterval == 1500 ? L"Low"
                                      : L"Normal",
             {speed.x + 12, speed.y, 143, 42}, 13);
        glyph(L"\xE70D", {speed.x + 157, speed.y, 20, 42}, 10);
        hit(630, speed, L"Low: 1.5 seconds | Normal: 1 second | High: 0.5 seconds");
        fill({x, 607, w, 119}, palette.window, 1, 5);
        glyph(L"\xE8A7", {x + 16, 625, 30, 30}, 20);
        text(L"Minimize to tray when closing", {x + 62, 617, w - 155, 28}, 14, 0, 600);
        text(L"Closing keeps the window ready to reopen", {x + 62, 645, w - 90, 25}, 12, palette.muted);
        text(L"All measurements pause while hidden or minimized. Use the tray menu to exit.",
             {x + 62, 675, w - 85, 23}, 12, palette.muted);
        toggle(640, {x + w - 62, 630, 42, 22}, closeToTray, L"Minimize to tray when closing");
        fill({x, 758, w, 119}, palette.window, 1, 5);
        glyph(L"\xEA18", {x + 16, 775, 30, 30}, 20);
        text(L"Run as administrator", {x + 62, 768, w - 155, 28}, 14, 0, 600);
        text(L"Request Windows permission at launch for full monitoring and process controls",
             {x + 62, 797, w - 90, 25}, 12, palette.muted);
        text(adminActive ? L"Administrator access is active" : L"Current session has standard access",
             {x + 62, 831, w - 290, 25}, 12, palette.muted);
        toggle(620, {x + w - 62, 781, 42, 22}, runAdmin, L"Run as administrator");
        if (!adminActive)
            button(621, {x + w - 223, 823, 205, 44}, L"", L"Restart as administrator");
        fill({x, 893, w, 115}, palette.window, 1, 5);
        glyph(L"\xE9D9", {x + 16, 911, 30, 30}, 20);
        text(L"Replace Task Manager", {x + 62, 903, w - 155, 28}, 14, 0, 600);
        text(L"Open this app when Windows Task Manager is requested", {x + 62, 931, w - 90, 25}, 12,
             palette.muted);
        std::wstring status = replacementPending
                                  ? L"The replacement change could not be applied. Toggle again to retry."
                              : replacement.kind == integration::ReplacementKind::Owned
                                  ? L"Replaced. Turn off before moving or deleting this executable."
                              : replacement.kind == integration::ReplacementKind::Other
                                  ? L"Another replacement is registered; its setting will be preserved."
                              : replacement.kind == integration::ReplacementKind::Unavailable
                                  ? L"Windows could not read the replacement setting."
                                  : L"Windows Task Manager is the default.";
        text(status, {x + 62, 964, w - 90, 27}, 12, palette.muted);
        toggle(660, {x + w - 62, 916, 42, 22}, desiredReplacement, L"Replace Task Manager");
        fill({x, 1024, w, 85}, palette.window, 1, 5);
        glyph(L"\xE777", {x + 16, 1042, 30, 30}, 20);
        text(L"Reset settings", {x + 62, 1034, w - 280, 28}, 14, 0, 600);
        text(L"Restore all settings to their defaults", {x + 62, 1062, w - 280, 25}, 12, palette.muted);
        button(650, {x + w - 184, 1044, 164, 42}, L"", L"Reset to defaults");
        text(L"Configuration", {x, 1131, w, 34}, 20, 0, 600);
        text(L"CleanTaskMan.ini beside this executable", {x, 1170, w - 180, 28}, 13);
        button(622, {x + w - 173, 1162, 170, 44}, L"", L"Open configuration file");
        if (settingsWriter.error())
            text(L"The configuration file could not be saved. Check folder write permission.",
                 {x, 1212, w, 26}, 12, 0xE06C75);
        else
            text(L"Settings save immediately. Window placement saves on exit only when changed.",
                 {x, 1212, w, 26}, 12, palette.muted);
        text(L"Clean Task Manager 0.7.0", {x, 1317, w, 28}, 14);
        text(L"Native Windows monitoring and process controls", {x, 1348, w, 25}, 12, palette.muted);
        hitOffsetY = 0;
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        target->PopAxisAlignedClip();
        drawScrollbar(100, height - 100, fullHeight - 100);
    }
    void draw() {
        HWND surface = drawingWindow();
        PAINTSTRUCT ps;
        BeginPaint(surface, &ps);
        if (!createGraphics()) {
            FillRect(ps.hdc, &ps.rcPaint, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            SetTextColor(ps.hdc, RGB(255, 255, 255));
            SetBkMode(ps.hdc, TRANSPARENT);
            RECT r{20, 20, 700, 100};
            DrawTextW(ps.hdc, L"Unable to initialize Direct2D. Close and reopen Clean Task Manager.", -1, &r,
                      DT_LEFT | DT_WORDBREAK);
            EndPaint(surface, &ps);
            return;
        }
        hits.clear();
        target->BeginDraw();
        target->Clear(color(palette.window));
        drawTitle();
        drawNav();
        drawCommands();
        if (page == 0)
            drawProcesses();
        else if (page == 1)
            drawPerformance();
        else if (page == 7)
            drawSettings();
        else
            drawGeneric();
        if (!tooltip.empty()) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(window, &pt);
            unsigned lines = 1;
            size_t longest = 0, currentLine = 0;
            for (wchar_t c : tooltip) {
                if (c == L'\n') {
                    lines++;
                    longest = std::max(longest, currentLine);
                    currentLine = 0;
                } else
                    currentLine++;
            }
            longest = std::max(longest, currentLine);
            float th = lines * 20.0f + 10;
            float tw = std::min(470.0f, static_cast<float>(longest) * 6 + 24);
            Box b{std::clamp(pt.x / scale + 12, 8.0f, std::max(8.0f, width - tw - 8)),
                  std::max(8.0f, std::min(pt.y / scale + 23, height - th - 8)), tw, th};
            fill(b, isDark ? 0x2B2B2B : 0xF8F8F8, 1, 4);
            stroke(b, palette.border, 1, 4);
            size_t at = 0;
            for (unsigned i = 0; i < lines; i++) {
                auto end = tooltip.find(L'\n', at);
                text(tooltip.substr(at, end == tooltip.npos ? end : end - at),
                     {b.x + 10, b.y + 5 + i * 20.0f, b.w - 20, 20}, 12);
                if (end == tooltip.npos)
                    break;
                at = end + 1;
            }
        }
        HRESULT hr = target->EndDraw();
        drawCount++;
        if (hr == D2DERR_RECREATE_TARGET) {
            brush.reset();
            target.reset();
            invalidate();
        }
        EndPaint(surface, &ps);
        synchronizeHover();
        if (!firstDraw && SUCCEEDED(hr) && IsWindowVisible(window) && !IsIconic(window)) {
            firstDraw = true;
            record("first_draw_submitted");
            PostMessageW(window, WM_APP + 1, 0, 0);
            if (!benchmarkFile.empty()) {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                double ms = (now.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart;
                HANDLE f = CreateFileW(benchmarkFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (f != INVALID_HANDLE_VALUE) {
                    char b[256];
                    int len = snprintf(
                        b, sizeof(b),
                        "{\"entry_to_first_draw_submit_ms\":%.3f,\"dpi\":%u,\"sample_data\":false}\n", ms,
                        GetDpiForWindow(window));
                    DWORD wrote;
                    WriteFile(f, b, static_cast<DWORD>(len), &wrote, nullptr);
                    CloseHandle(f);
                }
                SetTimer(window, 2, 120, nullptr);
            }
        }
    }
    void changePage(int p) {
        selectionKeys[page] = selectionKey();
        pageSort[page] = sortColumn;
        pageAscending[page] = sortAscending;
        page = p;
        selected = -1;
        sortColumn = pageSort[p];
        sortAscending = pageAscending[p];
        if (p != 7)
            lastContentPage = p;
        if (page == 7)
            refreshReplacement();
        query.clear();
        SetWindowTextW(search, L"");
        rebuildProcessRows();
        restoreSelection(selectionKeys[page]);
        scroll = 0;
        hscroll = 0;
        hover = -1;
        tooltip.clear();
        navFocus = p;
        layout();
        persistPreferences();
        SetFocus(window);
    }
    void unavailableCommand() {
        MessageBoxW(window, L"This command is not available yet.", L"Clean Task Manager",
                    MB_OK | MB_ICONINFORMATION);
    }
    std::vector<telemetry::Process> selectedProcesses() const {
        if (page == 0 && selected >= 0 && static_cast<size_t>(selected) < processRows.size())
            return processRows[selected].members;
        if (page == 5 && selected >= 0 && static_cast<size_t>(selected) < processes.size())
            return {processes[selected]};
        return {};
    }
    void errorMessage(const std::wstring& detail, DWORD error) {
        wchar_t* message = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, error, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
        auto text = detail + (error ? L"\n\nWindows error " + std::to_wstring(error) +
                                          (message ? L": " + std::wstring(message) : L"")
                                    : L"");
        if (message)
            LocalFree(message);
        MessageBoxW(window, text.c_str(), L"Clean Task Manager", MB_OK | MB_ICONERROR);
    }
    void processCommand(int command, std::vector<telemetry::Process> members) {
        if (members.empty() || actionBusy)
            return;
        if (command == 903 || command == 904) {
            auto& p = members.front();
            actions::Identity identity{p.pid, p.born};
            auto result =
                command == 903 ? processActions.location(identity) : processActions.properties(identity);
            if (!result.success)
                errorMessage(result.detail, result.error);
            return;
        }
        if (command == 902) {
            std::wstring prompt =
                L"End " + members.front().name +
                (members.size() > 1 ? L" (" + std::to_wstring(members.size()) + L" processes)" : L"") +
                L"?\n\nUnsaved work in these processes may be lost.";
            if (MessageBoxW(window, prompt.c_str(), L"End task", MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) !=
                IDYES)
                return;
        }
        bool enable = !std::all_of(members.begin(), members.end(), [](auto& p) { return p.efficient; });
        if (actionThread.joinable())
            actionThread.join();
        actionBusy = true;
        // Capture explicit PID + creation-time pairs before the worker starts.
        actionThread = std::thread([this, command, members = std::move(members), enable] {
            std::wstring failures;
            unsigned failed = 0;
            for (auto& p : members) {
                if (closing)
                    break;
                auto result = command == 902 ? processActions.end({p.pid, p.born})
                                             : processActions.efficiency({p.pid, p.born}, enable);
                if (!result.success) {
                    failed++;
                    if (failed <= 8)
                        failures += p.name + L" (PID " + std::to_wstring(p.pid) + L"): " + result.detail +
                                    L" [" + std::to_wstring(result.error) + L"]\n";
                }
            }
            if (failed > 8)
                failures += L"And " + std::to_wstring(failed - 8) + L" further processes.\n";
            {
                std::lock_guard lock(actionMutex);
                actionError = std::move(failures);
            }
            actionBusy = false;
            if (!closing)
                PostMessageW(window, WM_APP + 3, 0, 0);
        });
        invalidate();
    }
    void cpuMenu(float x, float y) {
        HMENU menu = CreatePopupMenu(), graphs = CreatePopupMenu();
        AppendMenuW(graphs, MF_STRING | (cpuGraphMode == 0 ? MF_CHECKED : 0), 930, L"Overall utilization");
        AppendMenuW(graphs, MF_STRING | (cpuGraphMode == 1 ? MF_CHECKED : 0), 931, L"Logical processors");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(graphs), L"Change graph to");
        POINT pt{static_cast<LONG>(x * scale), static_cast<LONG>(y * scale)};
        ClientToScreen(window, &pt);
        int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, window, nullptr);
        DestroyMenu(menu);
        if (command == 930 || command == 931) {
            cpuGraphMode = command - 930;
            persistPreferences();
            invalidate();
        }
    }
    void showMenu(float x, float y, bool row) {
        // The native menu pumps messages. Freeze its target before entering it.
        auto members = row ? selectedProcesses() : std::vector<telemetry::Process>{};
        auto name = members.empty() ? L"" : members.front().name;
        HMENU menu = CreatePopupMenu();
        if (row) {
            AppendMenuW(menu, MF_STRING, 901, L"Copy name");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            bool identified = !members.empty() && std::all_of(members.begin(), members.end(), [](auto& p) {
                return p.born && p.pid > 4 && p.pid != GetCurrentProcessId();
            });
            AppendMenuW(menu, MF_STRING | (!identified || actionBusy ? MF_GRAYED : 0), 902, L"End task");
            AppendMenuW(menu,
                        MF_STRING | (!identified || actionBusy ? MF_GRAYED : 0) |
                            (!members.empty() && std::all_of(members.begin(), members.end(),
                                                             [](auto& p) { return p.efficient; })
                                 ? MF_CHECKED
                                 : 0),
                        908, L"Efficiency mode");
            AppendMenuW(menu, MF_STRING | (members.empty() || !members.front().born ? MF_GRAYED : 0), 903,
                        L"Open file location");
            AppendMenuW(menu, MF_STRING | (members.empty() || !members.front().born ? MF_GRAYED : 0), 904,
                        L"Properties");
        } else {
            AppendMenuW(menu, MF_STRING | (navExpanded ? MF_CHECKED : 0), 905, L"Expand navigation");
            AppendMenuW(menu, MF_STRING, 906, L"Settings");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 907, L"About Clean Task Manager");
        }
        POINT pt{static_cast<LONG>(x * scale), static_cast<LONG>(y * scale)};
        ClientToScreen(window, &pt);
        int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, window, nullptr);
        DestroyMenu(menu);
        if (command == 905) {
            navExpanded = !navExpanded;
            hscroll = 0;
            layout();
            persistPreferences();
        }
        if (command == 906)
            changePage(7);
        if (command == 907)
            MessageBoxW(window,
                        L"Clean Task Manager 0.7.0\nNative Windows monitoring and process controls.\n\nBuilt "
                        L"by Reed and Morgan with direction and testing by Alex.",
                        L"About Clean Task Manager", MB_OK);
        if (command == 902 || command == 903 || command == 904 || command == 908)
            processCommand(command, std::move(members));
        if (command == 901 && !name.empty()) {
            if (OpenClipboard(window)) {
                HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (name.size() + 1) * sizeof(wchar_t));
                if (mem) {
                    void* dest = GlobalLock(mem);
                    if (dest) {
                        memcpy(dest, name.c_str(), (name.size() + 1) * sizeof(wchar_t));
                        GlobalUnlock(mem);
                        EmptyClipboard();
                        if (!SetClipboardData(CF_UNICODETEXT, mem))
                            GlobalFree(mem);
                    } else
                        GlobalFree(mem);
                }
                CloseClipboard();
            }
        }
        invalidate();
    }
    void activate(int id) {
        tooltip.clear();
        if (id == 10)
            ShowWindow(window, SW_MINIMIZE);
        else if (id == 11) {
            ShowWindow(window, IsZoomed(window) ? SW_RESTORE : SW_MAXIMIZE);
            capturePreferences();
        } else if (id == 12)
            PostMessageW(window, WM_CLOSE, 0, 0);
        else if (id == 14)
            SetFocus(search);
        else if (id == 90) {
            navExpanded = !navExpanded;
            hscroll = 0;
            layout();
            persistPreferences();
        } else if (id >= 100 && id < 108)
            changePage(id - 100);
        else if (id >= 200 && id < 300) {
            auto list = performanceDevices();
            if (static_cast<size_t>(id - 200) < list.size()) {
                device = id - 200;
                deviceKey = list[device].key;
                focusArea = 3;
                persistPreferences();
                invalidate();
            }
        } else if (id == 303) {
            showMenu(width - 20, 96, false);
        } else if ((id == 301 || id == 302) && (page == 0 || page == 5))
            processCommand(id == 301 ? 902 : 908, selectedProcesses());
        else if (id >= 300 && id < 306)
            unavailableCommand();
        else if (id >= 400 && id < 403) {
            collapsed[id - 400] = !collapsed[id - 400];
            scroll = 0;
            persistPreferences();
            invalidate();
        } else if (id >= 500 && id < 510) {
            auto keep = selectionKey();
            int col = id - 500;
            sortAscending = sortColumn == col ? !sortAscending : (col < 2);
            sortColumn = col;
            rebuildProcessRows();
            restoreSelection(keep);
            persistPreferences();
            invalidate();
        } else if (id >= 600 && id < 603) {
            if (themeMode != id - 600) {
                themeMode = id - 600;
                updateTheme();
                persistPreferences();
            }
        } else if (id == 610) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING | (defaultTab < 0 ? MF_CHECKED : 0), 940, L"Last saved page");
            for (int i = 0; i < 8; i++)
                AppendMenuW(menu, MF_STRING | (defaultTab == i ? MF_CHECKED : 0), 941 + i,
                            navigation::pages[i]);
            POINT pt{static_cast<LONG>((width - 235) * scale), static_cast<LONG>((467 - scroll) * scale)};
            ClientToScreen(window, &pt);
            int command =
                TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, window, nullptr);
            DestroyMenu(menu);
            if (command >= 940 && command <= 948 && defaultTab != command - 941) {
                defaultTab = command - 941;
                persistPreferences();
                invalidate();
            }
        } else if (id == 620) {
            runAdmin = !runAdmin;
            persistPreferences();
            invalidate();
        } else if (id == 621)
            restartAdministrator();
        else if (id == 630) {
            HMENU menu = CreatePopupMenu();
            const wchar_t* labels[] = {L"Low (1.5 seconds)", L"Normal (1 second)", L"High (0.5 seconds)"};
            unsigned values[] = {1500, 1000, 500};
            for (int i = 0; i < 3; i++)
                AppendMenuW(menu, MF_STRING | (updateInterval == values[i] ? MF_CHECKED : 0), 950 + i,
                            labels[i]);
            POINT pt{static_cast<LONG>((width - 235) * scale), static_cast<LONG>((568 - scroll) * scale)};
            ClientToScreen(window, &pt);
            int command =
                TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, window, nullptr);
            DestroyMenu(menu);
            if (command >= 950 && command <= 952 && updateInterval != values[command - 950]) {
                updateInterval = values[command - 950];
                collector->setUpdateInterval(updateInterval);
                persistPreferences();
                invalidate();
            }
        } else if (id == 640) {
            closeToTray = !closeToTray;
            persistPreferences();
            invalidate();
        } else if (id == 650)
            resetSettings();
        else if (id == 660) {
            refreshReplacement();
            changeReplacement(!desiredReplacement);
        } else if (id == 622) {
            if (GetFileAttributesW(settings::filePath().c_str()) != INVALID_FILE_ATTRIBUTES)
                ShellExecuteW(window, L"open", settings::filePath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            else
                MessageBoxW(window, L"The configuration file is created when you change a setting.",
                            L"Clean Task Manager", MB_OK | MB_ICONINFORMATION);
        } else if (id == 700) {
            for (auto& h : hits)
                if (h.id == 700) {
                    MessageBoxW(window, h.tip.c_str(), L"Memory slots reported by firmware", MB_OK);
                    break;
                }
        } else if (id == 801) {
            scroll = std::max(0.0f, scroll - viewHeight * .8f);
            invalidate();
        } else if (id == 802) {
            scroll += viewHeight * .8f;
            invalidate();
        } else if (id >= 20000 && id < 30000) {
            int idx = id - 20000;
            if (static_cast<size_t>(idx) < processRows.size()) {
                auto key = processRows[idx].groupKey;
                auto keep = selectionKey();
                if (expanded.count(key))
                    expanded.erase(key);
                else
                    expanded.insert(key);
                rebuildProcessRows();
                restoreSelection(keep);
                persistPreferences();
                invalidate();
            }
        } else if (id >= 10000 && id < 20000) {
            selected = id - 10000;
            focusArea = 2;
            SetFocus(window);
            invalidate();
        }
    }
    void ensureSelectionVisible() {
        if (page == 0) {
            float y = 0;
            auto order = processOrder();
            for (int g = 0; g < 3; g++) {
                bool any = false;
                for (int i : order)
                    if (processRows[i].value.group == g)
                        any = true;
                if (!any)
                    continue;
                y += 36;
                if (collapsed[g])
                    continue;
                for (int i : order)
                    if (processRows[i].value.group == g) {
                        if (i == selected) {
                            if (y < scroll)
                                scroll = y;
                            else if (y + 32 > scroll + viewHeight)
                                scroll = y + 32 - viewHeight;
                            invalidate();
                            return;
                        }
                        y += 32;
                    }
            }
        } else {
            auto it = std::find(visibleRows.begin(), visibleRows.end(), selected);
            if (it != visibleRows.end()) {
                float y = static_cast<float>(it - visibleRows.begin()) * 32;
                if (y < scroll)
                    scroll = y;
                else if (y + 32 > scroll + viewHeight)
                    scroll = y + 32 - viewHeight;
                invalidate();
            }
        }
    }
    void key(WPARAM key) {
        keyboard = true;
        tooltip.clear();
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        if (ctrl && key == 'F') {
            SetFocus(search);
            SendMessageW(search, EM_SETSEL, 0, -1);
            return;
        }
        if (ctrl && key == VK_TAB) {
            changePage((page + (shift ? 7 : 1)) % 8);
            return;
        }
        if (key == VK_TAB) {
            focusArea = focusArea == 1 ? (page == 1 ? 3 : 2) : 1;
            navFocus = page;
            SetFocus(window);
            invalidate();
            return;
        }
        if (key == VK_ESCAPE) {
            SetWindowTextW(search, L"");
            SetFocus(window);
            return;
        }
        if (focusArea == 1) {
            if (key == VK_UP || key == VK_DOWN) {
                navFocus = (navFocus + (key == VK_UP ? 7 : 1)) % 8;
                invalidate();
            }
            if (key == VK_RETURN || key == VK_SPACE)
                changePage(navFocus);
            if (key == VK_RIGHT) {
                focusArea = page == 1 ? 3 : 2;
                invalidate();
            }
        } else if (focusArea == 3) {
            if (key == VK_UP || key == VK_DOWN) {
                auto list = performanceDevices();
                device = std::clamp(device + (key == VK_UP ? -1 : 1), 0, static_cast<int>(list.size()) - 1);
                deviceKey = list[device].key;
                float y = 108 + device * performanceRowHeight;
                if (y - deviceScroll < 108)
                    deviceScroll = y - 108;
                else if (y + performanceRowHeight - deviceScroll > height)
                    deviceScroll = y + performanceRowHeight - height;
                persistPreferences();
                invalidate();
            }
            if (key == VK_LEFT) {
                focusArea = 1;
                invalidate();
            }
        } else if (page != 1 && page != 7) {
            if (key == VK_UP || key == VK_DOWN || key == VK_HOME || key == VK_END || key == VK_PRIOR ||
                key == VK_NEXT) {
                auto it = std::find(visibleRows.begin(), visibleRows.end(), selected);
                int idx = it == visibleRows.end() ? -1 : static_cast<int>(it - visibleRows.begin());
                int step = key == VK_UP ? -1 : key == VK_DOWN ? 1 : key == VK_PRIOR ? -10 : 10;
                idx = key == VK_HOME  ? 0
                      : key == VK_END ? static_cast<int>(visibleRows.size()) - 1
                                      : idx + step;
                if (!visibleRows.empty()) {
                    idx = std::clamp(idx, 0, static_cast<int>(visibleRows.size()) - 1);
                    selected = visibleRows[idx];
                    ensureSelectionVisible();
                }
                invalidate();
            }
            if (page == 0 && selected >= 0 && (key == VK_RIGHT || key == VK_LEFT || key == VK_SPACE)) {
                if (static_cast<size_t>(selected) < processRows.size()) {
                    auto group = processRows[selected].groupKey;
                    auto keep = selectionKey();
                    if (key == VK_LEFT)
                        expanded.erase(group);
                    else if (key == VK_RIGHT)
                        expanded.insert(group);
                    else if (expanded.count(group))
                        expanded.erase(group);
                    else
                        expanded.insert(group);
                    rebuildProcessRows();
                    restoreSelection(keep);
                    persistPreferences();
                    invalidate();
                }
            }
            if (key == VK_APPS) {
                showMenu(navWidth() + 100, 190, true);
            }
        }
    }
    static LRESULT CALLBACK editProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
        auto* self = reinterpret_cast<App*>(ref);
        if (msg == WM_ERASEBKGND) {
            RECT r;
            GetClientRect(h, &r);
            auto dc = reinterpret_cast<HDC>(wp);
            SetDCBrushColor(dc, gdiColor(self->palette.search));
            FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            return 1;
        }
        if (msg == WM_SYSKEYDOWN && wp >= '1' && wp <= '8') {
            self->changePage(static_cast<int>(wp - '1'));
            return 0;
        }
        if (msg == WM_KEYDOWN && wp == VK_TAB && GetKeyState(VK_CONTROL) < 0) {
            self->key(wp);
            return 0;
        }
        if (msg == WM_KEYDOWN && (wp == VK_ESCAPE || wp == VK_TAB)) {
            self->focusArea = wp == VK_TAB ? (self->page == 1 ? 3 : 2) : 1;
            SetFocus(self->window);
            if (wp == VK_ESCAPE)
                SetWindowTextW(h, L"");
            self->keyboard = true;
            self->invalidate();
            return 0;
        }
        if (msg == WM_KEYDOWN && wp == 'F' && GetKeyState(VK_CONTROL) < 0) {
            SendMessageW(h, EM_SETSEL, 0, -1);
            return 0;
        }
        return DefSubclassProc(h, msg, wp, lp);
    }
    static LRESULT CALLBACK canvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(hwnd, msg, wp, lp);
        if (msg == WM_NCHITTEST)
            return HTTRANSPARENT; // Input stays on the top-level custom frame.
        if (msg == WM_ERASEBKGND) {
            self->eraseOpaque(reinterpret_cast<HDC>(wp));
            return 1;
        }
        if (msg == WM_PAINT) {
            self->draw();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->window = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(hwnd, msg, wp, lp);
        auto& a = *self;
        if (a.restoreMsg && msg == a.restoreMsg) {
            if (a.closing)
                return 0;
            a.restoreWindow();
            return 1;
        }
        if (a.taskbarCreatedMsg && msg == a.taskbarCreatedMsg && a.trayHidden) {
            a.trayPresent = false;
            if (!a.addTray())
                a.restoreWindow();
            return 0;
        }
        switch (msg) {
        case WM_CREATE: {
            a.appIcon = nativeAppIcon();
            if (a.appIcon) {
                SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(a.appIcon));
                SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(a.appIcon));
            }
            a.restoreMsg = integration::restoreMessage();
            a.taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
            if (a.restoreMsg)
                ChangeWindowMessageFilterEx(hwnd, a.restoreMsg, MSGFLT_ALLOW, nullptr);
            if (a.taskbarCreatedMsg)
                ChangeWindowMessageFilterEx(hwnd, a.taskbarCreatedMsg, MSGFLT_ALLOW, nullptr);
            // Give Direct2D its own HWND. Native GDI controls are siblings,
            // rather than painting into the HWND that owns the swap surface.
            WNDCLASSW graphics{};
            graphics.hInstance = GetModuleHandleW(nullptr);
            graphics.lpfnWndProc = canvasProc;
            graphics.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            graphics.lpszClassName = L"CleanTaskMan.OpaqueCanvas.1";
            if (!RegisterClassW(&graphics) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                return -1;
            a.canvas =
                CreateWindowExW(0, graphics.lpszClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0,
                                1, 1, hwnd, nullptr, graphics.hInstance, self);
            if (!a.canvas)
                return -1;
            // Constant-alpha redirection gives the small GDI editor an opaque
            // retained surface, independent of Direct2D/DWM resize clipping.
            a.search = CreateWindowExW(
                WS_EX_LAYERED, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | ES_AUTOHSCROLL, 0, 0,
                200, 22, hwnd, reinterpret_cast<HMENU>(100), GetModuleHandleW(nullptr), nullptr);
            if (!a.search)
                return -1;
            if (!SetLayeredWindowAttributes(a.search, 0, 255, LWA_ALPHA))
                return -1;
            SetWindowPos(a.search, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            SendMessageW(a.search, EM_SETCUEBANNER, TRUE,
                         reinterpret_cast<LPARAM>(L"Type a name or PID to search"));
            SendMessageW(a.search, EM_SETLIMITTEXT, 256, 0);
            SetWindowSubclass(a.search, editProc, 1, reinterpret_cast<DWORD_PTR>(self));
            MARGINS m{};
            DwmExtendFrameIntoClientArea(hwnd, &m);
            int backdrop = 1;
            DwmSetWindowAttribute(hwnd, 38, &backdrop, sizeof(backdrop)); // DWMSBT_NONE
            int round = 2;
            DwmSetWindowAttribute(hwnd, 33, &round, sizeof(round));
            a.updateTheme();
            a.layout();
            return 0;
        }
        case WM_NCCALCSIZE:
            if (wp) {
                if (IsZoomed(hwnd)) {
                    auto* n = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                    MONITORINFO mi{sizeof(mi), {}, {}, 0};
                    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
                    n->rgrc[0] = mi.rcWork;
                }
                return WVR_REDRAW;
            }
            break;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            UINT dpi = GetDpiForWindow(hwnd);
            if (!dpi)
                dpi = 96;
            mm->ptMinTrackSize = {MulDiv(760, dpi, 96), MulDiv(540, dpi, 96)};
            return 0;
        }
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            RECT r;
            GetClientRect(hwnd, &r);
            int edge = std::max(5, static_cast<int>(6 * a.scale));
            if (!IsZoomed(hwnd)) {
                bool l = pt.x < edge, rr = pt.x >= r.right - edge, t = pt.y < edge,
                     b = pt.y >= r.bottom - edge;
                if (t && l)
                    return HTTOPLEFT;
                if (t && rr)
                    return HTTOPRIGHT;
                if (b && l)
                    return HTBOTTOMLEFT;
                if (b && rr)
                    return HTBOTTOMRIGHT;
                if (l)
                    return HTLEFT;
                if (rr)
                    return HTRIGHT;
                if (t)
                    return HTTOP;
                if (b)
                    return HTBOTTOM;
            }
            float x = pt.x / a.scale, y = pt.y / a.scale;
            int id = a.hitAt(x, y);
            if (id == 11)
                return HTMAXBUTTON;
            if (y < 48 && id == -1)
                return HTCAPTION;
            return HTCLIENT;
        }
        case WM_NCMOUSEMOVE:
            a.pointerKnown = false;
            if (wp == HTMAXBUTTON) {
                if (a.hover != 11) {
                    a.hover = 11;
                    a.invalidate();
                }
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE | TME_NONCLIENT, hwnd, 0};
                TrackMouseEvent(&t);
                return 0;
            }
            break;
        case WM_NCMOUSELEAVE:
            a.hover = -1;
            a.invalidate();
            return 0;
        case WM_NCLBUTTONDOWN:
            if (wp == HTMAXBUTTON) {
                a.pressed = 11;
                return 0;
            }
            break;
        case WM_NCLBUTTONUP:
            if (wp == HTMAXBUTTON) {
                if (a.pressed == 11)
                    a.activate(11);
                a.pressed = -1;
                return 0;
            }
            break;
        case WM_ERASEBKGND:
            a.eraseOpaque(reinterpret_cast<HDC>(wp));
            return 1;
        case WM_APP + 1:
            a.startCollectors();
            return 0;
        case WM_APP + 2:
            if (!a.closing && wp == a.collectorGeneration) {
                if (!a.metricsPaused)
                    a.acceptSnapshots();
                else
                    *a.notificationPending = false;
            }
            return 0;
        case WM_APP + 4:
            if (a.collectorStopping)
                a.finishCollectorStop();
            return 0;
        case WM_APP + 5:
            if (LOWORD(lp) == WM_CONTEXTMENU)
                a.trayMenu();
            else if (LOWORD(lp) == NIN_SELECT || LOWORD(lp) == NIN_KEYSELECT || LOWORD(lp) == WM_LBUTTONUP)
                a.restoreWindow();
            return 0;
        case WM_APP + 3: {
            std::wstring message;
            {
                std::lock_guard lock(a.actionMutex);
                message = std::move(a.actionError);
            }
            if (!message.empty() && !a.closing)
                MessageBoxW(hwnd, message.c_str(), L"Process action results", MB_OK | MB_ICONWARNING);
            a.invalidate();
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            a.eraseOpaque(ps.hdc);
            EndPaint(hwnd, &ps);
            if (a.canvas) {
                a.invalidate();
                a.present();
            }
            return 0;
        }
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED)
                a.pauseMetrics();
            else {
                a.prepareResume();
                a.layout();
            }
            return 0;
        case WM_WINDOWPOSCHANGED:
            if (!IsIconic(hwnd) && !IsZoomed(hwnd))
                GetWindowRect(hwnd, &a.normalRect);
            break;
        case WM_EXITSIZEMOVE:
            a.capturePreferences();
            return 0;
        case WM_DPICHANGED: {
            auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            a.layout();
            return 0;
        }
        case WM_SETTINGCHANGE:
            a.updateTheme();
            return 0;
        case WM_DISPLAYCHANGE:
            a.layout();
            return 0;
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wp);
            auto c = a.palette.text;
            SetTextColor(dc, RGB((c >> 16) & 255, (c >> 8) & 255, c & 255));
            c = a.palette.search;
            SetBkColor(dc, RGB((c >> 16) & 255, (c >> 8) & 255, c & 255));
            SetBkMode(dc, OPAQUE);
            return reinterpret_cast<LRESULT>(a.editBrush);
        }
        case WM_COMMAND:
            if (reinterpret_cast<HWND>(lp) == a.search && HIWORD(wp) == EN_CHANGE) {
                int len = GetWindowTextLengthW(a.search);
                std::wstring value(static_cast<size_t>(len) + 1, L'\0');
                GetWindowTextW(a.search, value.data(), len + 1);
                value.resize(len);
                a.query = value;
                a.scroll = 0;
                a.selected = -1;
                a.rebuildProcessRows();
                a.invalidate();
                return 0;
            }
            break;
        case WM_MOUSEMOVE: {
            float x = GET_X_LPARAM(lp) / a.scale, y = GET_Y_LPARAM(lp) / a.scale;
            a.pointerX = x;
            a.pointerY = y;
            a.pointerKnown = true;
            if (a.draggingScroll) {
                float thumb = std::max(28.0f, a.viewHeight * a.viewHeight / std::max(1.0f, a.contentHeight));
                float travel = a.viewHeight - thumb;
                if (travel > 0)
                    a.scroll = std::clamp(a.dragStartScroll +
                                              (y - a.dragStartY) / travel * (a.contentHeight - a.viewHeight),
                                          0.0f, std::max(0.0f, a.contentHeight - a.viewHeight));
                a.invalidate();
                return 0;
            }
            int id = a.hitAt(x, y);
            if (id != a.hover) {
                a.hover = id;
                a.tooltip.clear();
                a.invalidate();
            }
            TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE | TME_HOVER, hwnd, 650};
            TrackMouseEvent(&t);
            return 0;
        }
        case WM_MOUSEHOVER:
            for (auto& h : a.hits)
                if (h.id == a.hover && !h.tip.empty() && (h.id < 1000 || h.tip.size() > 32)) {
                    a.tooltip = h.tip;
                    a.invalidate();
                    break;
                }
            return 0;
        case WM_MOUSELEAVE:
            a.pointerKnown = false;
            a.hover = -1;
            a.tooltip.clear();
            a.invalidate();
            return 0;
        case WM_LBUTTONDOWN: {
            a.keyboard = false;
            float x = GET_X_LPARAM(lp) / a.scale, y = GET_Y_LPARAM(lp) / a.scale;
            a.pressed = a.hitAt(x, y);
            SetFocus(hwnd);
            a.tooltip.clear();
            a.pressedPage = a.page;
            a.pressedKey = a.inputKey(a.pressed);
            a.pressedCoreRevision = a.core->revision;
            a.pressedDeviceRevision = a.devices->revision;
            if (a.pressed == 800) {
                a.draggingScroll = true;
                a.dragStartY = y;
                a.dragStartScroll = a.scroll;
                SetCapture(hwnd);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (a.draggingScroll) {
                a.draggingScroll = false;
                ReleaseCapture();
                a.pressed = -1;
                return 0;
            }
            int id = a.hitAt(GET_X_LPARAM(lp) / a.scale, GET_Y_LPARAM(lp) / a.scale);
            bool same = a.pressed == id, safe = a.pressedPage == a.page;
            bool dynamic = (a.pressed >= 10000 && a.pressed < 30000) || (a.pressed >= 200 && a.pressed < 300);
            if (dynamic) {
                if (!a.pressedKey.empty())
                    same = a.inputKey(id) == a.pressedKey;
                else
                    safe = safe && a.core->revision == a.pressedCoreRevision &&
                           a.devices->revision == a.pressedDeviceRevision;
            }
            if (same && safe)
                a.activate(id);
            a.pressed = -1;
            a.pressedKey.clear();
            return 0;
        }
        case WM_CAPTURECHANGED:
            a.draggingScroll = false;
            return 0;
        case WM_CONTEXTMENU: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (pt.x == -1) {
                a.showMenu(a.navWidth() + 100, 190, true);
                return 0;
            } else
                ScreenToClient(hwnd, &pt);
            int id = a.hitAt(pt.x / a.scale, pt.y / a.scale);
            if (id >= 10000 && id < 20000) {
                a.selected = id - 10000;
                a.invalidate();
                a.showMenu(pt.x / a.scale, pt.y / a.scale, true);
            } else if (a.page == 1 && a.deviceKey == L"cpu" && pt.x / a.scale > a.navWidth() + 230 &&
                       pt.y / a.scale >= 145)
                a.cpuMenu(pt.x / a.scale, pt.y / a.scale);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            float delta = GET_WHEEL_DELTA_WPARAM(wp) / static_cast<float>(WHEEL_DELTA);
            POINT cursor{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &cursor);
            if (a.page == 1 && cursor.x / a.scale < a.navWidth() + a.performanceRailWidth() + 4) {
                auto count = a.performanceDevices().size();
                a.deviceScroll = std::clamp(
                    a.deviceScroll - delta * performanceRowHeight, 0.0f,
                    std::max(0.0f, static_cast<float>(count) * performanceRowHeight - (a.height - 108)));
                a.invalidate();
                return 0;
            }
            if (GetKeyState(VK_SHIFT) < 0) {
                a.hscroll = std::clamp(a.hscroll - delta * 96, 0.0f,
                                       std::max(0.0f, a.contentWidth - (a.width - a.navWidth() - 12)));
            } else
                a.scroll =
                    std::clamp(a.scroll - delta * 96, 0.0f, std::max(0.0f, a.contentHeight - a.viewHeight));
            a.tooltip.clear();
            a.invalidate();
            return 0;
        }
        case WM_MOUSEHWHEEL:
            a.hscroll =
                std::clamp(a.hscroll + GET_WHEEL_DELTA_WPARAM(wp) / static_cast<float>(WHEEL_DELTA) * 64,
                           0.0f, std::max(0.0f, a.contentWidth - (a.width - a.navWidth() - 12)));
            a.invalidate();
            return 0;
        case WM_KEYDOWN:
            a.key(wp);
            return 0;
        case WM_SYSKEYDOWN:
            if (wp >= '1' && wp <= '8') {
                a.changePage(static_cast<int>(wp - '1'));
                return 0;
            }
            break;
        case WM_TIMER:
            if (wp == 2 || wp == 3) {
                KillTimer(hwnd, static_cast<UINT_PTR>(wp));
                a.forceExit = true;
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
            return 0;
        case WM_CLOSE:
            if (a.closeToTray && !a.forceExit) {
                a.hideToTray();
                return 0;
            }
            a.closing = true;
            a.collectorGeneration++;
            a.collector->requestStop();
            ShowWindow(hwnd, SW_HIDE);
            a.queuePlacementOnExit();
            a.removeTray();
            a.record("window_closing");
            DestroyWindow(hwnd);
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wp) {
                // Session termination need not dispatch another posted message.
                // Drain only changed placement before returning.
                a.closing = true;
                a.collectorGeneration++;
                a.collector->requestStop();
                a.queuePlacementOnExit();
                if (a.placementQueued)
                    a.placementWriter.flush();
                a.forceExit = true;
                a.removeTray();
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_DESTROY:
            a.closing = true;
            a.collector->requestStop();
            a.removeTray();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    LARGE_INTEGER entry, frequency;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&entry);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    struct Apartment {
        HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ~Apartment() {
            if (SUCCEEDED(result))
                CoUninitialize();
        }
    } apartment;
    struct Instance {
        HANDLE handle = nullptr;
        bool owned = false;
        ~Instance() {
            if (handle) {
                if (owned)
                    ReleaseMutex(handle);
                CloseHandle(handle);
            }
        }
    } instanceLock;
    Instance bootstrapLock;
    App app;
    app.start = entry;
    app.frequency = frequency;
    app.selections.fill(-1);
    app.loadPreferences(settings::load(settings::filePath()));
    app.adminActive = activity::elevated();
    app.persistedPreferences = app.preferences;
    INITCOMMONCONTROLSEX ic{sizeof(ic), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&ic);
    int argc = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool noAdmin = false, elevationAttempted = false;
    DWORD handoffPid = 0;
    uint64_t handoffBorn = 0;
    std::wstring handoffToken, forwarded;
    std::optional<settings::Values> handoffState;
    for (int i = 1; args && i < argc; i++) {
        std::wstring arg = args[i];
        if (arg == L"--no-admin")
            noAdmin = true;
        if (arg == L"--elevated")
            elevationAttempted = true;
        if (arg.rfind(L"--handoff-pid=", 0) == 0)
            handoffPid = wcstoul(arg.c_str() + 14, nullptr, 10);
        if (arg.rfind(L"--handoff-born=", 0) == 0)
            handoffBorn = wcstoull(arg.c_str() + 15, nullptr, 10);
        if (arg.rfind(L"--handoff-state=", 0) == 0)
            handoffToken = arg.substr(16);
        if (arg != L"--elevated" && arg != L"--no-admin" && arg.rfind(L"--handoff-", 0) != 0)
            forwarded += L" " + App::quoteArgument(arg);
        if (arg.rfind(L"--benchmark-file=", 0) == 0)
            app.benchmarkFile = arg.substr(17);
        if (arg == L"--light")
            app.themeMode = 2;
        if (arg == L"--dark")
            app.themeMode = 1;
        if (arg == L"--expanded")
            app.navExpanded = true;
        if (arg.rfind(L"--diagnostics=", 0) == 0)
            app.diagnosticsFile = arg.substr(14);
        if (arg.rfind(L"--sensor-delay-ms=", 0) == 0)
            app.sensorDelay =
                static_cast<unsigned>(std::clamp(wcstol(arg.c_str() + 18, nullptr, 10), 0L, 60000L));
        if (arg.rfind(L"--exit-after-ms=", 0) == 0)
            app.exitAfter =
                static_cast<unsigned>(std::clamp(wcstol(arg.c_str() + 16, nullptr, 10), 1L, 600000L));
        if (arg == L"--page=performance") {
            app.page = 1;
            app.navFocus = 1;
        }
    }
    if (args)
        LocalFree(args);
    auto className = integration::instanceClass();
    UINT restore = integration::restoreMessage();
    if (handoffPid && app.adminActive) {
        HANDLE parent = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, handoffPid);
        if (parent) {
            wchar_t path[32768]{};
            DWORD length = 32768;
            FILETIME born{}, exited{}, k{}, u{};
            if (handoffBorn && GetProcessTimes(parent, &born, &exited, &k, &u) &&
                handoffBorn == ((uint64_t(born.dwHighDateTime) << 32) | born.dwLowDateTime) &&
                QueryFullProcessImageNameW(parent, 0, path, &length) &&
                _wcsicmp(path, integration::executablePath().c_str()) == 0) {
                auto payload = integration::SettingsHandoff::receive(handoffToken);
                if (!payload) {
                    CloseHandle(parent);
                    return 0;
                }
                handoffState = settings::parse(*payload);
                // Commit transfers responsibility for the copied, unsaved state.
                // Keep it alive until the validated parent actually exits.
                DWORD waited = WaitForSingleObject(parent, INFINITE);
                CloseHandle(parent);
                parent = nullptr;
                if (waited != WAIT_OBJECT_0) {
                    MessageBoxW(nullptr,
                                L"The previous window is still closing. Reopen the app after it exits.",
                                L"Clean Task Manager", MB_OK | MB_ICONWARNING);
                    return 0;
                }
            }
            if (parent)
                CloseHandle(parent);
        }
    }
    if (handoffState) {
        auto state = *handoffState;
        int active = settings::integer(state, "Transient.ActivePage", 0, 0, 7);
        settings::Values baseline;
        for (auto& [key, value] : state)
            if (key.rfind("Transient.Baseline", 0) == 0)
                baseline["Window." + key.substr(18)] = value;
        app.placementBaseline = settings::readPlacement(baseline);
        bool desired = settings::integer(state, "Transient.DesiredReplacement", 0, 0, 1) != 0;
        for (auto it = state.begin(); it != state.end();)
            if (it->first.rfind("Transient.", 0) == 0)
                it = state.erase(it);
            else
                ++it;
        app.loadPreferences(std::move(state));
        app.page = app.navFocus = active;
        app.desiredReplacement = desired;
    }
    if (integration::restoreExisting(className, restore))
        return 0;
    if (app.runAdmin && !app.adminActive && !noAdmin && !elevationAttempted) {
        // Coalesce simultaneous cold launches while the first UAC request is
        // outstanding. The elevated child skips this bootstrap-only lock.
        auto bootstrapName = L"Local\\" + className + L".Bootstrap";
        bootstrapLock.handle = CreateMutexW(nullptr, TRUE, bootstrapName.c_str());
        DWORD gateError = GetLastError();
        if (bootstrapLock.handle) {
            if (gateError == ERROR_ALREADY_EXISTS) {
                DWORD waited = WaitForSingleObject(bootstrapLock.handle, 0);
                if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED)
                    return 0;
            }
            bootstrapLock.owned = true;
        } else if (gateError == ERROR_ACCESS_DENIED)
            return 0;
        if (integration::restoreExisting(className, restore))
            return 0;
        wchar_t exe[32768]{};
        GetModuleFileNameW(nullptr, exe, 32768);
        auto parameters = L"--elevated" + forwarded;
        SHELLEXECUTEINFOW request{};
        request.cbSize = sizeof(request);
        request.lpVerb = L"runas";
        request.lpFile = exe;
        request.lpParameters = parameters.c_str();
        request.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&request)) {
            for (unsigned i = 0; i < 100; i++) {
                if (FindWindowW(className.c_str(), nullptr))
                    break;
                Sleep(20);
            }
            return 0;
        }
        // A declined UAC prompt opens a usable standard-access session, without a loop.
        if (GetLastError() != ERROR_CANCELLED)
            MessageBoxW(nullptr,
                        L"Windows could not start the administrator session. Opening with standard access.",
                        L"Clean Task Manager", MB_OK | MB_ICONWARNING);
    }
    auto mutexName = L"Local\\" + className;
    instanceLock.handle = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    DWORD instanceError = GetLastError();
    if (instanceLock.handle && instanceError != ERROR_ALREADY_EXISTS)
        instanceLock.owned = true;
    if (instanceLock.handle && instanceError == ERROR_ALREADY_EXISTS) {
        for (unsigned attempt = 0; attempt < 100 && !instanceLock.owned; attempt++) {
            if (integration::restoreExisting(className, restore))
                return 0;
            DWORD waited = WaitForSingleObject(instanceLock.handle, 20);
            if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED)
                instanceLock.owned = true;
            else if (waited != WAIT_TIMEOUT)
                break;
        }
        if (!instanceLock.owned) {
            MessageBoxW(nullptr,
                        L"Another instance exists, but Windows could not restore its window. Try again after "
                        L"it finishes starting or closing.",
                        L"Clean Task Manager", MB_OK | MB_ICONWARNING);
            return 0;
        }
    } else if (!instanceLock.handle && instanceError == ERROR_ACCESS_DENIED) {
        if (!integration::restoreExisting(className, restore, 100))
            MessageBoxW(nullptr,
                        L"Windows denied access to the existing instance and could not restore its window.",
                        L"Clean Task Manager", MB_OK | MB_ICONWARNING);
        return 0;
    }
    if (!instanceLock.handle) {
        MessageBoxW(nullptr, L"Windows could not establish the application instance.", L"Clean Task Manager",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    app.refreshReplacement(!handoffState);
    if (handoffState) {
        app.desiredReplacement =
            settings::integer(*handoffState, "Transient.DesiredReplacement", 0, 0, 1) != 0;
        app.replacementPending =
            app.desiredReplacement != (app.replacement.kind == integration::ReplacementKind::Owned);
    }
    if (!handoffState) {
        auto values = settings::load(settings::placementPath());
        if (settings::overlayPlacement(app.preferences, values)) {
            app.savedMaximized = settings::integer(app.preferences, "Window.Maximized", 0, 0, 1) != 0;
        }
    }
    app.sortColumn = app.pageSort[app.page];
    app.sortAscending = app.pageAscending[app.page];
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = App::proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = className.c_str();
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc))
        return 1;
    UINT dpi = GetDpiForSystem();
    int w = MulDiv(1180, dpi, 96), h = MulDiv(780, dpi, 96);
    POINT center{GetSystemMetrics(SM_CXSCREEN) / 2, GetSystemMetrics(SM_CYSCREEN) / 2};
    MONITORINFO mi{sizeof(mi), {}, {}, 0};
    GetMonitorInfoW(MonitorFromPoint(center, MONITOR_DEFAULTTOPRIMARY), &mi);
    int x = mi.rcWork.left + std::max(0L, ((mi.rcWork.right - mi.rcWork.left) - w) / 2);
    int y = mi.rcWork.top + std::max(0L, ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2);
    if (app.preferences.count("Window.Width")) {
        x = settings::integer(app.preferences, "Window.Left", x, -100000, 100000);
        y = settings::integer(app.preferences, "Window.Top", y, -100000, 100000);
        w = settings::integer(app.preferences, "Window.Width", w, 320, 40000);
        h = settings::integer(app.preferences, "Window.Height", h, 240, 40000);
        RECT saved{x, y, x + w, y + h};
        GetMonitorInfoW(MonitorFromRect(&saved, MONITOR_DEFAULTTONEAREST), &mi);
    }
    int workWidth = mi.rcWork.right - mi.rcWork.left, workHeight = mi.rcWork.bottom - mi.rcWork.top;
    w = std::clamp(w, std::min(MulDiv(760, dpi, 96), workWidth), workWidth);
    h = std::clamp(h, std::min(MulDiv(540, dpi, 96), workHeight), workHeight);
    x = std::clamp<int>(x, mi.rcWork.left, mi.rcWork.right - w);
    y = std::clamp<int>(y, mi.rcWork.top, mi.rcWork.bottom - h);
    app.normalRect = {x, y, x + w, y + h};
    HWND hwnd =
        CreateWindowExW(0, wc.lpszClassName, L"Clean Task Manager", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x,
                        y, w, h, nullptr, nullptr, instance, &app);
    if (!hwnd)
        return 2;
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
    // Prepare the complete empty frame and font resources while hidden.
    // The visible first draw is separately gated; no telemetry can start here.
    app.draw();
    ShowWindow(hwnd, app.savedMaximized ? SW_SHOWMAXIMIZED : show == SW_HIDE ? SW_SHOWNORMAL : show);
    app.invalidate();
    app.present();
    if (!app.placementBaseline)
        app.placementBaseline = app.currentPlacement();
    if (app.exitAfter)
        SetTimer(hwnd, 3, app.exitAfter, nullptr);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

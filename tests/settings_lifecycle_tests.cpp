// Exercises production state transitions using a private, never-shown window.
// This does not automate or send input to the user's application windows.
#define wWinMain CleanTaskManEntryForTests
#include "../src/main.cpp"
#undef wWinMain
#include <cstdio>

namespace {
int failures = 0;
UINT fixtureRestoreMessage = 0;
unsigned restoreCalls = 0;
bool emptyRestorePayload = true;
LRESULT CALLBACK restoreFixture(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == fixtureRestoreMessage) {
        restoreCalls++;
        emptyRestorePayload = emptyRestorePayload && !wp && !lp;
        return 1;
    }
    return DefWindowProcW(window, message, wp, lp);
}
void check(bool good, const char* message) {
    printf("%s %s\n", good ? "PASS" : "FAIL", message);
    fflush(stdout);
    if (!good)
        failures++;
}
template <class F> bool until(F f, unsigned ms = 4000) {
    auto start = GetTickCount64();
    while (GetTickCount64() - start < ms) {
        if (f())
            return true;
        Sleep(5);
    }
    return false;
}
std::wstring regString(HKEY key, const wchar_t* name) {
    wchar_t data[1024]{};
    DWORD bytes = sizeof(data);
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, data, &bytes) != ERROR_SUCCESS)
        return L"";
    return data;
}
} // namespace
int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    DeleteFileW(settings::filePath().c_str());
    DeleteFileW(settings::placementPath().c_str());
    {
        auto transfer = integration::SettingsHandoff::create("[Window]\nLeft=123\n");
        std::optional<std::string> received;
        std::thread reader([&] { received = integration::SettingsHandoff::receive(transfer->token()); });
        bool ready = transfer->waitReady(2000);
        transfer->commit();
        reader.join();
        check(ready && received && *received == "[Window]\nLeft=123\n",
              "two-phase RAM handoff preserves unsaved settings without a file");
        check(!integration::SettingsHandoff::receive(L"invalid") &&
                  !integration::SettingsHandoff::create(std::string(262145, 'x')),
              "handoff rejects invalid tokens and oversized payloads");
    }
    auto key = L"Software\\CleanTaskMan.QA." + std::to_wstring(GetCurrentProcessId());
    HKEY opened = nullptr;
    check(RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY,
                          nullptr, &opened, nullptr) == ERROR_SUCCESS,
          "isolated HKCU replacement fixture created (no host IFEO writes)");
    if (!opened)
        return 1;
    const wchar_t* keep = L"preserve unrelated setting";
    RegSetValueExW(opened, L"Unrelated", 0, REG_SZ, reinterpret_cast<const BYTE*>(keep),
                   static_cast<DWORD>((wcslen(keep) + 1) * 2));
    HKEY child = nullptr;
    RegCreateKeyExW(opened, L"UnrelatedSubkey", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &child, nullptr);
    if (child)
        RegCloseKey(child);
    integration::TaskManager manager(L"C:\\Portable App\\CleanTaskMan.exe", HKEY_CURRENT_USER, key);
    check(manager.state().kind == integration::ReplacementKind::Default,
          "absent Debugger means default Task Manager");
    check(manager.set(true) == 0 && manager.state().kind == integration::ReplacementKind::Owned,
          "replacement writes exact quoted executable and marker");
    check(regString(opened, L"Debugger") ==
              L"\"C:\\Portable App\\CleanTaskMan.exe\" --task-manager-replacement",
          "spaces in executable path are quoted");
    check(manager.set(true) == 0, "replacement enable is idempotent");
    check(manager.set(false) == 0 && manager.state().kind == integration::ReplacementKind::Default,
          "replacement disable removes owned value");
    auto ownedExpand = manager.command();
    RegSetValueExW(opened, L"Debugger", 0, REG_EXPAND_SZ, reinterpret_cast<const BYTE*>(ownedExpand.c_str()),
                   static_cast<DWORD>((ownedExpand.size() + 1) * 2));
    check(manager.state().kind == integration::ReplacementKind::Owned && manager.set(false) == 0,
          "exact owned expandable-string values can also be restored");
    check(regString(opened, L"Unrelated") == keep &&
              RegOpenKeyExW(opened, L"UnrelatedSubkey", 0, KEY_READ, &child) == 0,
          "unrelated values and subkeys survive restore");
    if (child)
        RegCloseKey(child);
    const wchar_t* foreign = L"\"C:\\Other App\\Monitor.exe\"";
    RegSetValueExW(opened, L"Debugger", 0, REG_SZ, reinterpret_cast<const BYTE*>(foreign),
                   static_cast<DWORD>((wcslen(foreign) + 1) * 2));
    check(manager.set(true) == ERROR_ALREADY_EXISTS && manager.set(false) == ERROR_ALREADY_EXISTS &&
              regString(opened, L"Debugger") == foreign,
          "foreign replacement is preserved on enable and disable");
    RegDeleteValueW(opened, L"Debugger");
    manager.set(true);
    auto prior = manager.command();
    integration::TaskManager relocated(L"D:\\New Folder\\CleanTaskMan.exe", HKEY_CURRENT_USER, key);
    check(relocated.state(prior).kind == integration::ReplacementKind::Owned &&
              relocated.set(false, prior) == 0,
          "previous exact owned location can be reverted after relocation");
    RegCloseKey(opened);
    RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
    check(integration::instanceClass() == integration::instanceClass() &&
              !integration::instanceClass().empty(),
          "instance identity is deterministic for the executable and user");
    auto fixtureClass = L"CleanTaskMan.PrivateRestoreFixture." + std::to_wstring(GetCurrentProcessId());
    fixtureRestoreMessage = integration::restoreMessage();
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = restoreFixture;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = fixtureClass.c_str();
    RegisterClassW(&windowClass);
    HWND restoreWindow = CreateWindowExW(0, fixtureClass.c_str(), L"Private restore fixture", WS_POPUP, 0, 0,
                                         1, 1, nullptr, nullptr, windowClass.hInstance, nullptr);
    check(restoreWindow && integration::restoreExisting(fixtureClass, fixtureRestoreMessage) &&
              restoreCalls == 1 && emptyRestorePayload && !IsWindowVisible(restoreWindow),
          "restore channel finds only its private fixture and sends an acknowledged empty payload");
    check(!integration::restoreExisting(fixtureClass + L".Missing", fixtureRestoreMessage),
          "absent resident is not reported as restored");
    DestroyWindow(restoreWindow);
    UnregisterClassW(fixtureClass.c_str(), windowClass.hInstance);

    HWND fixture = CreateWindowExW(0, L"STATIC", L"Private QA fixture", WS_POPUP, 0, 0, 1000, 700, nullptr,
                                   nullptr, GetModuleHandleW(nullptr), nullptr);
    check(fixture != nullptr && !IsWindowVisible(fixture), "private lifecycle fixture is never shown");
    {
        App app;
        app.window = fixture;
        app.loadPreferences(settings::parse("[Monitoring]\nUpdateIntervalMs=500\n[Window]\nCloseToTray=1\n["
                                            "Startup]\nRunAsAdministrator=0\nDefaultTab=1\n"));
        check(app.updateInterval == 500 && app.closeToTray && app.page == 1 && !app.runAdmin,
              "settings load selected cadence and close-to-tray toggle");
        app.capturePreferences();
        app.placementBaseline = app.currentPlacement();
        app.queuePlacementOnExit();
        check(GetFileAttributesW(settings::filePath().c_str()) == INVALID_FILE_ATTRIBUTES &&
                  GetFileAttributesW(settings::placementPath().c_str()) == INVALID_FILE_ATTRIBUTES,
              "unchanged placement and state capture write no files");
        app.activate(640);
        auto saved = settings::load(settings::filePath());
        check(!app.closeToTray && settings::integer(saved, "Window.CloseToTray", -1, 0, 1) == 0 &&
                  settings::integer(saved, "Monitoring.UpdateIntervalMs", 0, 0, 2000) == 500,
              "tray toggle immediately saves settings without a Save command");
        check(!saved.count("Window.Left") && !saved.count("Window.Width"),
              "general settings file does not capture window geometry");
        // Upgrade: preserve main-INI-only geometry through unrelated settings
        // changes when an unchanged close has no reason to create a sidecar.
        for (auto& [key, value] : settings::placementValues(app.currentPlacement(), ""))
            if (key != "Window.ConfigurationRevision" && key != "Window.Format")
                app.preferences[key] = value;
        app.persistedPreferences = app.preferences;
        app.activate(640);
        auto upgraded = settings::load(settings::filePath());
        check(settings::readPlacement(upgraded) == app.placementBaseline,
              "settings autosave retains untouched legacy INI placement during upgrade");
        app.placementQueued = false;
        app.queuePlacementOnExit();
        check(!app.placementQueued &&
                  GetFileAttributesW(settings::placementPath().c_str()) == INVALID_FILE_ATTRIBUTES,
              "unchanged legacy placement needs no exit write after a preference change");
        auto baseline = *app.placementBaseline;
        SetWindowPos(fixture, nullptr, 50, 60, 1020, 720, SWP_NOZORDER | SWP_NOACTIVATE);
        app.activate(620);
        check(settings::integer(settings::load(settings::filePath()), "Startup.RunAsAdministrator", -1, 0,
                                1) == 1 &&
                  *app.placementBaseline == baseline,
              "administrator preference saves immediately without changing placement baseline");
        auto mainBefore = settings::load(settings::filePath());
        app.queuePlacementOnExit();
        check(app.placementQueued && app.placementWriter.flush(),
              "changed placement queues a separate compact file");
        auto position = settings::load(settings::placementPath());
        auto loaded = mainBefore;
        check(settings::overlayPlacement(loaded, position) &&
                  settings::integer(loaded, "Window.Left", 0, 0, 1000) == 50 &&
                  settings::load(settings::filePath()) == mainBefore,
              "placement-only exit restores bounds and leaves general preferences untouched");
        app.activate(640);
        loaded = settings::load(settings::filePath());
        check(settings::overlayPlacement(loaded, position) &&
                  settings::integer(loaded, "Window.Left", 0, 0, 1000) == 50,
              "unrelated settings autosave cannot invalidate current placement");
        auto noMain = settings::Values{};
        check(settings::overlayPlacement(noMain, position),
              "new placement file also restores before any settings file has been created");
        auto corrupt = position;
        corrupt["Startup.RunAsAdministrator"] = "0";
        loaded = app.persistedPreferences;
        check(settings::overlayPlacement(loaded, corrupt) &&
                  settings::get(loaded, "Startup.RunAsAdministrator") ==
                      settings::get(app.persistedPreferences, "Startup.RunAsAdministrator"),
              "placement file cannot override non-placement settings");
        auto invalid = position;
        invalid["Window.Width"] = "0";
        check(!settings::overlayPlacement(loaded, invalid), "invalid placement is rejected");
        auto legacy = position;
        legacy.erase("Window.Format");
        legacy["Window.ConfigurationRevision"] = "old";
        loaded = app.persistedPreferences;
        loaded["Configuration.Revision"] = "new";
        check(!settings::overlayPlacement(loaded, legacy),
              "obsolete legacy placement keeps its revision guard");
        auto stamp = [](const std::wstring& path) {
            WIN32_FILE_ATTRIBUTE_DATA a{};
            GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a);
            return (uint64_t(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
        };
        app.placementQueued = false;
        app.placementBaseline = app.currentPlacement();
        auto oldStamp = stamp(settings::placementPath()), mainStamp = stamp(settings::filePath());
        app.queuePlacementOnExit();
        check(!app.placementQueued && stamp(settings::placementPath()) == oldStamp &&
                  stamp(settings::filePath()) == mainStamp,
              "unchanged subsequent exit writes neither file");
        check(app.persistPreferences() && stamp(settings::filePath()) == mainStamp,
              "unchanged settings snapshot performs no redundant write");
        app.placementQueued = false;
        SetWindowPos(fixture, nullptr, 90, 100, 1040, 740, SWP_NOZORDER | SWP_NOACTIVATE);
        app.handoffExit = true;
        app.queuePlacementOnExit();
        check(!app.placementQueued, "administrator handoff leaves placement for the new instance");
        app.handoffExit = false;
        // Queue both destinations before waiting: a compact exit write cannot
        // replace a general settings job that is already in progress.
        app.settingsWriter.queue(settings::filePath(), {{"Monitoring.UpdateIntervalMs", "1500"}});
        app.queuePlacementOnExit();
        check(app.settingsWriter.flush() && app.placementWriter.flush() &&
                  settings::integer(settings::load(settings::filePath()), "Monitoring.UpdateIntervalMs", 0, 0,
                                    2000) == 1500 &&
                  settings::integer(settings::load(settings::placementPath()), "Window.Left", 0, 0, 1000) ==
                      90,
              "independent writers preserve both preference and placement jobs");
        app.loadPreferences(settings::parse("[Monitoring]\nUpdateIntervalMs=700\n"));
        check(app.updateInterval == 1000 && !app.closeToTray && app.defaultTab == 0 && app.runAdmin,
              "invalid cadence resolves to defaults");
        app.taskManager =
            integration::TaskManager(L"C:\\Portable App\\CleanTaskMan.exe", HKEY_CURRENT_USER, key);
        check(app.changeReplacement(true) &&
                  app.taskManager.state().kind == integration::ReplacementKind::Owned,
              "replacement toggle immediately applies the isolated registry change");
        auto owned = settings::load(settings::filePath());
        check(settings::wide(settings::get(owned, "Integration.OwnedDebugger")) == app.taskManager.command(),
              "exact replacement ownership is saved immediately");
        app.closeToTray = true;
        app.updateInterval = 500;
        app.themeMode = 2;
        app.expanded.insert(L"example");
        app.applyDefaultSettings();
        check(!app.closeToTray && app.updateInterval == 1000 && app.themeMode == 0 && app.expanded.empty(),
              "reset applies preference defaults");
        check(app.taskManager.state().kind == integration::ReplacementKind::Default &&
                  app.ownedDebugger.empty() &&
                  settings::integer(settings::load(settings::filePath()), "Monitoring.UpdateIntervalMs", 0, 0,
                                    2000) == 1000,
              "reset immediately saves defaults and restores the app-owned replacement");
        RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());

        for (unsigned ms : {500u, 1000u, 1500u}) {
            telemetry::Collector c;
            c.setUpdateInterval(ms);
            c.start([] {});
            check(until([&] { return c.core()->revision >= 3; }),
                  "cadence collector reaches ordinary interval");
            auto a = c.core();
            check(until([&] { return c.core()->revision > a->revision; }, 2500),
                  "next ordinary CPU sample arrives");
            auto b = c.core();
            double elapsed = (b->timestamp - a->timestamp) * 1000;
            printf("CADENCE target_ms=%u actual_ms=%.2f\n", ms, elapsed);
            check(elapsed >= ms - 80 && elapsed <= ms + 300,
                  "ordinary cadence matches selected update speed");
            c.requestStop();
            c.join();
        }

        std::atomic<unsigned> notifications{0};
        app.collector->start([&] { notifications++; });
        app.collectorsStarted = true;
        check(until([&] { return app.collector->core()->revision >= 2; }), "fixture starts real collectors");
        app.core = app.collector->core();
        app.devices = app.collector->devices();
        app.pauseMetrics();
        check(app.metricsPaused && app.collectorStopping && !app.collectorsStarted,
              "minimize requests stop without waiting on UI thread");
        app.prepareResume();
        check(!app.metricsPaused && app.collectorStopping,
              "restore during stop remembers resume without a second collector");
        app.startCollectors();
        check(!app.collectorsStarted, "visible-paint/stop gates reject premature collector restart");
        app.finishCollectorStop();
        check(!app.collectorStopping && app.collector->startedAt() == 0,
              "old workers join before a pristine collector replaces them");
        check(!app.core->ready && std::isnan(app.core->cpu) && app.core->cpuHistory.empty(),
              "restore clears readings and histories for fresh baselines");
        auto count = notifications.load();
        Sleep(1700);
        check(notifications == count, "paused collector produces no continuing samples or notifications");
        app.collector->start([&] { notifications++; });
        app.collectorsStarted = true;
        check(until([&] { return app.collector->core()->revision >= 2; }),
              "fresh collector resumes and quickly produces measured CPU");
        app.pauseMetrics();
        app.finishCollectorStop();
        check(app.metricsPaused && !app.collectorsStarted,
              "a second minimize remains fully paused after shutdown");
        app.window = nullptr;
    }
    DestroyWindow(fixture);
    DeleteFileW(settings::filePath().c_str());
    DeleteFileW(settings::placementPath().c_str());
    CoUninitialize();
    printf("RESULT failures=%d; real HKLM Task Manager configuration unchanged\n", failures);
    return failures ? 1 : 0;
}

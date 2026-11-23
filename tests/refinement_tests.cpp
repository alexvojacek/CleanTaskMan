#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <set>
#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include "../src/telemetry.hpp"
#include "../src/settings.hpp"
#include "../src/process_actions.hpp"

int failures = 0;
void check(bool good, const char* text) {
    printf("%s %s\n", good ? "PASS" : "FAIL", text);
    fflush(stdout);
    if (!good)
        failures++;
}
uint64_t ticks(FILETIME t) {
    return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}
uint64_t birth(HANDLE p) {
    FILETIME b{}, e{}, k{}, u{};
    return GetProcessTimes(p, &b, &e, &k, &u) ? ticks(b) : 0;
}
struct Child {
    PROCESS_INFORMATION p{};
    bool start(const std::wstring& exe, const wchar_t* args) {
        auto cmd = L"\"" + exe + L"\" " + args;
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        return CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                              nullptr, &si, &p) != FALSE;
    }
    actions::Identity identity() {
        return {p.dwProcessId, birth(p.hProcess)};
    }
    ~Child() {
        if (p.hProcess) {
            if (WaitForSingleObject(p.hProcess, 0) == WAIT_TIMEOUT)
                TerminateProcess(p.hProcess, 0);
            WaitForSingleObject(p.hProcess, 2000);
            CloseHandle(p.hProcess);
            CloseHandle(p.hThread);
        }
    }
};
void traffic() {
    auto session = WinHttpOpen(L"CleanTaskMan-Native-QA/0.3", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return;
    WinHttpSetTimeouts(session, 3000, 3000, 3000, 3000);
    auto host = WinHttpConnect(session, L"learn.microsoft.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (host) {
        auto request =
            WinHttpOpenRequest(host, L"GET", L"/en-us/windows/win32/etw/event-tracing-portal", nullptr,
                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (request) {
            if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0,
                                   0) &&
                WinHttpReceiveResponse(request, nullptr)) {
                char data[16384];
                DWORD n = 0;
                size_t total = 0;
                while (total < 2 * 1024 * 1024 && WinHttpReadData(request, data, sizeof(data), &n) && n)
                    total += n;
            }
            WinHttpCloseHandle(request);
        }
        WinHttpCloseHandle(host);
    }
    WinHttpCloseHandle(session);
}
int workload(const std::wstring& folder) {
    auto file = folder + L"\\NativeQAHelper-" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    HANDLE h = CreateFileW(
        file.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_NO_BUFFERING | FILE_FLAG_DELETE_ON_CLOSE,
        nullptr);
    constexpr DWORD size = 1024 * 1024;
    auto bytes = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (bytes)
        memset(bytes, 'Q', size);
    std::thread net([] {
        for (int i = 0; i < 8; i++) {
            traffic();
            Sleep(250);
        }
    });
    uint64_t written = 0;
    DWORD error = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    for (int i = 0; i < 80; i++) {
        if (h != INVALID_HANDLE_VALUE && bytes) {
            LARGE_INTEGER offset{};
            offset.QuadPart = (i % 8) * size;
            SetFilePointerEx(h, offset, nullptr, FILE_BEGIN);
            DWORD n = 0;
            if (!WriteFile(h, bytes, size, &n, nullptr))
                error = GetLastError();
            written += n;
            FlushFileBuffers(h);
        }
        Sleep(200);
    }
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
    if (bytes)
        VirtualFree(bytes, 0, MEM_RELEASE);
    DeleteFileW(file.c_str());
    net.join();
    auto log = settings::executableFolder() + L"\\NativeQAHelper-" + std::to_wstring(GetCurrentProcessId()) +
               L".log";
    FILE* report = nullptr;
    _wfopen_s(&report, log.c_str(), L"w");
    if (report) {
        fprintf(report, "WORKLOAD pid=%lu tid=%lu written=%llu error=%lu\n", GetCurrentProcessId(),
                GetCurrentThreadId(), written, error);
        fclose(report);
    }
    Sleep(30000);
    return 0;
}
int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && std::wstring(argv[1]) == L"--idle") {
        Sleep(120000);
        return 0;
    }
    if (argc > 1 && std::wstring(argv[1]) == L"--work")
        return workload(argc > 2 ? argv[2] : settings::executableFolder());
    if (argc > 2 && std::wstring(argv[1]) == L"--report") {
        FILE* f = nullptr;
        _wfreopen_s(&f, argv[2], L"w", stdout);
        setvbuf(stdout, nullptr, _IONBF, 0);
    }
    printf("Native refinement validation; administrator=%s\n", activity::elevated() ? "yes" : "no");
    auto folder = settings::executableFolder();
    auto exe = folder + L"\\NativeQAHelper.exe";
    auto data = settings::parse(
        "[Startup]\nDefaultTab=1\nRunAsAdministrator=1\n[Window]\nLeft=-3200\nWidth=1180\nInvalid\n");
    check(settings::integer(data, "Window.Left", 0, -10000, 10000) == -3200,
          "signed placement parses correctly");
    check(settings::integer(settings::parse("[Window]\nWidth=999999999999999\n"), "Window.Width", 1180, 320,
                            10000) == 1180,
          "malformed and overflowing settings use defaults");
    data["Memory.Slot"] = settings::utf8(L"Controller0-ChannelA-DIMM1");
    check(settings::parse(settings::serialize(data)) == data, "single readable UTF-8 INI round trips");
    auto path = folder + L"\\QA-settings.ini";
    DWORD error = 0;
    check(settings::save(path, data, &error) && settings::load(path) == data,
          "configuration writes atomically and reloads");
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY);
    data["Window.Width"] = "1400";
    check(!settings::save(path, data, &error) && error != 0, "unwritable config reports a nonzero failure");
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(path.c_str());
    settings::Writer writer;
    writer.queue(path, data);
    data["Window.Width"] = "1500";
    writer.queue(path, data);
    check(writer.flush() && settings::load(path) == data,
          "asynchronous writer flush preserves latest preference");
    DeleteFileW(path.c_str());

    telemetry::Process a{}, b{}, c{};
    a.pid = 10;
    a.born = 100;
    a.name = L"Worker.exe";
    a.path = L"C:\\A\\Worker.exe";
    a.cpu = 2;
    a.memory = 10;
    a.disk = .1;
    a.network = .2;
    b = a;
    b.pid = 11;
    b.born = 200;
    b.path = L"c:\\a\\worker.EXE";
    b.cpu = 3;
    b.memory = 20;
    b.disk = telemetry::unknown;
    c = a;
    c.pid = 12;
    c.path = L"C:\\B\\Worker.exe";
    auto groups = telemetry::groupProcesses({a, b, c});
    check(groups.size() == 2 && groups[0].members.size() == 2,
          "duplicate executable paths group case-insensitively; different paths stay separate");
    check(groups[0].summary.cpu == 5 && groups[0].summary.memory == 30 && std::isnan(groups[0].summary.disk),
          "group totals sum real members and preserve unknown readings");
    check(groups[0].members[0].pid == 10 && groups[0].members[1].pid == 11,
          "group retains actual individual identities");

    Child child;
    check(child.start(exe, L"--idle"), "controlled helper starts");
    actions::Processes actions;
    auto id = child.identity();
    check(id.born != 0, "helper identity includes creation time");
    check(!actions.efficiency({id.pid, id.born + 1}, true).success,
          "stale process identity cannot change efficiency");
    check(!actions.end({id.pid, id.born + 1}).success &&
              WaitForSingleObject(child.p.hProcess, 0) == WAIT_TIMEOUT,
          "stale identity cannot end the replacement process");
    check(!actions.end({GetCurrentProcessId(), birth(GetCurrentProcess())}).success,
          "self termination is guarded");
    auto enabled = actions.efficiency(id, true);
    printf("Efficiency enable: success=%d error=%lu\n", enabled.success, enabled.error);
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    check(enabled.success && GetPriorityClass(child.p.hProcess) == IDLE_PRIORITY_CLASS &&
              GetProcessInformation(child.p.hProcess, ProcessPowerThrottling, &state, sizeof(state)) &&
              (state.StateMask & 1) && (state.ControlMask & 1),
          "efficiency applies both Idle priority and execution EcoQoS");
    check(actions.efficiency(id, false).success &&
              GetPriorityClass(child.p.hProcess) == NORMAL_PRIORITY_CLASS,
          "efficiency disable restores original priority");
    actions.efficiency(id, true);
    SetPriorityClass(child.p.hProcess, BELOW_NORMAL_PRIORITY_CLASS);
    check(actions.efficiency(id, false).success &&
              GetPriorityClass(child.p.hProcess) == BELOW_NORMAL_PRIORITY_CLASS,
          "disable preserves an external priority change");
    check(!actions::Processes::path(id).empty(), "fresh validated executable path is available");
    check(actions.end(id).success && WaitForSingleObject(child.p.hProcess, 0) == WAIT_OBJECT_0,
          "End task terminates only the controlled helper");

    telemetry::Collector live;
    live.start([] {});
    Sleep(3500);
    if (activity::elevated()) {
        std::wstring diskFolder = folder;
        for (int i = 1; i < argc; i++)
            if (std::wstring(argv[i]).rfind(L"--disk-folder=", 0) == 0)
                diskFolder = std::wstring(argv[i]).substr(14);
        auto arguments = L"--work \"" + diskFolder + L"\"";
        Child work;
        check(work.start(exe, arguments.c_str()),
              "disk/network workload helper starts after trace initialization");
        auto workId = work.identity();
        bool disk = false, network = false, healthy = false;
        uint64_t maxDisk = 0, maxNet = 0;
        DWORD traceError = 0;
        for (int i = 0; i < 45; i++) {
            Sleep(500);
            auto core = live.core();
            if (core->activity) {
                auto frame = core->activity;
                healthy = healthy || (frame->active && frame->valid);
                traceError = frame->error;
                auto item = frame->totals.find({workId.pid, workId.born});
                if (item != frame->totals.end()) {
                    maxDisk = std::max(maxDisk, item->second.diskRead + item->second.diskWrite);
                    maxNet = std::max(maxNet, item->second.networkReceive + item->second.networkSend);
                }
            }
            for (auto& p : core->processes)
                if (p.pid == workId.pid && p.born == workId.born) {
                    disk = disk || (std::isfinite(p.disk) && p.disk > 0);
                    network = network || (std::isfinite(p.network) && p.network > 0);
                }
            if (disk && network)
                break;
        }
        printf("ACTIVITY helper_pid=%lu bytes_disk=%llu bytes_network=%llu last_error=%lu\n", workId.pid,
               maxDisk, maxNet, traceError);
        check(healthy, "own native ETW session becomes healthy");
        check(disk && maxDisk > 0, "real physical disk completions attribute to the helper lifetime");
        check(network && maxNet > 0, "real network bytes attribute to helper payload PID");
        actions.end(workId);
    } else {
        check(live.core()->activity && !live.core()->activity->active,
              "standard session does not invent per-process activity");
        printf("SKIP privileged ETW tests; run this runner once via Windows UAC.\n");
    }
    auto d = live.devices();
    printf("METADATA logical=%zu GHz=%.3f RAM=%u/%u speed=%u\n", d->logicalCpu.size(), d->frequencyGHz,
           d->memoryHardware.used, d->memoryHardware.slots, d->memoryHardware.speed);
    bool logical = true;
    std::set<std::pair<unsigned, unsigned>> ids;
    for (auto& c : d->logicalCpu) {
        if (!ids.emplace(c.group, c.index).second || !std::isfinite(c.usage) || c.usage < 0 || c.usage > 100)
            logical = false;
    }
    check(logical && d->logicalCpu.size() == GetActiveProcessorCount(ALL_PROCESSOR_GROUPS),
          "logical counters have distinct stable group/index identities and valid per-core range");
    check(d->memoryHardware.valid && d->memoryHardware.used > 0 &&
              d->memoryHardware.slots >= d->memoryHardware.used && d->memoryHardware.speed > 0,
          "firmware reports occupied slots and configured memory speed");
    bool ip = false, driver = false;
    for (auto& v : d->devices) {
        if (v.kind == telemetry::Kind::Network && !v.ipv4.empty())
            ip = true;
        if (v.kind == telemetry::Kind::Gpu && !v.driverVersion.empty())
            driver = true;
    }
    check(ip, "network adapter has numeric IP address");
    check(driver, "physical GPU has installed driver version");
    live.requestStop();
    live.join();
    printf("RESULT failures=%d\n", failures);
    return failures ? 1 : 0;
}

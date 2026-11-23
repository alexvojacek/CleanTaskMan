#include "../src/telemetry.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <mutex>
#include <dxgi1_2.h>
#include <winioctl.h>
#include <cstring>

int failures = 0;
void check(bool ok, const char* message) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok)
        failures++;
}
bool close(double a, double b) {
    return std::abs(a - b) < 0.00001;
}
template <class Predicate> bool until(Predicate predicate, unsigned timeoutMs) {
    ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < timeoutMs) {
        if (predicate())
            return true;
        Sleep(20);
    }
    return false;
}
int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--child") {
        Sleep(10000);
        return 0;
    }
    check(close(telemetry::rate(100, 400, 1.5), 200), "byte rate uses elapsed time");
    check(std::isnan(telemetry::rate(400, 100, 1)),
          "counter reset is unavailable, not a negative or huge rate");
    check(std::isnan(telemetry::rate(100, 400, 0)), "zero interval is unavailable");
    check(close(telemetry::cpuPercent(0, 10000000, 1, 4), 25),
          "one fully busy core on four logical processors is 25 percent");
    check(std::isnan(telemetry::cpuPercent(20, 10, 1, 4)), "CPU counter reset is unavailable");
    check(std::isnan(telemetry::cpuPercent(0, 100, 1, 0)),
          "unknown processor count cannot produce a percentage");
    std::vector<double> sortValues{2, telemetry::unknown, 1, telemetry::unknown};
    std::stable_sort(sortValues.begin(), sortValues.end(),
                     [](double a, double b) { return telemetry::metricLess(a, b, true); });
    check(sortValues[0] == 1 && sortValues[1] == 2 && std::isnan(sortValues[2]) && std::isnan(sortValues[3]),
          "numeric sorting keeps unknown readings last");
    std::stable_sort(sortValues.begin(), sortValues.end(),
                     [](double a, double b) { return telemetry::metricLess(a, b, false); });
    check(sortValues[0] == 2 && sortValues[1] == 1 && std::isnan(sortValues[2]),
          "descending sorting also keeps unknown readings last");
    std::vector<telemetry::Engine> engines{{L"A", 0, 0, 101, 30},
                                           {L"A", 0, 0, 202, 40},
                                           {L"A", 0, 1, 101, 60},
                                           {L"B", 0, 0, 101, 99},
                                           {L"A", 0, 2, 202, telemetry::unknown}};
    check(close(telemetry::gpuUsage(engines, L"A"), 70),
          "GPU sums processes within one engine, then takes busiest engine");
    check(close(telemetry::gpuUsage(engines, L"B"), 99), "GPU adapter identity keeps engines separate");
    check(std::isnan(telemetry::gpuUsage(engines, L"C")), "missing GPU counters remain unavailable");
    engines.push_back({L"A", 0, 0, 303, 80});
    check(close(telemetry::gpuUsage(engines, L"A"), 100),
          "GPU clamps only after summing engine contributions");
    using E = telemetry::GpuEngineKind;
    check(telemetry::gpuEngineKind(L"pid_1_luid_0x00000000_0x00000001_phys_0_eng_1_engtype_3D") ==
                  E::ThreeD &&
              telemetry::gpuEngineKind(L"x_engtype_Copy") == E::Copy &&
              telemetry::gpuEngineKind(L"x_engtype_VideoEncode") == E::VideoEncode &&
              telemetry::gpuEngineKind(L"x_engtype_VideoDecode") == E::VideoDecode &&
              telemetry::gpuEngineKind(L"x_engtype_Compute_0") == E::Other &&
              telemetry::gpuEngineKind(L"x_engtype_VideoDecode_extra") == E::Other,
          "engine classification uses the complete type suffix, not partial display-name matches");
    std::vector<telemetry::Engine> typed{{L"A", 0, 0, 1, 20, E::ThreeD},    {L"A", 0, 0, 2, 30, E::ThreeD},
                                         {L"A", 0, 1, 1, 40, E::ThreeD},    {L"A", 1, 0, 1, 35, E::ThreeD},
                                         {L"A", 0, 2, 1, 88, E::Other},     {L"B", 0, 0, 1, 99, E::ThreeD},
                                         {L"A", 0, 3, 1, 0, E::VideoEncode}};
    check(close(telemetry::gpuEngineUsage(typed, L"A", E::ThreeD), 50) &&
              close(telemetry::gpuUsage(typed, L"A"), 88),
          "type summaries take busiest physical/node total while overall still includes Compute/Other");
    check(close(telemetry::gpuEngineUsage(typed, L"A", E::VideoEncode), 0) &&
              std::isnan(telemetry::gpuEngineUsage(typed, L"A", E::VideoDecode)),
          "measured idle engines remain zero and unavailable engines remain blank");
    telemetry::Device oldGpu;
    oldGpu.key = L"pci:1";
    oldGpu.adapter = L"A";
    oldGpu.history = {{1, .2}};
    oldGpu.gpuEngines[0].history = {{1, .3}};
    oldGpu.dedicatedHistory = {{1, 1024}};
    oldGpu.sharedHistory = {{1, 2048}};
    auto sameGpu = oldGpu;
    sameGpu.history.clear();
    sameGpu.gpuEngines[0].history.clear();
    sameGpu.dedicatedHistory.clear();
    sameGpu.sharedHistory.clear();
    telemetry::retainGpuHistory(sameGpu, oldGpu);
    check(sameGpu.history.size() == 1 && sameGpu.gpuEngines[0].history.size() == 1 &&
              sameGpu.dedicatedHistory.back().value == 1024 && sameGpu.sharedHistory.back().value == 2048,
          "same adapter retains independent engine and byte histories");
    telemetry::Device changedGpu;
    changedGpu.key = oldGpu.key;
    changedGpu.adapter = L"B";
    telemetry::retainGpuHistory(changedGpu, oldGpu);
    check(changedGpu.history.empty() && changedGpu.gpuEngines[0].history.empty() &&
              changedGpu.dedicatedHistory.empty() && changedGpu.sharedHistory.empty(),
          "adapter alias changes cannot splice histories from another identity");
    telemetry::StorageInventory storage;
    storage.volumes = {{L"Volume-A", {2, 2}, 1024}, {L"volume-a", {2}, 1024}, {L"Volume-B", {1}, 2048}};
    storage.windowsVolume = L"VOLUME-A";
    storage.pageFileVolumes = {L"Volume-B"};
    storage.pageFilesComplete = storage.volumesComplete = true;
    auto disk2 = telemetry::diskDetails(2, storage), disk1 = telemetry::diskDetails(1, storage);
    check(disk2.formattedCapacity == 1024 && disk1.formattedCapacity == 2048,
          "formatted size counts each canonical volume once, including multiple extents on one disk");
    check(disk2.systemDisk == true && disk1.systemDisk == false && disk1.pageFile == true &&
              disk2.pageFile == false,
          "Windows and pagefile roles follow mapped volume identity rather than disk or drive order");
    storage.volumesComplete = false;
    check(std::isnan(telemetry::diskDetails(2, storage).formattedCapacity) &&
              telemetry::diskDetails(2, storage).systemDisk == true &&
              telemetry::diskDetails(1, storage).pageFile == true,
          "incomplete volume enumeration or one unmapped volume cannot expose a partial formatted total; "
          "positive roles remain independent");
    storage.volumesComplete = true;
    storage.pageFilesComplete = false;
    check(!telemetry::diskDetails(2, storage).pageFile.has_value() &&
              telemetry::diskDetails(1, storage).pageFile == true,
          "failed pagefile inventory preserves known Yes and leaves unproven No blank");
    storage.pageFilesComplete = true;
    storage.pageFileVolumes.push_back(L"unresolved-volume");
    check(!telemetry::diskDetails(2, storage).pageFile.has_value(),
          "one unresolved pagefile volume prevents a false No");
    storage.pageFileVolumes.clear();
    check(telemetry::diskDetails(1, storage).pageFile == false,
          "successful enumeration of no active pagefiles yields No");
    storage.windowsVolume = L"unresolved-volume";
    check(!telemetry::diskDetails(1, storage).systemDisk.has_value(),
          "unknown Windows volume leaves System disk blank");
    storage.volumes.push_back({L"spanned-volume", {1, 2}, 4096});
    check(std::isnan(telemetry::diskDetails(1, storage).formattedCapacity) &&
              std::isnan(telemetry::diskDetails(2, storage).formattedCapacity),
          "multi-disk volumes cannot add their whole size to each disk");
    storage.volumes = {{L"locked-or-unformatted", {1}, telemetry::unknown}};
    check(std::isnan(telemetry::diskDetails(1, storage).formattedCapacity) &&
              std::isnan(telemetry::diskDetails(9, storage).formattedCapacity),
          "unreadable or missing filesystem sizes remain blank");
    check(telemetry::formattedBytes(1024, 8, 512) == 4194304 &&
              std::isnan(telemetry::formattedBytes(UINT64_MAX, 8, 512)) &&
              std::isnan(telemetry::formattedBytes(1, 0, 512)),
          "quota-independent allocation size checks geometry and integer overflow");
    std::vector<unsigned char> extentBytes(offsetof(VOLUME_DISK_EXTENTS, Extents) + 2 * sizeof(DISK_EXTENT));
    DWORD count = 2;
    memcpy(extentBytes.data(), &count, sizeof(count));
    DISK_EXTENT extent{};
    extent.DiskNumber = 2;
    extent.ExtentLength.QuadPart = 4096;
    memcpy(extentBytes.data() + offsetof(VOLUME_DISK_EXTENTS, Extents), &extent, sizeof(extent));
    extent.StartingOffset.QuadPart = 4096;
    memcpy(extentBytes.data() + offsetof(VOLUME_DISK_EXTENTS, Extents) + sizeof(extent), &extent,
           sizeof(extent));
    check(telemetry::volumeDisks(extentBytes) == std::vector<unsigned>{2},
          "extent parser deduplicates repeated physical-disk extents");
    extent.DiskNumber = 1;
    memcpy(extentBytes.data() + offsetof(VOLUME_DISK_EXTENTS, Extents) + sizeof(extent), &extent,
           sizeof(extent));
    check(telemetry::volumeDisks(extentBytes) == std::vector<unsigned>({1, 2}),
          "extent parser retains all physical identities for spanned volumes");
    auto malformed = extentBytes;
    malformed.pop_back();
    check(telemetry::volumeDisks(malformed).empty(), "extent parser rejects truncated output");
    extent.ExtentLength.QuadPart = -1;
    memcpy(extentBytes.data() + offsetof(VOLUME_DISK_EXTENTS, Extents), &extent, sizeof(extent));
    check(telemetry::volumeDisks(extentBytes).empty(),
          "extent parser rejects invalid negative extent lengths");

    {
        telemetry::Collector deferred;
        std::atomic<unsigned> notifications{0};
        check(!deferred.core()->ready && !deferred.devices()->ready && deferred.startedAt() == 0,
              "construction does not collect");
        deferred.start([&] { notifications++; }, 5000);
        Sleep(150);
        check(!deferred.core()->ready && !deferred.devices()->ready && notifications == 0 &&
                  deferred.startedAt() == 0,
              "delay applies to every collector before discovery");
        double before = telemetry::clockSeconds();
        deferred.requestStop();
        deferred.join();
        check(telemetry::clockSeconds() - before < 0.5, "closing during delayed startup cancels promptly");
    }
    telemetry::Collector live;
    std::atomic<unsigned> updates{0};
    std::mutex firstMutex;
    std::shared_ptr<const telemetry::CoreSnapshot> firstPublished;
    live.start([&] {
        updates++;
        auto c = live.core();
        if (c->ready) {
            std::lock_guard lock(firstMutex);
            if (!firstPublished)
                firstPublished = c;
        }
    });
    check(until([&] { return live.core()->ready; }, 10000), "native process/memory snapshot arrives");
    std::shared_ptr<const telemetry::CoreSnapshot> first;
    {
        std::lock_guard lock(firstMutex);
        first = firstPublished;
    }
    if (!first)
        first = live.core();
    check(std::isnan(first->cpu), "first CPU reading waits for a measured interval");
    check(first->memory.valid && first->memory.total > 0 && first->memory.available <= first->memory.total,
          "physical memory is coherent");
    MEMORYSTATUSEX native{};
    native.dwLength = sizeof(native);
    GlobalMemoryStatusEx(&native);
    check(first->memory.total == native.ullTotalPhys, "RAM total matches independent Windows memory query");
    auto self = std::find_if(first->processes.begin(), first->processes.end(),
                             [](const telemetry::Process& p) { return p.pid == GetCurrentProcessId(); });
    check(self != first->processes.end() && self->born != 0, "current process has creation-time identity");
    check(self != first->processes.end() && std::isfinite(self->memory) && self->memory > 0,
          "private working set is measured for current process");
    check(until([&] { return live.core()->revision >= 3; }, 5000), "periodic core updates continue");
    auto current = live.core();
    check(std::isfinite(current->cpu) && current->cpu >= 0 && current->cpu <= 100,
          "total CPU is finite and normalized");
    check(current->info.logical == GetActiveProcessorCount(ALL_PROCESSOR_GROUPS),
          "logical processor count matches Windows");
    bool processRates = true;
    for (auto& p : current->processes)
        if (std::isfinite(p.cpu) && (p.cpu < 0 || p.cpu > 100))
            processRates = false;
    check(processRates, "all measured process CPU rates are in range");

    wchar_t exe[32768];
    GetModuleFileNameW(nullptr, exe, 32768);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --child";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    bool created = CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                  nullptr, &si, &pi) != FALSE;
    check(created, "spawn controlled test child");
    if (created) {
        check(until(
                  [&] {
                      auto c = live.core();
                      return std::any_of(c->processes.begin(), c->processes.end(),
                                         [&](const telemetry::Process& p) {
                                             return p.pid == pi.dwProcessId && p.born != 0;
                                         });
                  },
                  4000),
              "new process appears with stable identity");
        TerminateProcess(pi.hProcess, 0);
        WaitForSingleObject(pi.hProcess, 2000);
        check(until(
                  [&] {
                      auto c = live.core();
                      return std::none_of(
                          c->processes.begin(), c->processes.end(),
                          [&](const telemetry::Process& p) { return p.pid == pi.dwProcessId; });
                  },
                  4000),
              "exited process disappears");
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    check(until([&] { return live.devices()->revision >= 3; }, 15000),
          "disk/network/GPU collector publishes independently");
    auto d = live.devices();
    unsigned disks = 0, networks = 0, gpus = 0, validGpu = 0, validDisk = 0;
    printf("METADATA clock_GHz=%.3f logical=%zu memoryslots=%u/%u memoryspeed=%u\n", d->frequencyGHz,
           d->logicalCpu.size(), d->memoryHardware.used, d->memoryHardware.slots, d->memoryHardware.speed);
    for (auto& m : d->memoryHardware.modules)
        wprintf(L"MEMORY %ls %ls bytes=%llu speed=%u\n", m.locator.c_str(), m.type.c_str(),
                static_cast<unsigned long long>(m.capacity), m.speed);
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        for (unsigned i = 0; i < 64; i++) {
            IDXGIAdapter1* a = nullptr;
            if (factory->EnumAdapters1(i, &a) != S_OK)
                break;
            DXGI_ADAPTER_DESC1 desc{};
            a->GetDesc1(&desc);
            wprintf(L"DXGI %u %ls flags=%u luid=%08x_%08x dedicated=%llu outputs=", i, desc.Description,
                    desc.Flags, static_cast<unsigned>(desc.AdapterLuid.HighPart), desc.AdapterLuid.LowPart,
                    static_cast<unsigned long long>(desc.DedicatedVideoMemory));
            unsigned outputs = 0;
            for (;;) {
                IDXGIOutput* o = nullptr;
                if (a->EnumOutputs(outputs, &o) != S_OK)
                    break;
                outputs++;
                o->Release();
            }
            printf("%u\n", outputs);
            a->Release();
        }
        factory->Release();
    }
    for (auto& v : d->devices) {
        if (v.kind == telemetry::Kind::Disk) {
            disks++;
            if (std::isfinite(v.usage) && std::isfinite(v.read) && std::isfinite(v.write))
                validDisk++;
            printf("STORAGE capacity=%llu formatted=%.0f system=%d pagefile=%d\n",
                   static_cast<unsigned long long>(v.capacity), v.storage.formattedCapacity,
                   v.storage.systemDisk.has_value() ? (*v.storage.systemDisk ? 1 : 0) : -1,
                   v.storage.pageFile.has_value() ? (*v.storage.pageFile ? 1 : 0) : -1);
            wprintf(L"STORAGE_TYPE %ls\n", v.type.c_str());
            check(std::isnan(v.storage.formattedCapacity) ||
                      (v.storage.formattedCapacity >= 0 &&
                       (!v.capacity || v.storage.formattedCapacity <= v.capacity)),
                  "measured formatted capacity stays within the physical device");
        }
        if (v.kind == telemetry::Kind::Network)
            networks++;
        if (v.kind == telemetry::Kind::Gpu) {
            gpus++;
            if (std::isfinite(v.usage))
                validGpu++;
        }
        check(v.history.size() <= 65, "device history is bounded");
        if (v.kind != telemetry::Kind::Network && std::isfinite(v.usage))
            check(v.usage >= 0 && v.usage <= 100, "device utilization is normalized");
        wprintf(
            L"DEVICE %ls | %ls | %ls | flags %u | usage %.3f | read %.0f | write %.0f | temperature %.1f\n",
            v.key.c_str(), v.name.c_str(), v.description.c_str(), v.adapterFlags, v.usage, v.read, v.write,
            v.temperature);
        if (v.kind == telemetry::Kind::Gpu) {
            wprintf(L"DRIVER %ls | DATE %ls | LOCATION %ls\n", v.driverVersion.c_str(), v.driverDate.c_str(),
                    v.physicalLocation.c_str());
            printf("GPU_GRAPHS 3D=%.3f Copy=%.3f Encode=%.3f Decode=%.3f dedicated=%.0f shared=%.0f "
                   "histories=%zu/%zu/%zu/%zu/%zu/%zu\n",
                   v.gpuEngines[0].usage, v.gpuEngines[1].usage, v.gpuEngines[2].usage, v.gpuEngines[3].usage,
                   v.dedicatedUsed, v.sharedUsed, v.gpuEngines[0].history.size(),
                   v.gpuEngines[1].history.size(), v.gpuEngines[2].history.size(),
                   v.gpuEngines[3].history.size(), v.dedicatedHistory.size(), v.sharedHistory.size());
            bool series = true;
            for (auto& engine : v.gpuEngines)
                series = series && !engine.history.empty() && engine.history.size() <= 125 &&
                         (std::isnan(engine.usage) || (engine.usage >= 0 && engine.usage <= 100));
            check(series && v.dedicatedHistory.size() == v.sharedHistory.size(),
                  "real GPU publishes independent bounded graph histories");
        }
        if (v.kind == telemetry::Kind::Network)
            wprintf(L"IP %ls %ls\n", v.ipv4.c_str(), v.ipv6.c_str());
    }
    printf("OBSERVED processes=%zu disks=%u validDisks=%u networks=%u GPUs=%u validGPUs=%u services=%zu "
           "users=%zu startup=%zu core_ms=%.3f devices_ms=%.3f updates=%u\n",
           current->processes.size(), disks, validDisk, networks, gpus, validGpu, d->services.size(),
           d->users.size(), d->startup.size(), current->collectionMs, d->collectionMs, updates.load());
    check(!d->services.empty(), "native service inventory arrives");
    double before = telemetry::clockSeconds();
    live.requestStop();
    live.join();
    check(telemetry::clockSeconds() - before < 2.5, "normal collection shutdown completes promptly");
    printf("RESULT %d failures\n", failures);
    return failures ? 1 : 0;
}

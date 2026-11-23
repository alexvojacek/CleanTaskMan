#include "../src/telemetry.hpp"
#include "../src/process_disk.hpp"
#include "../src/cpu_parking.hpp"
#include "../src/activity.hpp"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <thread>
#include <atomic>
#include <algorithm>

namespace {
int failures = 0;
void check(bool ok, const char* message) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", message);
    fflush(stdout);
    if (!ok)
        failures++;
}
template <class T> void put(std::vector<unsigned char>& b, size_t at, T value) {
    memcpy(b.data() + at, &value, sizeof(value));
}
std::vector<unsigned char> record(DWORD pid, uint64_t born, unsigned threads = 0, size_t padding = 0) {
    std::vector<unsigned char> b(256 + 136 * threads + 40 + padding);
    put(b, 4, threads);
    put(b, 32, born);
    put(b, 80, uint64_t(pid));
    put(b, 256 + 136 * threads, process_disk::Counters{1024, 2048, 1, 2, 0});
    return b;
}
void fixtures() {
    auto a = record(10, 100, 1, 2), b = record(20, 200);
    put(a, 0, DWORD(a.size()));
    a.insert(a.end(), b.begin(), b.end());
    auto parsed = process_disk::parse(a);
    check(parsed.valid && parsed.records.size() == 2 && parsed.records.at(20).counters.write == 2048,
          "full-process disk parser accepts byte offsets without natural alignment");
    auto bad = a;
    put(bad, 4, 0xffffffffu);
    check(!process_disk::parse(bad).valid, "disk parser rejects overflowing thread counts");
    bad = a;
    put(bad, 0, 8u);
    check(!process_disk::parse(bad).valid, "disk parser rejects nonprogressing and truncated records");
    bad = a;
    put(bad, 80, uint64_t(20));
    check(!process_disk::parse(bad).valid, "disk parser rejects duplicate PIDs");
    bad = record(30, 300);
    bad.resize(295);
    check(!process_disk::parse(bad).valid, "disk parser rejects truncated counter extension");
    check(process_disk::parse(record(30, 0)).valid && process_disk::parse(record(30, 0)).records.empty(),
          "unidentified protected record cannot invent a process identity");
    process_disk::Snapshot before, after;
    before.valid = after.valid = true;
    before.timestamp = 1;
    after.timestamp = 1.5;
    before.records[10] = {100, {0, 0, 0, 0, 0}};
    after.records[10] = {100, {1048576, 1048576, 0, 0, 0}};
    check(process_disk::megabytesPerSecond(before, after, 10, 100) == 4,
          "process disk rate uses real elapsed time and both directions");
    check(std::isnan(process_disk::megabytesPerSecond(before, after, 10, 101)),
          "disk rate rejects mismatched process creation identity");
    after.records[10].born = 101;
    check(std::isnan(process_disk::megabytesPerSecond(before, after, 10, 0)),
          "disk baseline cannot cross PID reuse");
    after.records[10].born = 100;
    before.valid = false;
    check(std::isnan(process_disk::megabytesPerSecond(before, after, 10, 100)),
          "source failure requires a fresh native disk baseline");
    before.valid = true;
    before.records[10].counters.write = 2097152;
    check(std::isnan(process_disk::megabytesPerSecond(before, after, 10, 100)),
          "counter resets remain empty rather than huge or negative");
    SYSTEM_CPU_SET_INFORMATION cpu{};
    cpu.Size = sizeof(cpu);
    cpu.Type = CpuSetInformation;
    cpu.CpuSet.Group = 1;
    cpu.CpuSet.LogicalProcessorIndex = 3;
    cpu.CpuSet.Parked = 1;
    std::vector<unsigned char> cpus(sizeof(cpu));
    memcpy(cpus.data(), &cpu, sizeof(cpu));
    auto states = cpu_parking::parse(cpus);
    check(states.size() == 1 && states.at({1, 3}),
          "parking comes from the OS Parked flag with group/index identity");
    cpu.CpuSet.Group = 0;
    cpu.CpuSet.Parked = 0;
    auto offset = cpus.size();
    cpus.resize(offset + sizeof(cpu));
    memcpy(cpus.data() + offset, &cpu, sizeof(cpu));
    states = cpu_parking::parse(cpus);
    check(states.size() == 2 && !states.at({0, 3}) && states.at({1, 3}),
          "same logical index in different processor groups remains distinct");
    cpus.pop_back();
    check(cpu_parking::parse(cpus).empty(), "truncated parking results discard all partial states");
}
uint64_t diskBytes(const process_disk::Snapshot& a, const process_disk::Snapshot& b) {
    auto x = a.records.find(GetCurrentProcessId()), y = b.records.find(GetCurrentProcessId());
    if (!a.valid || !b.valid || x == a.records.end() || y == b.records.end() ||
        x->second.born != y->second.born)
        return UINT64_MAX;
    if (y->second.counters.read < x->second.counters.read ||
        y->second.counters.write < x->second.counters.write)
        return UINT64_MAX;
    return y->second.counters.read - x->second.counters.read + y->second.counters.write -
           x->second.counters.write;
}
void live(const std::wstring& root) {
    process_disk::Sampler sampler;
    auto warm = sampler.capture();
    check(warm.valid, "administrator native process-accounted disk source is valid");
    if (!warm.valid) {
        printf("NATIVE_STATUS %lu\n", warm.status);
        return;
    }
    telemetry::Collector collector;
    collector.setUpdateInterval(500);
    collector.start([] {});
    auto start = GetTickCount64();
    while (collector.core()->revision < 3 && GetTickCount64() - start < 10000)
        Sleep(10);
    std::wstring source = root + L"\\sensor-source-" + std::to_wstring(GetCurrentProcessId()) + L".tmp",
                 destination = source + L".copy";
    std::vector<unsigned char> buffer(8 * 1024 * 1024, 0x39);
    DWORD written = 0;
    HANDLE file = CreateFileW(source.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_TEMPORARY, nullptr);
    bool created = file != INVALID_HANDLE_VALUE;
    if (created) {
        created = WriteFile(file, buffer.data(), DWORD(buffer.size()), &written, nullptr) &&
                  written == buffer.size();
        CloseHandle(file);
    }
    auto before = sampler.capture();
    double peak = 0;
    bool copied = created;
    for (int i = 0; i < 32 && copied; i++) {
        copied = CopyFileW(source.c_str(), destination.c_str(), FALSE) != FALSE;
        Sleep(65);
        auto snapshot = collector.core();
        for (auto& p : snapshot->processes)
            if (p.pid == GetCurrentProcessId() && std::isfinite(p.disk))
                peak = std::max(peak, p.disk);
    }
    auto after = sampler.capture();
    auto delta = diskBytes(before, after);
    DeleteFileW(destination.c_str());
    DeleteFileW(source.c_str());
    collector.requestStop();
    auto closing = telemetry::clockSeconds();
    collector.join();
    double closeMs = (telemetry::clockSeconds() - closing) * 1000;
    printf("COPY drive=%c native_bytes=%llu displayed_peak_MBps=%.3f collector_join_ms=%.2f\n",
           static_cast<char>(root[0]), static_cast<unsigned long long>(delta), peak, closeMs);
    check(copied && delta >= 8 * 1024 * 1024 && delta != UINT64_MAX,
          "bounded file copies produce native process disk bytes");
    check(peak > 0, "production Processes snapshot reports nonzero disk rate for the copying process");
}
void pipes() {
    process_disk::Sampler sampler;
    sampler.capture();
    HANDLE read = nullptr, write = nullptr;
    check(CreatePipe(&read, &write, nullptr, 65536) != FALSE, "private pipe fixture opens");
    if (!read || !write)
        return;
    std::vector<unsigned char> block(65536, 1), output(65536);
    std::atomic<bool> good{true};
    // Warm allocations, thread runtime and query code before measurement.
    std::thread warm([] {});
    warm.join();
    auto before = sampler.capture();
    IO_COUNTERS ioBefore{}, ioAfter{};
    GetProcessIoCounters(GetCurrentProcess(), &ioBefore);
    std::thread sender([&] {
        for (unsigned i = 0; i < 512; i++) {
            DWORD n = 0;
            if (!WriteFile(write, block.data(), DWORD(block.size()), &n, nullptr) || n != block.size()) {
                good = false;
                break;
            }
        }
        CloseHandle(write);
    });
    uint64_t received = 0;
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(read, output.data(), DWORD(output.size()), &n, nullptr) || !n)
            break;
        received += n;
    }
    sender.join();
    CloseHandle(read);
    GetProcessIoCounters(GetCurrentProcess(), &ioAfter);
    auto after = sampler.capture();
    auto delta = diskBytes(before, after);
    printf("PIPE payload=%llu generic_IO=%llu native_disk=%llu\n", static_cast<unsigned long long>(received),
           static_cast<unsigned long long>(ioAfter.ReadTransferCount + ioAfter.WriteTransferCount -
                                           ioBefore.ReadTransferCount - ioBefore.WriteTransferCount),
           static_cast<unsigned long long>(delta));
    check(good && received == 32 * 1024 * 1024 && delta < 1024 * 1024,
          "pipe traffic is excluded from disk activity (allows incidental sub-MB OS faults)");
}
void coverage() {
    telemetry::Collector collector;
    collector.setUpdateInterval(500);
    collector.start([] {});
    auto start = GetTickCount64();
    while (collector.core()->revision < 5 && GetTickCount64() - start < 10000)
        Sleep(10);
    auto snapshot = collector.core();
    unsigned cpu = 0, memory = 0, disk = 0, network = 0;
    for (auto& p : snapshot->processes) {
        if (std::isfinite(p.cpu))
            cpu++;
        if (std::isfinite(p.memory))
            memory++;
        if (std::isfinite(p.disk))
            disk++;
        if (std::isfinite(p.network))
            network++;
        if (p.name == L"dwm.exe" || p.name == L"spoolsv.exe" || p.name == L"SearchIndexer.exe" ||
            p.name == L"gamingservices.exe")
            wprintf(L"PROCESS %ls pid=%lu cpu=%.2f memory_MB=%.2f disk_MBps=%.3f network_Mbps=%.3f\n",
                    p.name.c_str(), p.pid, p.cpu, p.memory, p.disk, p.network);
    }
    printf("COVERAGE elevated=%d total=%zu CPU=%u memory=%u disk=%u network=%u native_disk_valid=%d "
           "ETW_active=%d ETW_error=%lu\n",
           activity::elevated(), snapshot->processes.size(), cpu, memory, disk, network,
           snapshot->processDisk && snapshot->processDisk->valid,
           snapshot->activity && snapshot->activity->active,
           snapshot->activity ? snapshot->activity->error : 0);
    collector.requestStop();
    collector.join();
    fflush(stdout);
}
} // namespace
int main(int argc, char** argv) {
    if (argc > 1)
        freopen(argv[1], "w", stdout);
    if (argc > 2 && std::string(argv[2]) == "--coverage-only") {
        coverage();
        return 0;
    }
    fixtures();
    cpu_parking::Sampler parking;
    auto states = parking.capture();
    unsigned parked = 0;
    for (auto& [key, value] : states)
        if (value)
            parked++;
    printf("PARKING known=%zu parked=%u active=%lu\n", states.size(), parked,
           GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    check(states.size() == GetActiveProcessorCount(ALL_PROCESSOR_GROUPS),
          "native parking inventory covers every active logical CPU");
    wchar_t exe[32768];
    GetModuleFileNameW(nullptr, exe, 32768);
    std::wstring root = exe;
    root.resize(root.find_last_of(L"\\"));
    live(root);
    wchar_t extraVolume[32768];
    const DWORD extraLength = GetEnvironmentVariableW(L"CLEANTASKMAN_TEST_VOLUME", extraVolume, 32768);
    if (extraLength != 0 && extraLength < 32768)
        live(extraVolume);
    pipes();
    printf("RESULT failures=%d\n", failures);
    return failures ? 1 : 0;
}

#include "process_disk.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <cmath>
#ifdef PROCESS_DISK_DIAGNOSTICS
#include <cstdio>
#endif
namespace process_disk {
namespace {
constexpr size_t processHeader = 256, extendedThread = 136;
// Minimal private x64 ABI prefix. Microsoft's public SDK intentionally hides
// these fields; do not depend on MinGW's fuller winternl.h declaration.
struct ProcessPrefix {
    uint32_t next, threadCount;
    uint64_t reserved0[3];
    int64_t created;
    uint64_t reserved1[5];
    uintptr_t pid;
    unsigned char remaining[168];
};
static_assert(sizeof(void*) == 8);
static_assert(sizeof(ProcessPrefix) == processHeader);
static_assert(offsetof(ProcessPrefix, created) == 32);
static_assert(offsetof(ProcessPrefix, pid) == 80);
static_assert(sizeof(Counters) == 40);
template <class T> T read(const unsigned char* p) {
    T v{};
    memcpy(&v, p, sizeof(v));
    return v;
}
template <class T> T resolve(HMODULE module, const char* name) {
    auto address = GetProcAddress(module, name);
    T function{};
    static_assert(sizeof(function) == sizeof(address));
    memcpy(&function, &address, sizeof(function));
    return function;
}
double clockTime() {
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return double(now.QuadPart) / frequency.QuadPart;
}
} // namespace
Snapshot parse(std::span<const unsigned char> data) {
    Snapshot out;
    out.status = ERROR_INVALID_DATA;
    size_t at = 0;
    while (at < data.size()) {
        if (data.size() - at < processHeader)
            return out;
        auto* p = data.data() + at;
        auto next = read<ULONG>(p), threads = read<ULONG>(p + 4);
        size_t extent = next ? next : data.size() - at;
#ifdef PROCESS_DISK_DIAGNOSTICS
        printf("HEADER offset=%zu returned=%zu next=%lu threads=%lu pid=%llu\n", at, data.size(), next,
               threads, read<uint64_t>(p + 80));
#endif
        // NextEntryOffset is a byte offset, without guaranteed natural
        // alignment. All field reads use memcpy.
        if (extent > data.size() - at || extent < processHeader)
            return out;
        if (threads > (extent - processHeader) / extendedThread)
            return out;
        size_t extension = processHeader + size_t(threads) * extendedThread;
        if (extent - extension < sizeof(Counters))
            return out;
        auto pid = read<uint64_t>(p + 80);
        auto born = read<int64_t>(p + 32);
#ifdef PROCESS_DISK_DIAGNOSTICS
        printf("PARSE offset=%zu extent=%zu threads=%lu pid=%llu born=%lld extension=%zu spare=%zu\n", at,
               extent, threads, pid, born, extension, extent - extension);
#endif
        if (pid > MAXDWORD || born < 0)
            return out;
        if (pid) {
            Record record{static_cast<uint64_t>(born), read<Counters>(p + extension)};
            if (!record.born && pid > 4) {
                if (!next) {
                    out.valid = true;
                    out.status = 0;
                    return out;
                }
                at += next;
                continue;
            }
            if (!out.records.emplace(static_cast<DWORD>(pid), record).second)
                return out;
        }
        if (!next) {
            out.valid = true;
            out.status = 0;
            return out;
        }
        at += next;
    }
    return out;
}
double megabytesPerSecond(const Snapshot& before, const Snapshot& after, DWORD pid, uint64_t born) {
    const double missing = std::numeric_limits<double>::quiet_NaN();
    double seconds = after.timestamp - before.timestamp;
    if (!before.valid || !after.valid || !std::isfinite(seconds) || seconds <= 0)
        return missing;
    auto a = before.records.find(pid), b = after.records.find(pid);
    if (a == before.records.end() || b == after.records.end() || a->second.born != b->second.born ||
        (born && born != b->second.born))
        return missing;
    auto& x = a->second.counters;
    auto& y = b->second.counters;
    if (y.read < x.read || y.write < x.write)
        return missing;
    return (double(y.read - x.read) + double(y.write - x.write)) / seconds / 1048576.0;
}
struct Sampler::Impl {
    using Query = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
    Query query = nullptr;
    bool initialized = false, disabled = false;
    DWORD failure = 0;
    std::vector<unsigned char> buffer;
    void initialize() {
        initialized = true;
        auto module = GetModuleHandleW(L"ntdll.dll");
        auto version = resolve<LONG(NTAPI*)(OSVERSIONINFOW*)>(module, "RtlGetVersion");
        OSVERSIONINFOW info{};
        info.dwOSVersionInfoSize = sizeof(info);
        if (!version || version(&info) < 0 || info.dwMajorVersion != 10 || info.dwBuildNumber < 22000 ||
            info.dwBuildNumber > 26300) {
            disabled = true;
            failure = ERROR_NOT_SUPPORTED;
            return;
        }
        query = resolve<Query>(module, "NtQuerySystemInformation");
        if (!query) {
            disabled = true;
            failure = ERROR_PROC_NOT_FOUND;
            return;
        }
        buffer.resize(256 * 1024);
    }
};
Sampler::Sampler() : impl(std::make_unique<Impl>()) {}
Sampler::~Sampler() = default;
Snapshot Sampler::capture() {
    if (!impl->initialized)
        impl->initialize();
    Snapshot out;
    out.status = impl->failure;
    if (impl->disabled)
        return out;
    constexpr size_t limit = 64 * 1024 * 1024;
    for (unsigned attempt = 0; attempt < 6; attempt++) {
        ULONG returned = 0;
        LONG status =
            impl->query(148, impl->buffer.data(), static_cast<ULONG>(impl->buffer.size()), &returned);
        if (static_cast<ULONG>(status) == 0xc0000004u || static_cast<ULONG>(status) == 0xc0000023u) {
            size_t required = std::max<size_t>(returned, size_t(impl->buffer.size()) * 2);
            if (required > limit - 65536) {
                out.status = ERROR_NOT_ENOUGH_MEMORY;
                return out;
            }
            impl->buffer.resize(required + 65536);
            continue;
        }
        if (status < 0) {
            out.status = static_cast<DWORD>(status);
            if (out.status == 0xc0000022u || out.status == 0xc0000003u || out.status == 0xc0000002u) {
                impl->disabled = true;
                impl->failure = out.status;
            }
            return out;
        }
        if (!returned || returned > impl->buffer.size()) {
            out.status = ERROR_INVALID_DATA;
            return out;
        }
        out = parse(std::span(impl->buffer.data(), returned));
        out.timestamp = clockTime();
        if (out.valid) {
            FILETIME b{}, e{}, k{}, u{};
            auto self = out.records.find(GetCurrentProcessId());
            if (!GetProcessTimes(GetCurrentProcess(), &b, &e, &k, &u) || self == out.records.end() ||
                self->second.born != ((uint64_t(b.dwHighDateTime) << 32) | b.dwLowDateTime)) {
                out.valid = false;
                out.status = ERROR_INVALID_DATA;
                out.records.clear();
            }
        }
        if (!out.valid) {
            impl->disabled = true;
            impl->failure = out.status;
            out.records.clear();
        }
        return out;
    }
    out.status = ERROR_RETRY;
    return out;
}
} // namespace process_disk

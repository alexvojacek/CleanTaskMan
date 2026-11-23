#include "../src/process_disk.hpp"
#include <tlhelp32.h>
#include <cstdio>
#include <string>
#include <vector>
bool taskmgr() {
    HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    bool found = false;
    if (Process32FirstW(h, &e))
        do {
            if (_wcsicmp(e.szExeFile, L"taskmgr.exe") == 0)
                found = true;
        } while (Process32NextW(h, &e));
    CloseHandle(h);
    return found;
}
int wmain(int argc, wchar_t** argv) {
    if (argc > 1) {
        FILE* f = nullptr;
        _wfreopen_s(&f, argv[1], L"w", stdout);
        setvbuf(stdout, nullptr, _IONBF, 0);
    }
    process_disk::Sampler sampler;
    auto initial = sampler.capture();
    printf("QUERY valid=%d status=0x%08lx records=%zu taskmgr=%d\n", initial.valid, initial.status,
           initial.records.size(), taskmgr());
    if (!initial.valid)
        return 1;
    auto path = std::wstring(argc > 2 ? argv[2] : L".") + L"\\disk-probe-" +
                std::to_wstring(GetCurrentProcessId()) + L".tmp";
    HANDLE file =
        CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        printf("FILE error=%lu\n", GetLastError());
        return 2;
    }
    std::vector<char> buffer(1024 * 1024, 'Q');
    uint64_t written = 0;
    for (int i = 0; i < 16; i++) {
        DWORD n = 0;
        if (WriteFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &n, nullptr))
            written += n;
        FlushFileBuffers(file);
        Sleep(100);
        auto next = sampler.capture();
        auto before = initial.records.find(GetCurrentProcessId()),
             after = next.records.find(GetCurrentProcessId());
        if (next.valid && before != initial.records.end() && after != next.records.end())
            printf("SAMPLE bytes_written=%llu native_read_delta=%llu native_write_delta=%llu query_ms=%.3f "
                   "taskmgr=%d\n",
                   written, after->second.counters.read - before->second.counters.read,
                   after->second.counters.write - before->second.counters.write,
                   (next.timestamp - initial.timestamp) * 1000, taskmgr());
    }
    CloseHandle(file);
    return 0;
}

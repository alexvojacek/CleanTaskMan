#include <winsock2.h>
#include <ws2ipdef.h>
#include <ws2tcpip.h>
#include "telemetry.hpp"
#include "cpu_parking.hpp"
#include <tlhelp32.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <dxgi1_2.h>
#include <wtsapi32.h>
#include <winioctl.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <bit>
#include <setupapi.h>
#include <devpkey.h>
#include <cstring>
#ifdef CPU_STARTUP_DIAGNOSTICS
#include <cstdio>
#endif

namespace telemetry {
namespace {
constexpr unsigned startupCpuIntervalMs = 100;
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
uint64_t ticks(FILETIME t) {
    return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}
std::wstring lower(std::wstring s) {
    for (auto& c : s)
        c = static_cast<wchar_t>(towlower(c));
    return s;
}
bool good(double d) {
    return std::isfinite(d) && d >= 0;
}
constexpr GUID displayClass{0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
// DEVPKEY_Device_DriverDate, defined here because the portable toolchain's
// import libraries do not provide the header's external property-key symbol.
constexpr DEVPROPKEY driverDateKey{
    {0xa8b865dd, 0x2e3d, 0x4094, {0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6}}, 2};
struct DriverInfo {
    std::wstring version, date;
};
std::map<std::wstring, DriverInfo> driverVersions() {
    std::map<std::wstring, DriverInfo> out;
    HDEVINFO set = SetupDiGetClassDevsW(&displayClass, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
        return out;
    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(data);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &data); i++) {
        DWORD bus = 0, address = 0, got = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(set, &data, SPDRP_BUSNUMBER, nullptr,
                                               reinterpret_cast<BYTE*>(&bus), sizeof(bus), &got) ||
            !SetupDiGetDeviceRegistryPropertyW(set, &data, SPDRP_ADDRESS, nullptr,
                                               reinterpret_cast<BYTE*>(&address), sizeof(address), &got))
            continue;
        DriverInfo info;
        DEVPROPTYPE propertyType = 0;
        FILETIME date{};
        DWORD required = 0;
        if (SetupDiGetDevicePropertyW(set, &data, &driverDateKey, &propertyType,
                                      reinterpret_cast<BYTE*>(&date), sizeof(date), &required, 0) &&
            propertyType == DEVPROP_TYPE_FILETIME && required == sizeof(date) && ticks(date)) {
            SYSTEMTIME calendar{};
            wchar_t formatted[32]{};
            if (FileTimeToSystemTime(&date, &calendar)) {
                swprintf(formatted, 32, L"%04u-%02u-%02u", calendar.wYear, calendar.wMonth, calendar.wDay);
                info.date = formatted;
            }
        }
        HKEY key = SetupDiOpenDevRegKey(set, &data, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
        wchar_t version[256]{};
        DWORD bytes = sizeof(version);
        if (key != INVALID_HANDLE_VALUE) {
            if (RegGetValueW(key, nullptr, L"DriverVersion", RRF_RT_REG_SZ, nullptr, version, &bytes) ==
                ERROR_SUCCESS)
                info.version = version;
            RegCloseKey(key);
        }
        out[std::to_wstring(bus) + L":" + std::to_wstring(address >> 16) + L":" +
            std::to_wstring(address & 0xffff)] = std::move(info);
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}
struct Addresses {
    std::wstring v4, v6;
};
std::map<uint64_t, Addresses> adapterAddresses() {
    std::map<uint64_t, Addresses> out;
    ULONG bytes = 16384;
    for (int attempt = 0; attempt < 3; attempt++) {
        std::vector<unsigned char> b(bytes);
        auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(b.data());
        ULONG result = GetAdaptersAddresses(
            AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, a,
            &bytes);
        if (result == ERROR_BUFFER_OVERFLOW && bytes < 8 * 1024 * 1024)
            continue;
        if (result != NO_ERROR)
            return out;
        for (; a; a = a->Next) {
            Addresses addresses;
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                wchar_t text[128]{};
                auto* socket = u->Address.lpSockaddr;
                if (!socket)
                    continue;
                if (socket->sa_family == AF_INET) {
                    auto* v = reinterpret_cast<sockaddr_in*>(socket);
                    if (InetNtopW(AF_INET, &v->sin_addr, text, 128)) {
                        if (!addresses.v4.empty())
                            addresses.v4 += L", ";
                        addresses.v4 += text;
                    }
                } else if (socket->sa_family == AF_INET6) {
                    auto* v = reinterpret_cast<sockaddr_in6*>(socket);
                    if (InetNtopW(AF_INET6, &v->sin6_addr, text, 128)) {
                        if (!addresses.v6.empty())
                            addresses.v6 += L", ";
                        addresses.v6 += text;
                        if (v->sin6_scope_id)
                            addresses.v6 += L"%" + std::to_wstring(v->sin6_scope_id);
                    }
                }
            }
            out[a->Luid.Value] = std::move(addresses);
        }
        return out;
    }
    return out;
}
MemoryHardware memoryFirmware() {
    constexpr DWORD provider = 0x52534d42;
    DWORD bytes = GetSystemFirmwareTable(provider, 0, nullptr, 0);
    if (bytes < 8 || bytes > 16 * 1024 * 1024)
        return {};
    std::vector<unsigned char> b(bytes);
    DWORD actual = GetSystemFirmwareTable(provider, 0, b.data(), bytes);
    if (!actual || actual > bytes)
        return {};
    b.resize(actual);
    return parseMemoryFirmware(b);
}
void append(std::vector<Point>& history, double time, double value) {
    history.push_back({time, value});
    history.erase(history.begin(), std::find_if(history.begin(), history.end(),
                                                [&](const Point& p) { return p.time >= time - 60; }));
}
int iconFor(const std::wstring& n) {
    auto s = lower(n);
    if (s == L"explorer.exe")
        return 1;
    if (s == L"chrome.exe")
        return 2;
    if (s == L"msedge.exe")
        return 3;
    if (s == L"calculatorapp.exe" || s == L"calculator.exe")
        return 4;
    if (s == L"notepad.exe")
        return 5;
    if (s == L"systemsettings.exe")
        return 6;
    if (s.find(L"terminal") != s.npos)
        return 7;
    if (s == L"onedrive.exe")
        return 8;
    if (s.find(L"nvidia") != s.npos || s == L"nvcontainer.exe")
        return 9;
    return 0;
}
std::set<DWORD> visibleApps() {
    std::set<DWORD> out;
    EnumWindows(
        [](HWND w, LPARAM ptr) -> BOOL {
            if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER) ||
                (GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
                return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid && GetWindowTextLengthW(w) > 0)
                reinterpret_cast<std::set<DWORD>*>(ptr)->insert(pid);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&out));
    return out;
}
CpuInfo cpuInfo() {
    CpuInfo c;
    c.logical = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    c.virtualization = IsProcessorFeaturePresent(PF_VIRT_FIRMWARE_ENABLED);
    wchar_t name[256]{};
    DWORD bytes = sizeof(name);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, name, &bytes) == ERROR_SUCCESS)
        c.name = name;
    while (!c.name.empty() && c.name.front() == L' ')
        c.name.erase(c.name.begin());
    DWORD mhz = 0;
    bytes = sizeof(mhz);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"~MHz",
                     RRF_RT_REG_DWORD, nullptr, &mhz, &bytes) == ERROR_SUCCESS)
        c.baseGHz = mhz / 1000.0;
    DWORD size = 0;
    GetLogicalProcessorInformationEx(RelationAll, nullptr, &size);
    std::vector<unsigned char> buffer(size);
    if (size &&
        GetLogicalProcessorInformationEx(
            RelationAll, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &size)) {
        for (size_t offset = 0; offset + 8 <= size;) {
            auto* p = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + offset);
            if (p->Size == 0 || offset + p->Size > size)
                break;
            if (p->Relationship == RelationProcessorCore)
                c.cores++;
            if (p->Relationship == RelationProcessorPackage)
                c.sockets++;
            if (p->Relationship == RelationCache) {
                auto& cache = p->Cache;
                if (cache.Level == 1)
                    c.l1 += cache.CacheSize;
                if (cache.Level == 2)
                    c.l2 += cache.CacheSize;
                if (cache.Level == 3)
                    c.l3 += cache.CacheSize;
            }
            offset += p->Size;
        }
    }
    return c;
}
struct MemoryEx2 {
    PROCESS_MEMORY_COUNTERS_EX base;
    SIZE_T privateWorkingSet;
    ULONG64 sharedCommit;
};
struct CachedProcess {
    HANDLE handle = nullptr;
    uint64_t born = 0, lastCpu = 0;
    double lastTime = 0;
    std::wstring path, user, arch;
    ~CachedProcess() {
        if (handle)
            CloseHandle(handle);
    }
};
void processMetadata(CachedProcess& p) {
    wchar_t path[32768];
    DWORD length = 32768;
    if (QueryFullProcessImageNameW(p.handle, 0, path, &length))
        p.path.assign(path, length);
    USHORT machine = 0, native = 0;
    if (IsWow64Process2(p.handle, &machine, &native))
        p.arch = machine == IMAGE_FILE_MACHINE_I386   ? L"x86"
                 : native == IMAGE_FILE_MACHINE_ARM64 ? L"ARM64"
                                                      : L"x64";
    HANDLE token = nullptr;
    if (OpenProcessToken(p.handle, TOKEN_QUERY, &token)) {
        Handle owned(token);
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<unsigned char> buffer(size);
        if (size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
            auto* u = reinterpret_cast<TOKEN_USER*>(buffer.data());
            wchar_t name[256], domain[256];
            DWORD n = 256, d = 256;
            SID_NAME_USE use{};
            // LookupAccountSid can contact a domain. Avoid it in the fast sampler:
            // well-known identities and the current local user need no directory lookup.
            if (IsWellKnownSid(u->User.Sid, WinLocalSystemSid))
                p.user = L"SYSTEM";
            else if (IsWellKnownSid(u->User.Sid, WinLocalServiceSid))
                p.user = L"LOCAL SERVICE";
            else if (IsWellKnownSid(u->User.Sid, WinNetworkServiceSid))
                p.user = L"NETWORK SERVICE";
            else {
                HANDLE current = nullptr;
                if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &current)) {
                    Handle ct(current);
                    DWORD len = 0;
                    GetTokenInformation(current, TokenUser, nullptr, 0, &len);
                    std::vector<unsigned char> own(len);
                    if (len && GetTokenInformation(current, TokenUser, own.data(), len, &len) &&
                        EqualSid(u->User.Sid, reinterpret_cast<TOKEN_USER*>(own.data())->User.Sid)) {
                        if (GetUserNameW(name, &n))
                            p.user = name;
                    }
                }
            }
            (void)domain;
            (void)d;
            (void)use;
        }
    }
}
struct Counter {
    PDH_HCOUNTER handle = nullptr;
#ifdef CPU_STARTUP_DIAGNOSTICS
    bool debugLogical = false;
#endif
    void add(PDH_HQUERY query, const wchar_t* path) {
#ifdef CPU_STARTUP_DIAGNOSTICS
        debugLogical = wcscmp(path, L"\\Processor Information(*)\\% Processor Time") == 0;
#endif
        if (PdhAddEnglishCounterW(query, path, 0, &handle) != ERROR_SUCCESS)
            handle = nullptr;
    }
    double scalar() const {
        if (!handle)
            return unknown;
        PDH_FMT_COUNTERVALUE v{};
        if (PdhGetFormattedCounterValue(handle, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &v) ==
                ERROR_SUCCESS &&
            (v.CStatus == PDH_CSTATUS_VALID_DATA || v.CStatus == PDH_CSTATUS_NEW_DATA) && good(v.doubleValue))
            return v.doubleValue;
        return unknown;
    }
    std::map<std::wstring, double> values() const {
        std::map<std::wstring, double> result;
        if (!handle)
            return result;
        for (int attempt = 0; attempt < 3; attempt++) {
            DWORD bytes = 0, count = 0;
            PDH_STATUS status = PdhGetFormattedCounterArrayW(handle, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100,
                                                             &bytes, &count, nullptr);
            if (status != static_cast<PDH_STATUS>(PDH_MORE_DATA) || !bytes || bytes > 64 * 1024 * 1024)
                return result;
            std::vector<unsigned char> buffer(bytes);
            auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
            status = PdhGetFormattedCounterArrayW(handle, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count,
                                                  items);
            if (status == static_cast<PDH_STATUS>(PDH_MORE_DATA))
                continue;
            if (status != ERROR_SUCCESS)
                return result;
            for (DWORD i = 0; i < count; i++) {
                auto& v = items[i].FmtValue;
#ifdef CPU_STARTUP_DIAGNOSTICS
                if (debugLogical &&
                    ((v.CStatus != PDH_CSTATUS_VALID_DATA && v.CStatus != PDH_CSTATUS_NEW_DATA) ||
                     !good(v.doubleValue)))
                    fwprintf(stderr, L"LOGICAL_SOURCE name=%ls status=0x%08lx value=%.9f\n",
                             items[i].szName ? items[i].szName : L"", v.CStatus, v.doubleValue);
#endif
                if ((v.CStatus == PDH_CSTATUS_VALID_DATA || v.CStatus == PDH_CSTATUS_NEW_DATA) &&
                    good(v.doubleValue) && items[i].szName)
                    result[items[i].szName] = v.doubleValue;
            }
            return result;
        }
        return result;
    }
};
struct Query {
    PDH_HQUERY handle = nullptr;
    Query() {
        PdhOpenQueryW(nullptr, 0, &handle);
    }
    ~Query() {
        if (handle)
            PdhCloseQuery(handle);
    }
};
std::wstring luidKey(LUID l) {
    wchar_t b[64];
    swprintf(b, 64, L"%08x_%08x", static_cast<unsigned>(l.HighPart), static_cast<unsigned>(l.LowPart));
    return b;
}
std::wstring instanceLuid(const std::wstring& s) {
    auto pos = s.find(L"luid_0x");
    if (pos == s.npos)
        return L"";
    unsigned high = 0, low = 0;
    if (swscanf(s.c_str() + pos, L"luid_0x%x_0x%x", &high, &low) != 2)
        return L"";
    wchar_t b[64];
    swprintf(b, 64, L"%08x_%08x", high, low);
    return b;
}
bool adapterQuery(LUID luid, unsigned type, void* data, unsigned size) {
    struct Open {
        LUID luid;
        UINT handle;
    };
    struct Close {
        UINT handle;
    };
    struct Info {
        UINT handle, type;
        void* data;
        UINT size;
    };
    using OpenFn = LONG(WINAPI*)(Open*);
    using CloseFn = LONG(WINAPI*)(const Close*);
    using InfoFn = LONG(WINAPI*)(const Info*);
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi)
        return false;
    auto open = std::bit_cast<OpenFn>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid"));
    auto close = std::bit_cast<CloseFn>(GetProcAddress(gdi, "D3DKMTCloseAdapter"));
    auto query = std::bit_cast<InfoFn>(GetProcAddress(gdi, "D3DKMTQueryAdapterInfo"));
    if (!open || !close || !query)
        return false;
    Open o{luid, 0};
    if (open(&o) != 0)
        return false;
    Info info{o.handle, type, data, size};
    LONG result = query(&info);
    Close c{o.handle};
    close(&c);
    return result == 0;
}
std::vector<Device> gpuInventory() {
    std::vector<Device> out;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
        return out;
    for (UINT i = 0; i < 64; i++) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) != S_OK)
            break;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            Device d;
            d.kind = Kind::Gpu;
            d.key = L"gpu:" + luidKey(desc.AdapterLuid);
            d.adapter = luidKey(desc.AdapterLuid);
            adapterQuery(desc.AdapterLuid, 15, &d.adapterFlags, sizeof(d.adapterFlags));
            if (d.adapterFlags & ((1u << 2) | (1u << 6))) {
                adapter->Release();
                continue;
            }
            struct Address {
                UINT bus, device, function;
            };
            Address address{};
            if (adapterQuery(desc.AdapterLuid, 6, &address, sizeof(address)) && address.bus != 0xFFFFFFFF) {
                d.physicalId = std::to_wstring(address.bus) + L":" + std::to_wstring(address.device) + L":" +
                               std::to_wstring(address.function);
            }
            d.name = L"GPU " + std::to_wstring(out.size());
            d.description = desc.Description;
            d.color = 0x97A8F4;
            d.capacity = desc.DedicatedVideoMemory;
            d.sharedCapacity = desc.SharedSystemMemory;
            out.push_back(d);
        }
        adapter->Release();
    }
    factory->Release();
    return out;
}
double temperature(const std::wstring& key) {
    // Query the already-installed graphics driver. No external driver or vendor DLL.
    struct Perf {
        UINT physical;
        alignas(8) uint64_t memoryFreq, maxMemoryFreq, maxMemoryFreqOc, bandwidth, pcie;
        ULONG fan, power, temp;
        UCHAR overrideState;
    };
    unsigned high = 0, low = 0;
    if (swscanf(key.c_str(), L"gpu:%x_%x", &high, &low) != 2)
        return unknown;
    Perf p{};
    return adapterQuery({low, static_cast<LONG>(high)}, 62, &p, sizeof(p)) && p.temp > 0 && p.temp < 1500
               ? p.temp / 10.0
               : unknown;
}
void diskInfo(Device& d, int index) {
    std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(index);
    Handle h(
        CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (h.value == INVALID_HANDLE_VALUE)
        return;
    DWORD got = 0;
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    std::vector<unsigned char> b(4096);
    if (DeviceIoControl(h.value, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), b.data(),
                        static_cast<DWORD>(b.size()), &got, nullptr)) {
        auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(b.data());
        auto stringAt = [&](DWORD offset) {
            if (!offset || offset >= got)
                return std::wstring{};
            auto* s = reinterpret_cast<char*>(b.data() + offset);
            size_t len = strnlen(s, got - offset);
            int n = MultiByteToWideChar(CP_ACP, 0, s, static_cast<int>(len), nullptr, 0);
            std::wstring w(n, L'\0');
            if (n)
                MultiByteToWideChar(CP_ACP, 0, s, static_cast<int>(len), w.data(), n);
            while (!w.empty() && w.back() == L' ')
                w.pop_back();
            return w;
        };
        d.description = stringAt(desc->ProductIdOffset);
        d.type = desc->BusType == BusTypeNvme   ? L"SSD (NVMe)"
                 : desc->BusType == BusTypeSata ? L"SATA"
                                                : L"Disk";
        d.physicalId = stringAt(desc->SerialNumberOffset);
    }
    q.PropertyId = StorageDeviceSeekPenaltyProperty;
    DEVICE_SEEK_PENALTY_DESCRIPTOR penalty{};
    if (DeviceIoControl(h.value, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &penalty, sizeof(penalty), &got,
                        nullptr) &&
        got >= sizeof(penalty) && penalty.Size >= sizeof(penalty)) {
        if (penalty.IncursSeekPenalty)
            d.type = L"HDD";
        else if (d.type == L"SATA")
            d.type = L"SSD (SATA)";
    }
    DISK_GEOMETRY_EX geometry{};
    if (DeviceIoControl(h.value, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geometry, sizeof(geometry),
                        &got, nullptr))
        d.capacity = geometry.DiskSize.QuadPart;
}
std::wstring volumeForPath(std::wstring path) {
    if (path.rfind(L"\\??\\", 0) == 0)
        path.erase(0, 4);
    if (path.rfind(L"\\\\?\\", 0) == 0 && path.size() > 6 && path[5] == L':')
        path.erase(0, 4);
    bool drive = path.size() > 2 && iswalpha(path[0]) && path[1] == L':' && path[2] == L'\\';
    if (!drive && path.rfind(L"\\\\?\\Volume{", 0) != 0)
        return L"";
    wchar_t root[32768]{}, volume[128]{};
    if (!GetVolumePathNameW(path.c_str(), root, 32768) ||
        !GetVolumeNameForVolumeMountPointW(root, volume, 128))
        return L"";
    return lower(volume);
}
StorageInventory storageInventory(const std::atomic<bool>& stopping) {
    StorageInventory out;
    struct ErrorMode {
        DWORD original = GetThreadErrorMode();
        bool changed =
            SetThreadErrorMode(original | SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, nullptr) != FALSE;
        ~ErrorMode() {
            if (changed)
                SetThreadErrorMode(original, nullptr);
        }
    } quiet;
    using Space = HRESULT(WINAPI*)(LPCWSTR, DISK_SPACE_INFORMATION*);
    auto space =
        std::bit_cast<Space>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetDiskSpaceInformationW"));
    wchar_t name[1024]{};
    HANDLE find = FindFirstVolumeW(name, 1024);
    bool mappingsComplete = true;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (stopping)
                break;
            StorageVolume v;
            v.key = lower(name);
            auto path = v.key;
            if (!path.empty() && path.back() == L'\\')
                path.pop_back();
            UINT type = GetDriveTypeW(v.key.c_str());
            if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) {
                if (type == DRIVE_UNKNOWN || type == DRIVE_NO_ROOT_DIR)
                    mappingsComplete = false;
                continue;
            }
            Handle volume(CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, 0, nullptr));
            if (volume.value == INVALID_HANDLE_VALUE) {
                mappingsComplete = false;
                continue;
            }
            std::vector<unsigned char> buffer(sizeof(VOLUME_DISK_EXTENTS));
            DWORD got = 0;
            bool unsupported = false;
            for (int attempt = 0; attempt < 4; attempt++) {
                if (DeviceIoControl(volume.value, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
                                    buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr)) {
                    if (got <= buffer.size()) {
                        buffer.resize(got);
                        v.disks = volumeDisks(buffer);
                    }
                    break;
                }
                DWORD error = GetLastError();
                if (error != ERROR_MORE_DATA) {
                    unsupported = error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED;
                    break;
                }
                if (got < sizeof(DWORD) || buffer.size() < sizeof(DWORD))
                    break;
                DWORD count = 0;
                memcpy(&count, buffer.data(), sizeof(count));
                if (!count || count > 4096)
                    break;
                size_t needed =
                    offsetof(VOLUME_DISK_EXTENTS, Extents) + static_cast<size_t>(count) * sizeof(DISK_EXTENT);
                if (needed <= buffer.size())
                    break;
                buffer.resize(needed);
            }
            // Some nonpartitioned virtual/RAM disks expose a simple storage
            // number but do not implement volume extents. Never use this to
            // reinterpret malformed or multi-disk extent results.
            if (v.disks.empty() && unsupported) {
                STORAGE_DEVICE_NUMBER number{};
                if (DeviceIoControl(volume.value, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &number,
                                    sizeof(number), &got, nullptr) &&
                    got >= sizeof(number) && number.DeviceType == FILE_DEVICE_DISK &&
                    number.DeviceNumber != MAXDWORD)
                    v.disks.push_back(number.DeviceNumber);
            }
            if (v.disks.empty()) {
                mappingsComplete = false;
                continue;
            }
            DISK_SPACE_INFORMATION info{};
            if (space && space(v.key.c_str(), &info) == S_OK)
                v.formattedCapacity = formattedBytes(info.ActualTotalAllocationUnits,
                                                     info.SectorsPerAllocationUnit, info.BytesPerSector);
            if (!good(v.formattedCapacity)) {
                // Windows can reject GetDiskSpaceInformation for FAT32 EFI
                // volumes. The older total is safe only when this filesystem
                // explicitly reports that it cannot apply per-user quotas.
                DWORD flags = 0;
                wchar_t filesystem[64]{};
                ULARGE_INTEGER total{};
                if (GetVolumeInformationW(v.key.c_str(), nullptr, 0, nullptr, nullptr, &flags, filesystem,
                                          64) &&
                    filesystem[0] && !(flags & FILE_VOLUME_QUOTAS) &&
                    GetDiskFreeSpaceExW(v.key.c_str(), nullptr, &total, nullptr))
                    v.formattedCapacity = static_cast<double>(total.QuadPart);
            }
            out.volumes.push_back(std::move(v));
        } while (FindNextVolumeW(find, name, 1024));
        DWORD enumerationError = GetLastError();
        out.volumesComplete = mappingsComplete && !stopping && enumerationError == ERROR_NO_MORE_FILES;
        FindVolumeClose(find);
    }
    if (stopping)
        return out;
    wchar_t windows[32768]{};
    UINT length = GetSystemWindowsDirectoryW(windows, 32768);
    if (length && length < 32768)
        out.windowsVolume = volumeForPath(windows);
    std::vector<std::wstring> paths;
    bool enumerated =
        EnumPageFilesW(
            [](LPVOID context, PENUM_PAGE_FILE_INFORMATION, LPCWSTR file) -> BOOL {
                reinterpret_cast<std::vector<std::wstring>*>(context)->push_back(file ? file : L"");
                return TRUE;
            },
            &paths) != FALSE;
    out.pageFilesComplete = enumerated;
    for (auto& path : paths) {
        auto volume = volumeForPath(path);
        if (volume.empty())
            out.pageFilesComplete = false;
        else
            out.pageFileVolumes.push_back(std::move(volume));
    }
    return out;
}
void inventoryExtras(DeviceSnapshot& s) {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (manager) {
        DWORD bytes = 0, count = 0, resume = 0;
        EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, nullptr, 0,
                              &bytes, &count, &resume, nullptr);
        std::vector<unsigned char> b(bytes + 4096);
        resume = 0;
        if (EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, b.data(),
                                  static_cast<DWORD>(b.size()), &bytes, &count, &resume, nullptr)) {
            auto* services = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(b.data());
            for (DWORD i = 0; i < count; i++) {
                auto& v = services[i];
                DWORD state = v.ServiceStatusProcess.dwCurrentState;
                s.services.push_back({v.lpServiceName, v.lpDisplayName,
                                      state == SERVICE_RUNNING   ? L"Running"
                                      : state == SERVICE_STOPPED ? L"Stopped"
                                      : state == SERVICE_PAUSED  ? L"Paused"
                                                                 : L"Pending",
                                      v.ServiceStatusProcess.dwProcessId});
            }
        }
        CloseServiceHandle(manager);
    }
    PWTS_SESSION_INFOW sessions = nullptr;
    DWORD count = 0;
    if (WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) {
        for (DWORD i = 0; i < count; i++) {
            LPWSTR name = nullptr;
            DWORD bytes = 0;
            if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessions[i].SessionId, WTSUserName,
                                            &name, &bytes)) {
                if (name && *name)
                    s.users.push_back({std::to_wstring(sessions[i].SessionId), name,
                                       sessions[i].State == WTSActive ? L"" : L"Disconnected",
                                       sessions[i].SessionId});
                WTSFreeMemory(name);
            }
        }
        WTSFreeMemory(sessions);
    }
    for (int hive = 0; hive < 2; hive++)
        for (int wow = 0; wow < 2; wow++) {
            HKEY root = hive ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, key = nullptr;
            if (RegOpenKeyExW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                              KEY_READ | (wow ? KEY_WOW64_32KEY : KEY_WOW64_64KEY), &key) != ERROR_SUCCESS)
                continue;
            for (DWORD i = 0; i < 512; i++) {
                wchar_t name[1024], command[16384];
                DWORD n = 1024, bytes = sizeof(command), type = 0;
                LONG result =
                    RegEnumValueW(key, i, name, &n, nullptr, &type, reinterpret_cast<BYTE*>(command), &bytes);
                if (result == ERROR_NO_MORE_ITEMS)
                    break;
                if (result != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
                    continue;
                command[16383] = 0;
                std::wstring id = std::to_wstring(hive) + L":" + name;
                if (std::any_of(s.startup.begin(), s.startup.end(),
                                [&](const Startup& a) { return a.key == id; }))
                    continue;
                BYTE state[64]{};
                DWORD size = sizeof(state);
                std::wstring path =
                    wow ? L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run32"
                        : L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
                bool known = RegGetValueW(root, path.c_str(), name, RRF_RT_REG_BINARY, nullptr, state,
                                          &size) == ERROR_SUCCESS;
                std::wstring status = !known          ? L""
                                      : state[0] == 2 ? L"Enabled"
                                      : state[0] == 3 ? L"Disabled"
                                                      : L"";
                s.startup.push_back({id, name, command, status});
            }
            RegCloseKey(key);
        }
}
} // namespace
MemoryHardware parseMemoryFirmware(const std::vector<unsigned char>& raw) {
    MemoryHardware out;
    if (raw.size() < 8)
        return out;
    auto u16 = [&](size_t p) -> uint16_t { return static_cast<uint16_t>(raw[p] | (raw[p + 1] << 8)); };
    auto u32 = [&](size_t p) -> uint32_t {
        return static_cast<uint32_t>(raw[p]) | (static_cast<uint32_t>(raw[p + 1]) << 8) |
               (static_cast<uint32_t>(raw[p + 2]) << 16) | (static_cast<uint32_t>(raw[p + 3]) << 24);
    };
    size_t length = u32(4);
    if (length > raw.size() - 8)
        return out;
    size_t end = 8 + length;
    std::map<unsigned, unsigned> arrays;
    struct ModuleWithArray {
        unsigned array;
        MemoryModule module;
    };
    std::vector<ModuleWithArray> modules;
    for (size_t p = 8; p + 4 <= end;) {
        unsigned type = raw[p], n = raw[p + 1];
        if (n < 4 || p + n > end)
            return {};
        size_t strings = p + n, q = strings;
        while (q + 1 < end && (raw[q] != 0 || raw[q + 1] != 0))
            q++;
        if (q + 1 >= end)
            return {};
        auto stringAt = [&](unsigned index) {
            if (!index)
                return std::wstring{};
            size_t a = strings;
            for (unsigned i = 1; i < index && a < q; i++) {
                while (a < q && raw[a])
                    a++;
                a++;
            }
            if (a >= q)
                return std::wstring{};
            size_t b = a;
            while (b <= q && raw[b])
                b++;
            int count = MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(raw.data() + a),
                                            static_cast<int>(b - a), nullptr, 0);
            std::wstring text(count, L'\0');
            if (count)
                MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(raw.data() + a),
                                    static_cast<int>(b - a), text.data(), count);
            return text;
        };
        if (type == 16 && n >= 15 && raw[p + 5] == 3)
            arrays[u16(p + 2)] = u16(p + 13);
        if (type == 17 && n >= 21) {
            MemoryModule m;
            unsigned size = u16(p + 12);
            m.populated = size != 0;
            m.capacityKnown = size != 0xffff;
            if (size == 0x7fff) {
                if (n >= 32)
                    m.capacity = static_cast<uint64_t>(u32(p + 28) & 0x7fffffff) * 1048576;
                else
                    m.capacityKnown = false;
            } else if (size != 0xffff)
                m.capacity = static_cast<uint64_t>(size & 0x7fff) * (size & 0x8000 ? 1024 : 1048576);
            m.locator = stringAt(raw[p + 16]);
            m.bank = stringAt(raw[p + 17]);
            unsigned memoryType = raw[p + 18];
            m.type = memoryType == 0x22   ? L"DDR5"
                     : memoryType == 0x1a ? L"DDR4"
                     : memoryType == 0x18 ? L"DDR3"
                     : memoryType == 0x23 ? L"LPDDR5"
                     : memoryType == 0x1e ? L"LPDDR4"
                                          : L"";
            unsigned form = raw[p + 14];
            m.formFactor = form == 9 ? L"DIMM" : form == 13 ? L"SODIMM" : form == 16 ? L"Onboard" : L"";
            if (n >= 34) {
                unsigned speed = u16(p + 32);
                if (speed == 0xffff && n >= 92)
                    m.speed = u32(p + 88);
                else if (speed != 0xffff)
                    m.speed = speed;
            }
            modules.push_back({u16(p + 4), std::move(m)});
        }
        p = q + 2;
        if (type == 127)
            break;
    }
    for (auto& a : arrays)
        out.slots += a.second;
    bool first = true, speedKnown = true;
    for (auto& a : modules) {
        if (!arrays.empty() && !arrays.count(a.array))
            continue;
        auto& m = a.module;
        out.modules.push_back(m);
        if (!m.populated)
            continue;
        out.used++;
        if (!out.occupied.empty())
            out.occupied += L", ";
        out.occupied += m.locator.empty() ? m.bank : m.locator;
        if (first) {
            out.type = m.type;
            out.formFactor = m.formFactor;
            out.speed = m.speed;
            first = false;
        } else {
            if (out.type != m.type)
                out.type.clear();
            if (out.formFactor != m.formFactor)
                out.formFactor.clear();
            if (out.speed != m.speed)
                speedKnown = false;
        }
        if (!m.speed)
            speedKnown = false;
    }
    if (!speedKnown)
        out.speed = 0;
    if (arrays.empty())
        out.slots = static_cast<unsigned>(out.modules.size());
    out.valid = !out.modules.empty() || !arrays.empty();
    return out;
}
std::vector<ProcessGroup> groupProcesses(const std::vector<Process>& processes) {
    std::vector<ProcessGroup> out;
    std::map<std::wstring, size_t> byKey;
    for (auto& p : processes) {
        if (!p.pid)
            continue;
        std::wstring key = (p.path.empty() ? L"name:" + lower(p.name) : L"path:" + lower(p.path)) +
                           L"|session:" + std::to_wstring(p.session);
        auto [it, inserted] = byKey.emplace(key, out.size());
        if (inserted) {
            ProcessGroup group;
            group.key = key;
            group.summary = p;
            group.members.push_back(p);
            out.push_back(std::move(group));
        } else {
            auto& g = out[it->second];
            g.members.push_back(p);
            g.summary.group = std::min(g.summary.group, p.group);
            g.summary.cpu += p.cpu;
            g.summary.memory += p.memory;
            g.summary.disk += p.disk;
            g.summary.network += p.network;
            g.summary.threads += p.threads;
            g.summary.handles += p.handles;
            g.summary.efficient = g.summary.efficient && p.efficient;
        }
    }
    for (auto& g : out) {
        g.summary.children = g.members.size() > 1 ? static_cast<int>(g.members.size()) : 0;
    }
    return out;
}
double clockSeconds() {
    LARGE_INTEGER n, f;
    QueryPerformanceCounter(&n);
    QueryPerformanceFrequency(&f);
    return static_cast<double>(n.QuadPart) / f.QuadPart;
}
double rate(uint64_t previous, uint64_t current, double seconds) {
    return current >= previous && std::isfinite(seconds) && seconds > 0
               ? static_cast<double>(current - previous) / seconds
               : unknown;
}
double cpuPercent(uint64_t previous, uint64_t current, double seconds, unsigned logical) {
    double v = rate(previous, current, seconds);
    return logical && good(v) ? std::clamp(v / 100000.0 / logical, 0.0, 100.0) : unknown;
}
bool metricLess(double a, double b, bool ascending) {
    bool ak = std::isfinite(a), bk = std::isfinite(b);
    if (ak != bk)
        return ak;
    if (!ak)
        return false;
    return ascending ? a < b : a > b;
}
double gpuUsage(const std::vector<Engine>& engines, const std::wstring& adapter) {
    std::map<std::pair<unsigned, unsigned>, double> totals;
    for (auto& e : engines)
        if (e.adapter == adapter && good(e.usage))
            totals[{e.physical, e.node}] += e.usage;
    if (totals.empty())
        return unknown;
    double peak = 0;
    for (auto& e : totals)
        peak = std::max(peak, e.second);
    return std::clamp(peak, 0.0, 100.0);
}
double formattedBytes(uint64_t units, unsigned sectors, unsigned bytes) {
    if (!sectors || !bytes)
        return unknown;
    uint64_t cluster = uint64_t(sectors) * bytes;
    if (units > std::numeric_limits<uint64_t>::max() / cluster)
        return unknown;
    return static_cast<double>(units * cluster);
}
std::vector<unsigned> volumeDisks(const std::vector<unsigned char>& bytes) {
    constexpr size_t offset = offsetof(VOLUME_DISK_EXTENTS, Extents);
    if (bytes.size() < offset)
        return {};
    DWORD count = 0;
    memcpy(&count, bytes.data(), sizeof(count));
    if (!count || count > (bytes.size() - offset) / sizeof(DISK_EXTENT))
        return {};
    std::set<unsigned> disks;
    for (DWORD i = 0; i < count; i++) {
        DISK_EXTENT extent{};
        memcpy(&extent, bytes.data() + offset + static_cast<size_t>(i) * sizeof(extent), sizeof(extent));
        if (extent.DiskNumber == MAXDWORD || extent.StartingOffset.QuadPart < 0 ||
            extent.ExtentLength.QuadPart <= 0 ||
            extent.StartingOffset.QuadPart > INT64_MAX - extent.ExtentLength.QuadPart)
            return {};
        disks.insert(extent.DiskNumber);
    }
    return {disks.begin(), disks.end()};
}
DiskDetails diskDetails(unsigned disk, const StorageInventory& inventory) {
    DiskDetails out;
    double total = 0;
    bool found = false, complete = true;
    std::set<std::wstring> seen;
    auto contains = [&](const StorageVolume& v) {
        return std::find(v.disks.begin(), v.disks.end(), disk) != v.disks.end();
    };
    auto lookup = [&](const std::wstring& key) -> const StorageVolume* {
        for (auto& v : inventory.volumes)
            if (!key.empty() && lower(v.key) == lower(key) && !v.disks.empty())
                return &v;
        return nullptr;
    };
    for (auto& v : inventory.volumes)
        if (contains(v) && seen.insert(lower(v.key)).second) {
            found = true;
            if (!good(v.formattedCapacity) ||
                std::any_of(v.disks.begin(), v.disks.end(), [&](unsigned d) { return d != disk; }))
                complete = false;
            else
                total += v.formattedCapacity;
        }
    if (inventory.volumesComplete && found && complete && std::isfinite(total))
        out.formattedCapacity = total;
    if (auto windows = lookup(inventory.windowsVolume))
        out.systemDisk = contains(*windows);
    bool pagesKnown = inventory.pageFilesComplete, hasPage = false;
    for (auto& key : inventory.pageFileVolumes) {
        auto volume = lookup(key);
        if (!volume)
            pagesKnown = false;
        else if (contains(*volume))
            hasPage = true;
    }
    if (hasPage || pagesKnown)
        out.pageFile = hasPage;
    return out;
}
GpuEngineKind gpuEngineKind(const std::wstring& instance) {
    auto pos = instance.find(L"_engtype_");
    if (pos == instance.npos)
        return GpuEngineKind::Other;
    auto name = lower(instance.substr(pos + 9));
    if (name == L"3d")
        return GpuEngineKind::ThreeD;
    if (name == L"copy")
        return GpuEngineKind::Copy;
    if (name == L"videoencode")
        return GpuEngineKind::VideoEncode;
    if (name == L"videodecode")
        return GpuEngineKind::VideoDecode;
    return GpuEngineKind::Other;
}
double gpuEngineUsage(const std::vector<Engine>& engines, const std::wstring& adapter, GpuEngineKind kind) {
    std::map<std::pair<unsigned, unsigned>, double> totals;
    for (auto& e : engines)
        if (e.adapter == adapter && e.kind == kind && good(e.usage))
            totals[{e.physical, e.node}] += e.usage;
    if (totals.empty())
        return unknown;
    double peak = 0;
    for (auto& e : totals)
        peak = std::max(peak, e.second);
    return std::clamp(peak, 0.0, 100.0);
}
void retainGpuHistory(Device& current, const Device& previous) {
    if (current.key != previous.key || current.adapter != previous.adapter)
        return;
    current.history = previous.history;
    current.dedicatedHistory = previous.dedicatedHistory;
    current.sharedHistory = previous.sharedHistory;
    for (size_t i = 0; i < current.gpuEngines.size(); i++)
        current.gpuEngines[i].history = previous.gpuEngines[i].history;
}
struct Collector::Impl {
    std::atomic<bool> stopping{false};
    std::atomic<unsigned> interval{1000};
    std::atomic<double> started{0};
    HANDLE stopEvent = nullptr;
    std::thread coreThread, deviceThread;
    mutable std::mutex mutex;
    std::shared_ptr<const CoreSnapshot> core = std::make_shared<CoreSnapshot>();
    std::shared_ptr<const DeviceSnapshot> devices = std::make_shared<DeviceSnapshot>();
    Notify notify;
    activity::Trace activityTrace;
    bool wait(unsigned ms) {
        return stopping.load() || WaitForSingleObject(stopEvent, ms) != WAIT_TIMEOUT;
    }
    void publish(std::shared_ptr<const CoreSnapshot> p) {
        {
            std::lock_guard lock(mutex);
            core = std::move(p);
        }
        if (!stopping && notify)
            notify();
    }
    void publish(std::shared_ptr<const DeviceSnapshot> p) {
        {
            std::lock_guard lock(mutex);
            devices = std::move(p);
        }
        if (!stopping && notify)
            notify();
    }
    void runCore(unsigned delay) {
        if (wait(delay))
            return;
        started.store(clockSeconds());
        activityTrace.start(notify);
        // An administrator token still requires this privilege to be enabled.
        // This runs on the worker after the first visible UI presentation.
        HANDLE token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
            TOKEN_PRIVILEGES privilege{};
            privilege.PrivilegeCount = 1;
            if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &privilege.Privileges[0].Luid)) {
                privilege.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                AdjustTokenPrivileges(token, FALSE, &privilege, 0, nullptr, nullptr);
            }
            CloseHandle(token);
        }
        CpuInfo info = cpuInfo();
        std::map<DWORD, std::unique_ptr<CachedProcess>> cache;
        FILETIME pi{}, pk{}, pu{};
        bool previous = false;
        std::vector<Point> cpuHistory, memoryHistory;
        uint64_t rev = 0;
        std::shared_ptr<const activity::Frame> previousActivity;
        std::map<activity::Key, std::pair<double, double>> activityRates;
        process_disk::Sampler diskSampler;
        process_disk::Snapshot previousDisk;
        while (!stopping) {
            double begin = clockSeconds();
            auto s = std::make_shared<CoreSnapshot>();
            s->revision = ++rev;
            s->timestamp = begin;
            s->info = info;
            s->upMilliseconds = GetTickCount64();
            FILETIME idle{}, kernel{}, user{};
            if (GetSystemTimes(&idle, &kernel, &user)) {
                if (previous && GetActiveProcessorGroupCount() == 1) {
                    uint64_t k = ticks(kernel), u = ticks(user), old = ticks(pk) + ticks(pu), sum = k + u;
                    uint64_t id = ticks(idle), oldId = ticks(pi);
                    if (sum > old && id >= oldId && id - oldId <= sum - old)
                        s->cpu = 100.0 * (1.0 - static_cast<double>(id - oldId) / (sum - old));
                }
                pi = idle;
                pk = kernel;
                pu = user;
                previous = true;
            }
            MEMORYSTATUSEX mem{};
            mem.dwLength = sizeof(mem);
            if (GlobalMemoryStatusEx(&mem)) {
                s->memory.valid = true;
                s->memory.total = mem.ullTotalPhys;
                s->memory.available = mem.ullAvailPhys;
                ULONGLONG installed = 0;
                if (GetPhysicallyInstalledSystemMemory(&installed))
                    s->memory.installed = installed * 1024;
                PERFORMANCE_INFORMATION perf{};
                perf.cb = sizeof(perf);
                if (GetPerformanceInfo(&perf, sizeof(perf))) {
                    s->memory.performanceValid = true;
                    s->memory.commit = perf.CommitTotal * perf.PageSize;
                    s->memory.commitLimit = perf.CommitLimit * perf.PageSize;
                    s->memory.cache = perf.SystemCache * perf.PageSize;
                    s->memory.paged = perf.KernelPaged * perf.PageSize;
                    s->memory.nonpaged = perf.KernelNonpaged * perf.PageSize;
                    s->totalProcesses = perf.ProcessCount;
                    s->totalThreads = perf.ThreadCount;
                    s->totalHandles = perf.HandleCount;
                }
            }
            auto apps = visibleApps();
            std::set<DWORD> alive;
            Handle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            if (snap.value != INVALID_HANDLE_VALUE && Process32FirstW(snap.value, &entry))
                do {
                    if (stopping)
                        break;
                    Process p;
                    p.pid = entry.th32ProcessID;
                    p.parent = entry.th32ParentProcessID;
                    p.threads = entry.cntThreads;
                    p.name = entry.szExeFile;
                    p.icon = iconFor(p.name);
                    p.group = apps.count(p.pid) ? 0 : 1;
                    ProcessIdToSessionId(p.pid, &p.session);
                    alive.insert(p.pid);
                    auto systemName = lower(p.name);
                    if (p.group != 0 &&
                        (p.pid <= 4 || systemName == L"registry" || systemName == L"smss.exe" ||
                         systemName == L"csrss.exe" || systemName == L"wininit.exe" ||
                         systemName == L"winlogon.exe" || systemName == L"services.exe" ||
                         systemName == L"lsass.exe" || systemName == L"svchost.exe"))
                        p.group = 2;
                    auto it = cache.find(p.pid);
                    if (it != cache.end() && WaitForSingleObject(it->second->handle, 0) == WAIT_OBJECT_0) {
                        cache.erase(it);
                        it = cache.end();
                    }
                    if (it == cache.end()) {
                        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, p.pid);
                        if (h) {
                            auto c = std::make_unique<CachedProcess>();
                            c->handle = h;
                            FILETIME created{}, exit{}, k{}, u{};
                            if (GetProcessTimes(h, &created, &exit, &k, &u)) {
                                c->born = ticks(created);
                                processMetadata(*c);
                                it = cache.emplace(p.pid, std::move(c)).first;
                            }
                        }
                    }
                    if (it != cache.end()) {
                        auto& c = *it->second;
                        p.born = c.born;
                        p.path = c.path;
                        p.user = c.user;
                        p.architecture = c.arch;
                        if (p.group != 0 && (p.pid <= 4 || lower(p.path).find(L"\\windows\\system32\\") !=
                                                               std::wstring::npos))
                            p.group = 2;
                        FILETIME created{}, exit{}, k{}, u{};
                        if (GetProcessTimes(c.handle, &created, &exit, &k, &u) && ticks(created) == c.born) {
                            // Timestamp this PID's actual sample, excluding discovery work
                            // before its first read from the short startup denominator.
                            double sampledAt = clockSeconds();
                            p.cpuTicks = ticks(k) + ticks(u);
                            if (c.lastTime > 0)
                                p.cpu =
                                    cpuPercent(c.lastCpu, p.cpuTicks, sampledAt - c.lastTime, info.logical);
                            c.lastCpu = p.cpuTicks;
                            c.lastTime = sampledAt;
                        }
                        MemoryEx2 pm{};
                        pm.base.cb = sizeof(pm);
                        if (GetProcessMemoryInfo(c.handle, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm),
                                                 sizeof(pm)))
                            p.memory = pm.privateWorkingSet / 1048576.0;
                        DWORD handles = 0;
                        if (GetProcessHandleCount(c.handle, &handles))
                            p.handles = handles;
                        PROCESS_POWER_THROTTLING_STATE throttle{};
                        throttle.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
                        if (GetProcessInformation(c.handle, ProcessPowerThrottling, &throttle,
                                                  sizeof(throttle)))
                            p.efficient = (throttle.ControlMask & throttle.StateMask &
                                           PROCESS_POWER_THROTTLING_EXECUTION_SPEED) != 0 &&
                                          GetPriorityClass(c.handle) == IDLE_PRIORITY_CLASS;
                    }
                    if (p.pid == 0) {
                        p.name = L"System Idle Process";
                        p.group = 2;
                        p.cpu = std::isfinite(s->cpu) ? 100 - s->cpu : unknown;
                        p.memory = 0;
                    }
                    s->processes.push_back(std::move(p));
                } while (Process32NextW(snap.value, &entry));
            else
                s->error = L"Process information is unavailable.";
            for (auto it = cache.begin(); it != cache.end();)
                if (!alive.count(it->first))
                    it = cache.erase(it);
                else
                    ++it;
            std::sort(s->processes.begin(), s->processes.end(),
                      [](const Process& a, const Process& b) { return a.pid < b.pid; });
            auto activity = activityTrace.snapshot();
            s->activity = activity;
            if (!previousActivity || activity->timestamp != previousActivity->timestamp ||
                activity->active != previousActivity->active) {
                activityRates.clear();
                if (previousActivity && activity->active && activity->valid && previousActivity->valid &&
                    activity->epoch == previousActivity->epoch &&
                    activity->timestamp > previousActivity->timestamp) {
                    double seconds = activity->timestamp - previousActivity->timestamp;
                    for (auto& p : s->processes)
                        if (p.born && p.born <= previousActivity->capturedFileTime) {
                            activity::Key key{p.pid, p.born};
                            activity::Bytes before{}, after{};
                            auto old = previousActivity->totals.find(key);
                            if (old != previousActivity->totals.end())
                                before = old->second;
                            auto now = activity->totals.find(key);
                            if (now != activity->totals.end())
                                after = now->second;
                            activityRates[key] = {rate(before.diskRead + before.diskWrite,
                                                       after.diskRead + after.diskWrite, seconds) /
                                                      1048576.0,
                                                  rate(before.networkReceive + before.networkSend,
                                                       after.networkReceive + after.networkSend, seconds) *
                                                      8 / 1000000.0};
                        }
                }
                previousActivity = activity;
            }
            auto disk = std::make_shared<process_disk::Snapshot>(diskSampler.capture());
            s->processDisk = disk;
            for (auto& p : s->processes) {
                auto i = activityRates.find({p.pid, p.born});
                if (i != activityRates.end()) {
                    if (!disk->valid)
                        p.disk = i->second.first;
                    p.network = i->second.second;
                }
                if (disk->valid)
                    p.disk = process_disk::megabytesPerSecond(previousDisk, *disk, p.pid, p.born);
            }
            previousDisk = *disk;
            append(cpuHistory, begin, s->cpu / 100.0);
            append(memoryHistory, begin,
                   s->memory.valid ? 1.0 - static_cast<double>(s->memory.available) / s->memory.total
                                   : unknown);
            s->cpuHistory = cpuHistory;
            s->memoryHistory = memoryHistory;
            s->ready = true;
            s->collectionMs = (clockSeconds() - begin) * 1000;
            publish(s);
            // One real short interval seeds CPU output; subsequent refreshes use
            // the selected cadence. Every PID gets this long after its first read.
            unsigned pause =
                rev == 1
                    ? startupCpuIntervalMs
                    : static_cast<unsigned>(std::max(1.0, interval.load() - (clockSeconds() - begin) * 1000));
            if (wait(pause))
                break;
        }
    }
    void runDevices(unsigned delay) {
        if (wait(delay))
            return;
        HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        WSADATA winsock{};
        bool winsockReady = WSAStartup(MAKEWORD(2, 2), &winsock) == 0;
        Query query;
        Counter read, write, idle, response, engines, dedicated, shared, cpuTotal, actualFrequency,
            performance, logical;
        double nominalGHz = cpuInfo().baseGHz;
        if (query.handle) {
            read.add(query.handle, L"\\PhysicalDisk(*)\\Disk Read Bytes/sec");
            write.add(query.handle, L"\\PhysicalDisk(*)\\Disk Write Bytes/sec");
            idle.add(query.handle, L"\\PhysicalDisk(*)\\% Idle Time");
            response.add(query.handle, L"\\PhysicalDisk(*)\\Avg. Disk sec/Transfer");
            engines.add(query.handle, L"\\GPU Engine(*)\\Utilization Percentage");
            dedicated.add(query.handle, L"\\GPU Adapter Memory(*)\\Dedicated Usage");
            shared.add(query.handle, L"\\GPU Adapter Memory(*)\\Shared Usage");
            cpuTotal.add(query.handle, L"\\Processor Information(_Total)\\% Processor Time");
            actualFrequency.add(query.handle, L"\\Processor Information(_Total)\\Actual Frequency");
            performance.add(query.handle, L"\\Processor Information(_Total)\\% Processor Performance");
            logical.add(query.handle, L"\\Processor Information(*)\\% Processor Time");
        }
        // Prime rate counters before inventory so discovery time contributes to
        // a genuine first measurement rather than preceding a full-second wait.
        double primedAt = 0;
        if (query.handle && PdhCollectQueryData(query.handle) == ERROR_SUCCESS)
            primedAt = clockSeconds();
        std::map<std::wstring, Device> known;
        std::map<uint64_t, std::pair<uint64_t, uint64_t>> previousNet;
        std::vector<Device> gpus;
        double lastNetwork = 0, lastInventory = -100;
        uint64_t rev = 0;
        DeviceSnapshot extra;
        std::vector<Point> cpuHistory;
        std::map<std::wstring, std::vector<Point>> logicalHistory;
        std::map<uint64_t, Addresses> addresses;
        std::map<std::wstring, DriverInfo> drivers;
        StorageInventory storage;
        cpu_parking::Sampler parkingSampler;
        while (!stopping) {
            double begin = clockSeconds(), cycleStarted = begin;
            auto s = std::make_shared<DeviceSnapshot>();
            s->timestamp = begin;
            s->revision = ++rev;
            bool refreshInventory = begin - lastInventory >= 10;
            if (refreshInventory) {
                gpus = gpuInventory();
                if (stopping)
                    break;
                extra = DeviceSnapshot{};
                inventoryExtras(extra);
                if (stopping)
                    break;
                extra.memoryHardware = memoryFirmware();
                if (stopping)
                    break;
                addresses = adapterAddresses();
                if (stopping)
                    break;
                drivers = driverVersions();
                if (stopping)
                    break;
                storage = storageInventory(stopping);
                lastInventory = begin;
            }
            s->services = extra.services;
            s->users = extra.users;
            s->startup = extra.startup;
            s->memoryHardware = extra.memoryHardware;
            if (rev == 1 && primedAt > 0) {
                double remaining = startupCpuIntervalMs - (clockSeconds() - primedAt) * 1000;
                if (remaining > 0 && wait(static_cast<unsigned>(std::ceil(remaining))))
                    break;
            }
            begin = clockSeconds();
            s->timestamp = begin;
            bool collected = query.handle && PdhCollectQueryData(query.handle) == ERROR_SUCCESS;
            if (stopping)
                break;
            auto parking = parkingSampler.capture();
            s->cpu = collected ? cpuTotal.scalar() : unknown;
            append(cpuHistory, begin, s->cpu / 100);
            s->cpuHistory = cpuHistory;
            if (collected) {
                double actual = actualFrequency.scalar(), ratio = performance.scalar();
                s->frequencyGHz = good(actual) && actual > 0        ? actual / 1000
                                  : good(ratio) && good(nominalGHz) ? nominalGHz * ratio / 100
                                                                    : unknown;
                auto values = logical.values();
                for (unsigned group = 0; group < GetActiveProcessorGroupCount(); group++)
                    for (unsigned index = 0; index < GetActiveProcessorCount(group); index++) {
                        auto key = std::to_wstring(group) + L"," + std::to_wstring(index);
                        auto found = values.find(key);
                        double value = found == values.end() ? unknown : found->second;
                        auto& h = logicalHistory[key];
                        append(h, begin, std::clamp(value / 100, 0.0, 1.0));
                        s->logicalCpu.push_back({group, index, value, h});
                        auto state = parking.find({group, index});
                        if (state != parking.end()) {
                            s->logicalCpu.back().parkingKnown = true;
                            s->logicalCpu.back().parked = state->second;
                        }
                    }
                std::sort(s->logicalCpu.begin(), s->logicalCpu.end(),
                          [](const LogicalCpu& a, const LogicalCpu& b) {
                              return std::make_pair(a.group, a.index) < std::make_pair(b.group, b.index);
                          });
            }
            auto r = collected ? read.values() : std::map<std::wstring, double>{};
            auto w = collected ? write.values() : std::map<std::wstring, double>{};
            auto id = collected ? idle.values() : std::map<std::wstring, double>{};
            auto resp = collected ? response.values() : std::map<std::wstring, double>{};
            std::set<std::wstring> diskNames;
            for (auto& a : r)
                diskNames.insert(a.first);
            for (auto& a : w)
                diskNames.insert(a.first);
            for (auto& a : id)
                diskNames.insert(a.first);
            // Discovery can use raw instance names even before rate counters have a second sample.
            if (diskNames.empty() && idle.handle) {
                DWORD bytes = 0, count = 0;
                if (PdhGetRawCounterArrayW(idle.handle, &bytes, &count, nullptr) ==
                        static_cast<PDH_STATUS>(PDH_MORE_DATA) &&
                    bytes && bytes < 16 * 1024 * 1024) {
                    std::vector<unsigned char> b(bytes);
                    auto* items = reinterpret_cast<PDH_RAW_COUNTER_ITEM_W*>(b.data());
                    if (PdhGetRawCounterArrayW(idle.handle, &bytes, &count, items) == ERROR_SUCCESS)
                        for (DWORD i = 0; i < count; i++)
                            if (items[i].szName)
                                diskNames.insert(items[i].szName);
                }
            }
            for (auto& name : diskNames) {
                if (stopping)
                    break;
                if (name == L"_Total")
                    continue;
                wchar_t* end = nullptr;
                long index = wcstol(name.c_str(), &end, 10);
                if (end == name.c_str() || index < 0)
                    continue;
                std::wstring key = L"disk:" + std::to_wstring(index);
                Device d;
                if (known.count(key))
                    d = known[key];
                else {
                    d.key = key;
                    d.kind = Kind::Disk;
                    d.name = L"Disk " + std::to_wstring(index);
                    if (*end == L' ' && end[1])
                        d.name += L" (" + std::wstring(end + 1) + L")";
                    d.type = L"Disk";
                    diskInfo(d, static_cast<int>(index));
                }
                if (refreshInventory && known.count(key)) {
                    Device refreshed = d;
                    diskInfo(refreshed, static_cast<int>(index));
                    if (!refreshed.physicalId.empty() && refreshed.physicalId != d.physicalId)
                        refreshed.history.clear();
                    d = std::move(refreshed);
                }
                d.name = L"Disk " + std::to_wstring(index);
                if (*end == L' ' && end[1])
                    d.name += L" (" + std::wstring(end + 1) + L")";
                d.storage = diskDetails(static_cast<unsigned>(index), storage);
                d.read = r.count(name) ? r[name] : unknown;
                d.write = w.count(name) ? w[name] : unknown;
                d.usage = id.count(name) ? std::clamp(100.0 - id[name], 0.0, 100.0) : unknown;
                d.responseMs = resp.count(name) ? resp[name] * 1000 : unknown;
                if (good(d.usage))
                    s->diskUsage = good(s->diskUsage) ? std::max(s->diskUsage, d.usage) : d.usage;
                append(d.history, begin, d.usage / 100);
                known[key] = d;
                s->devices.push_back(std::move(d));
            }
            PMIB_IF_TABLE2 table = nullptr;
            std::set<uint64_t> netAlive;
            if (GetIfTable2(&table) == NO_ERROR) {
                for (ULONG i = 0; i < table->NumEntries; i++) {
                    auto& row = table->Table[i];
                    if ((row.Type != IF_TYPE_ETHERNET_CSMACD && row.Type != IF_TYPE_IEEE80211) ||
                        row.OperStatus != IfOperStatusUp ||
                        !row.InterfaceAndOperStatusFlags.HardwareInterface ||
                        row.InterfaceAndOperStatusFlags.FilterInterface)
                        continue;
                    uint64_t luid = row.InterfaceLuid.Value;
                    netAlive.insert(luid);
                    std::wstring key = L"net:" + std::to_wstring(luid);
                    Device d;
                    if (known.count(key))
                        d = known[key];
                    d.key = key;
                    d.kind = Kind::Network;
                    d.color = 0xD9AB75;
                    d.name = row.Alias;
                    d.description = row.Description;
                    d.type = row.Type == IF_TYPE_IEEE80211 ? L"Wi-Fi" : L"Ethernet";
                    d.linkSpeed = std::max(row.ReceiveLinkSpeed, row.TransmitLinkSpeed);
                    d.read = d.write = unknown;
                    auto address = addresses.find(luid);
                    if (address != addresses.end()) {
                        d.ipv4 = address->second.v4;
                        d.ipv6 = address->second.v6;
                    }
                    auto it = previousNet.find(luid);
                    if (it != previousNet.end() && lastNetwork > 0) {
                        d.read = rate(it->second.first, row.InOctets, begin - lastNetwork);
                        d.write = rate(it->second.second, row.OutOctets, begin - lastNetwork);
                    }
                    previousNet[luid] = {row.InOctets, row.OutOctets};
                    d.usage = good(d.read) && good(d.write) && d.linkSpeed
                                  ? std::clamp((d.read + d.write) * 800.0 / d.linkSpeed, 0.0, 100.0)
                                  : unknown;
                    if (good(d.usage))
                        s->networkUsage =
                            good(s->networkUsage) ? std::max(s->networkUsage, d.usage) : d.usage;
                    append(d.history, begin,
                           good(d.read) && good(d.write) ? (d.read + d.write) * 8 : unknown);
                    known[key] = d;
                    s->devices.push_back(std::move(d));
                }
                FreeMibTable(table);
            }
            for (auto it = previousNet.begin(); it != previousNet.end();)
                if (!netAlive.count(it->first))
                    it = previousNet.erase(it);
                else
                    ++it;
            lastNetwork = begin;
            std::vector<Engine> samples;
            if (collected)
                for (auto& v : engines.values()) {
                    auto pos = v.first.find(L"phys_");
                    unsigned physical = 0, node = 0;
                    if (pos == v.first.npos ||
                        swscanf(v.first.c_str() + pos, L"phys_%u_eng_%u", &physical, &node) != 2)
                        continue;
                    samples.push_back(
                        {instanceLuid(v.first), physical, node, 0, v.second, gpuEngineKind(v.first)});
                }
            auto dm = collected ? dedicated.values() : std::map<std::wstring, double>{};
            auto sm = collected ? shared.values() : std::map<std::wstring, double>{};
            std::vector<Device> physicalGpus;
            std::map<std::wstring, size_t> locations;
            auto supported = [&](const Device& d) {
                return std::any_of(dm.begin(), dm.end(),
                                   [&](const auto& v) { return instanceLuid(v.first) == d.adapter; }) ||
                       std::any_of(samples.begin(), samples.end(),
                                   [&](const Engine& e) { return e.adapter == d.adapter; });
            };
            for (auto d : gpus) {
                std::wstring key = d.physicalId.empty() ? d.key : L"gpu-pci:" + d.physicalId;
                auto it = locations.find(key);
                if (it == locations.end()) {
                    d.key = key;
                    locations[key] = physicalGpus.size();
                    physicalGpus.push_back(d);
                } else if (supported(d) && !supported(physicalGpus[it->second])) {
                    d.key = key;
                    physicalGpus[it->second] = d;
                }
            }
            for (size_t i = 0; i < physicalGpus.size(); i++) {
                auto d = physicalGpus[i];
                d.name = L"GPU " + std::to_wstring(i);
                if (drivers.count(d.physicalId)) {
                    d.driverVersion = drivers[d.physicalId].version;
                    d.driverDate = drivers[d.physicalId].date;
                }
                unsigned bus = 0, dev = 0, fun = 0;
                if (swscanf(d.physicalId.c_str(), L"%u:%u:%u", &bus, &dev, &fun) == 3)
                    d.physicalLocation = L"PCIe " + std::to_wstring(bus) + L", Dev " + std::to_wstring(dev) +
                                         L", Fun " + std::to_wstring(fun);
                if (known.count(d.key))
                    retainGpuHistory(d, known[d.key]);
                std::wstring luid = d.adapter;
                d.usage = gpuUsage(samples, luid);
                for (size_t engine = 0; engine < d.gpuEngines.size(); engine++) {
                    auto& series = d.gpuEngines[engine];
                    series.usage = gpuEngineUsage(samples, luid, static_cast<GpuEngineKind>(engine));
                    append(series.history, begin, series.usage / 100);
                }
                for (auto& v : dm)
                    if (instanceLuid(v.first) == luid)
                        d.dedicatedUsed = good(d.dedicatedUsed) ? d.dedicatedUsed + v.second : v.second;
                for (auto& v : sm)
                    if (instanceLuid(v.first) == luid)
                        d.sharedUsed = good(d.sharedUsed) ? d.sharedUsed + v.second : v.second;
                append(d.dedicatedHistory, begin, d.dedicatedUsed);
                append(d.sharedHistory, begin, d.sharedUsed);
                d.temperature = temperature(L"gpu:" + d.adapter);
                append(d.history, begin, d.usage / 100);
                known[d.key] = d;
                s->devices.push_back(std::move(d));
            }
            std::set<std::wstring> live;
            for (auto& d : s->devices)
                live.insert(d.key);
            for (auto it = known.begin(); it != known.end();)
                if (!live.count(it->first))
                    it = known.erase(it);
                else
                    ++it;
            s->ready = true;
            s->collectionMs = (clockSeconds() - cycleStarted) * 1000;
            publish(s);
            bool startupRetry = rev == 1 && (!good(s->cpu) || !good(s->frequencyGHz) ||
                                             std::any_of(s->logicalCpu.begin(), s->logicalCpu.end(),
                                                         [](auto& cpu) { return !good(cpu.usage); }));
            unsigned pause =
                startupRetry
                    ? startupCpuIntervalMs
                    : static_cast<unsigned>(std::max(1.0, interval.load() - (clockSeconds() - begin) * 1000));
            if (wait(pause))
                break;
        }
        if (winsockReady)
            WSACleanup();
        if (SUCCEEDED(com))
            CoUninitialize();
    }
};
Collector::Collector() : impl(std::make_unique<Impl>()) {}
Collector::~Collector() {
    requestStop();
    join();
    if (impl->stopEvent)
        CloseHandle(impl->stopEvent);
}
void Collector::start(Notify notify, unsigned delayMs) {
    if (impl->stopEvent)
        return;
    impl->notify = std::move(notify);
    impl->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl->stopEvent)
        return;
    impl->coreThread = std::thread([this, delayMs] {
        try {
            impl->runCore(delayMs);
        } catch (...) {
            auto s = std::make_shared<CoreSnapshot>();
            s->error = L"Process collection failed.";
            impl->publish(s);
        }
    });
    impl->deviceThread = std::thread([this, delayMs] {
        try {
            impl->runDevices(delayMs);
        } catch (...) {
            auto s = std::make_shared<DeviceSnapshot>();
            s->error = L"Device collection failed.";
            impl->publish(s);
        }
    });
}
void Collector::requestStop() {
    impl->stopping = true;
    impl->activityTrace.requestStop();
    if (impl->stopEvent)
        SetEvent(impl->stopEvent);
}
void Collector::setUpdateInterval(unsigned ms) {
    if (ms != 500 && ms != 1000 && ms != 1500)
        ms = 1000;
    impl->interval = ms;
    impl->activityTrace.setUpdateInterval(ms);
}
void Collector::join() {
    if (impl->coreThread.joinable())
        impl->coreThread.join();
    if (impl->deviceThread.joinable())
        impl->deviceThread.join();
    impl->activityTrace.join();
}
std::shared_ptr<const CoreSnapshot> Collector::core() const {
    std::lock_guard lock(impl->mutex);
    return impl->core;
}
std::shared_ptr<const DeviceSnapshot> Collector::devices() const {
    std::lock_guard lock(impl->mutex);
    return impl->devices;
}
double Collector::startedAt() const {
    return impl->started.load();
}
} // namespace telemetry

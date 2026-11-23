#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include <array>
#include <optional>
#include "activity.hpp"
#include "process_disk.hpp"

namespace telemetry {
inline constexpr double unknown = std::numeric_limits<double>::quiet_NaN();
struct Point {
    double time = 0, value = unknown;
};
struct Process {
    DWORD pid = 0, parent = 0, session = 0, threads = 0, handles = 0;
    uint64_t born = 0, cpuTicks = 0;
    std::wstring name, path, user, architecture;
    int group = 1, icon = 0;
    double cpu = unknown, memory = unknown, disk = unknown, network = unknown;
    bool efficient = false;
    int children = 0;
};
struct ProcessGroup {
    std::wstring key;
    Process summary;
    std::vector<Process> members;
};
std::vector<ProcessGroup> groupProcesses(const std::vector<Process>& processes);
struct Memory {
    bool valid = false, performanceValid = false;
    uint64_t total = 0, available = 0, installed = 0, commit = 0, commitLimit = 0, cache = 0, paged = 0,
             nonpaged = 0;
};
struct CpuInfo {
    std::wstring name;
    unsigned logical = 0, cores = 0, sockets = 0;
    uint64_t l1 = 0, l2 = 0, l3 = 0;
    double baseGHz = unknown;
    bool virtualization = false;
};
struct CoreSnapshot {
    bool ready = false;
    uint64_t revision = 0, upMilliseconds = 0;
    double timestamp = 0, collectionMs = 0, cpu = unknown;
    Memory memory;
    CpuInfo info;
    unsigned totalProcesses = 0, totalThreads = 0, totalHandles = 0;
    std::vector<Process> processes;
    std::vector<Point> cpuHistory, memoryHistory;
    std::wstring error;
    std::shared_ptr<const activity::Frame> activity;
    std::shared_ptr<const process_disk::Snapshot> processDisk;
};
enum class Kind { Disk, Network, Gpu };
enum class GpuEngineKind { ThreeD, Copy, VideoEncode, VideoDecode, Other };
struct GpuSeries {
    double usage = unknown;
    std::vector<Point> history;
};
struct StorageVolume {
    std::wstring key;
    std::vector<unsigned> disks;
    double formattedCapacity = unknown;
};
struct StorageInventory {
    std::vector<StorageVolume> volumes;
    std::wstring windowsVolume;
    std::vector<std::wstring> pageFileVolumes;
    bool pageFilesComplete = false, volumesComplete = false;
};
struct DiskDetails {
    double formattedCapacity = unknown;
    std::optional<bool> systemDisk, pageFile;
};
DiskDetails diskDetails(unsigned disk, const StorageInventory& inventory);
double formattedBytes(uint64_t units, unsigned sectorsPerUnit, unsigned bytesPerSector);
std::vector<unsigned> volumeDisks(const std::vector<unsigned char>& bytes);
struct Device {
    std::wstring key, name, description, type, physicalId, adapter, ipv4, ipv6, driverVersion, driverDate,
        physicalLocation;
    Kind kind = Kind::Disk;
    unsigned color = 0x87C879;
    unsigned adapterFlags = 0;
    double usage = unknown, read = unknown, write = unknown, responseMs = unknown, temperature = unknown;
    double dedicatedUsed = unknown, sharedUsed = unknown;
    uint64_t capacity = 0, sharedCapacity = 0, linkSpeed = 0;
    bool connected = true;
    std::vector<Point> history;
    std::array<GpuSeries, 4> gpuEngines;
    std::vector<Point> dedicatedHistory, sharedHistory;
    DiskDetails storage;
};
struct Service {
    std::wstring name, description, status;
    DWORD pid = 0;
};
struct User {
    std::wstring key, name, status;
    DWORD session = 0;
};
struct Startup {
    std::wstring key, name, command, status;
};
struct MemoryModule {
    std::wstring locator, bank, type, formFactor;
    uint64_t capacity = 0;
    unsigned speed = 0;
    bool populated = false, capacityKnown = false;
};
struct MemoryHardware {
    bool valid = false;
    unsigned slots = 0, used = 0, speed = 0;
    std::wstring type, formFactor, occupied;
    std::vector<MemoryModule> modules;
};
MemoryHardware parseMemoryFirmware(const std::vector<unsigned char>& raw);
struct LogicalCpu {
    unsigned group = 0, index = 0;
    double usage = unknown;
    std::vector<Point> history;
    bool parkingKnown = false, parked = false;
};
struct DeviceSnapshot {
    bool ready = false;
    uint64_t revision = 0;
    double timestamp = 0, collectionMs = 0, networkUsage = unknown, diskUsage = unknown, cpu = unknown;
    std::vector<Point> cpuHistory;
    double frequencyGHz = unknown;
    std::vector<LogicalCpu> logicalCpu;
    MemoryHardware memoryHardware;
    std::vector<Device> devices;
    std::vector<Service> services;
    std::vector<User> users;
    std::vector<Startup> startup;
    std::wstring error;
};
// Pure calculations also exercised by tests, including reset and multi-engine cases.
double rate(uint64_t previous, uint64_t current, double seconds);
double cpuPercent(uint64_t previous, uint64_t current, double seconds, unsigned logical);
bool metricLess(double a, double b, bool ascending);
struct Engine {
    std::wstring adapter;
    unsigned physical = 0, node = 0;
    DWORD pid = 0;
    double usage = unknown;
    GpuEngineKind kind = GpuEngineKind::Other;
};
double gpuUsage(const std::vector<Engine>& engines, const std::wstring& adapter);
GpuEngineKind gpuEngineKind(const std::wstring& instance);
double gpuEngineUsage(const std::vector<Engine>& engines, const std::wstring& adapter, GpuEngineKind kind);
void retainGpuHistory(Device& current, const Device& previous);

// Constructor does no discovery, opens no query and starts no thread.
// start() is called only after the first complete UI draw has been presented.
class Collector {
  public:
    using Notify = std::function<void()>;
    Collector();
    ~Collector();
    Collector(const Collector&) = delete;
    Collector& operator=(const Collector&) = delete;
    void start(Notify notify, unsigned delayMs = 0);
    void setUpdateInterval(unsigned milliseconds);
    void requestStop();
    void join();
    std::shared_ptr<const CoreSnapshot> core() const;
    std::shared_ptr<const DeviceSnapshot> devices() const;
    double startedAt() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
double clockSeconds();
} // namespace telemetry

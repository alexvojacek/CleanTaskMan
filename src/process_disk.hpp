#pragma once
#include <windows.h>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <vector>
namespace process_disk {
struct Counters {
    uint64_t read = 0, write = 0, reads = 0, writes = 0, flushes = 0;
};
struct Record {
    uint64_t born = 0;
    Counters counters;
};
struct Snapshot {
    bool valid = false;
    DWORD status = 0;
    double timestamp = 0;
    std::map<DWORD, Record> records;
};
// Windows x64 SystemFullProcessInformation: fixed process header, extended
// thread array, then the process-accounted disk counter extension.
Snapshot parse(std::span<const unsigned char> data);
// A fresh baseline is required after failure, PID reuse or counter reset.
double megabytesPerSecond(const Snapshot& before, const Snapshot& after, DWORD pid, uint64_t born);
class Sampler {
  public:
    Sampler();
    ~Sampler();
    Snapshot capture();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace process_disk

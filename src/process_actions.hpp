#pragma once
#include <windows.h>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
namespace actions {
struct Identity {
    DWORD pid = 0;
    uint64_t born = 0;
};
struct Result {
    bool success = false;
    DWORD error = 0;
    std::wstring detail;
};
class Processes {
  public:
    Result end(const Identity& identity);
    Result efficiency(const Identity& identity, bool enabled);
    Result location(const Identity& identity);
    Result properties(const Identity& identity);
    static std::wstring path(const Identity& identity, DWORD* error = nullptr);

  private:
    struct Original {
        DWORD priority;
        PROCESS_POWER_THROTTLING_STATE power;
    };
    std::map<std::pair<DWORD, uint64_t>, Original> originals;
};
} // namespace actions

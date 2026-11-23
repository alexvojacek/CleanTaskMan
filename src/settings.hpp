#pragma once
#include <windows.h>
#include <map>
#include <string>
#include <memory>
#include <optional>

namespace settings {
using Values = std::map<std::string, std::string>;
std::wstring executableFolder();
std::wstring filePath();
Values parse(const std::string& text);
std::string serialize(const Values& values);
Values load(const std::wstring& path);
bool save(const std::wstring& path, const Values& values, DWORD* error = nullptr);
struct Placement {
    int left = 0, top = 0, width = 1180, height = 780, dpi = 96;
    bool maximized = false;
    bool operator==(const Placement&) const = default;
};
std::wstring placementPath();
Values placementValues(const Placement& position, const std::string& revision);
std::optional<Placement> readPlacement(const Values& values);
bool overlayPlacement(Values& preferences, const Values& placement);
bool saveCompact(const std::wstring& path, const Values& values, DWORD* error = nullptr);
int integer(const Values& values, const std::string& key, int fallback, int low, int high);
std::string get(const Values& values, const std::string& key, const std::string& fallback = "");
std::string utf8(const std::wstring& text);
std::wstring wide(const std::string& text);
class Writer {
  public:
    Writer();
    ~Writer();
    void queue(const std::wstring& path, Values values);
    void queueCompact(const std::wstring& path, Values values);
    bool flush();
    DWORD error() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace settings

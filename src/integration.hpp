#pragma once
#include <windows.h>
#include <string>
#include <memory>
#include <optional>
namespace integration {
std::wstring executablePath();
std::wstring instanceClass();
UINT restoreMessage();
class SettingsHandoff {
  public:
    ~SettingsHandoff();
    static std::unique_ptr<SettingsHandoff> create(const std::string& data);
    const std::wstring& token() const {
        return nonce;
    }
    bool waitReady(unsigned milliseconds) const;
    void commit() const;
    static std::optional<std::string> receive(const std::wstring& nonce);

  private:
    HANDLE mapping = nullptr, ready = nullptr, committed = nullptr;
    std::wstring nonce;
};
bool restoreExisting(const std::wstring& className, UINT message, unsigned attempts = 1);
enum class ReplacementKind { Default, Owned, Other, Unavailable };
struct ReplacementState {
    ReplacementKind kind = ReplacementKind::Unavailable;
    DWORD error = 0;
    std::wstring debugger;
};
class TaskManager {
  public:
    explicit TaskManager(
        std::wstring executable = executablePath(), HKEY hive = HKEY_LOCAL_MACHINE,
        std::wstring key =
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\taskmgr.exe");
    ReplacementState state(const std::wstring& previousOwned = L"") const;
    DWORD set(bool enabled, const std::wstring& previousOwned = L"") const;
    std::wstring command() const;

  private:
    std::wstring executable, key;
    HKEY hive;
};
} // namespace integration

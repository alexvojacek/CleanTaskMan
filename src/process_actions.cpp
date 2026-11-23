#include "process_actions.hpp"
#include <shellapi.h>
#include <shlobj.h>
#include <vector>
namespace actions {
namespace {
uint64_t ticks(FILETIME f) {
    return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}
struct Handle {
    HANDLE h = nullptr;
    ~Handle() {
        if (h)
            CloseHandle(h);
    }
};
HANDLE validated(const Identity& id, DWORD access, bool change, DWORD& error) {
    if (!id.born || id.pid <= 4 || (change && id.pid == GetCurrentProcessId())) {
        error = ERROR_ACCESS_DENIED;
        return nullptr;
    }
    HANDLE h = OpenProcess(access | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, id.pid);
    if (!h) {
        error = GetLastError();
        return nullptr;
    }
    FILETIME created{}, exit{}, k{}, u{};
    if (!GetProcessTimes(h, &created, &exit, &k, &u) || ticks(created) != id.born ||
        WaitForSingleObject(h, 0) != WAIT_TIMEOUT) {
        error = ERROR_INVALID_HANDLE;
        CloseHandle(h);
        return nullptr;
    }
    if (change) {
        BOOL critical = FALSE;
        if (!IsProcessCritical(h, &critical) || critical) {
            error = ERROR_ACCESS_DENIED;
            CloseHandle(h);
            return nullptr;
        }
    }
    error = ERROR_SUCCESS;
    return h;
}
Result result(bool ok, DWORD code, const wchar_t* what) {
    return {ok, ok ? ERROR_SUCCESS : code, ok ? L"" : what};
}
} // namespace
Result Processes::end(const Identity& id) {
    DWORD error = 0;
    Handle process{validated(id, PROCESS_TERMINATE, true, error)};
    if (!process.h)
        return result(false, error,
                      L"Windows did not allow this process to be ended, or the selected process has exited.");
    if (!TerminateProcess(process.h, 1))
        return result(false, GetLastError(), L"Windows could not end the process.");
    DWORD wait = WaitForSingleObject(process.h, 3000);
    return result(wait == WAIT_OBJECT_0, wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError(),
                  L"The end request was sent, but the process has not exited yet.");
}
Result Processes::efficiency(const Identity& id, bool enabled) {
    DWORD error = 0;
    Handle process{validated(id, PROCESS_SET_INFORMATION, true, error)};
    if (!process.h)
        return result(
            false, error,
            L"Windows did not allow the efficiency policy to be changed, or this process has exited.");
    PROCESS_POWER_THROTTLING_STATE before{};
    before.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    DWORD prior = GetPriorityClass(process.h);
    if (!prior || !GetProcessInformation(process.h, ProcessPowerThrottling, &before, sizeof(before)))
        return result(false, GetLastError(), L"The current process power policy is unavailable.");
    auto key = std::make_pair(id.pid, id.born);
    auto saved = originals.find(key);
    // Restore our change only while the process still has the priority we set.
    DWORD priority =
        enabled ? IDLE_PRIORITY_CLASS
                : (prior != IDLE_PRIORITY_CLASS
                       ? prior
                       : (saved != originals.end() ? saved->second.priority : NORMAL_PRIORITY_CLASS));
    auto desired = before;
    if (enabled) {
        desired.ControlMask |= PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        desired.StateMask |= PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    } else if (saved != originals.end()) {
        auto mask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        desired.ControlMask = (desired.ControlMask & ~mask) | (saved->second.power.ControlMask & mask);
        desired.StateMask = (desired.StateMask & ~mask) | (saved->second.power.StateMask & mask);
    } else {
        desired.ControlMask |= PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        desired.StateMask &= ~PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    }
    if (!SetPriorityClass(process.h, priority))
        return result(false, GetLastError(), L"Windows could not change the process priority.");
    if (!SetProcessInformation(process.h, ProcessPowerThrottling, &desired, sizeof(desired))) {
        DWORD code = GetLastError();
        SetPriorityClass(process.h, prior);
        return result(false, code, L"Windows could not apply the efficiency policy.");
    }
    PROCESS_POWER_THROTTLING_STATE after{};
    after.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    bool valid = GetProcessInformation(process.h, ProcessPowerThrottling, &after, sizeof(after)) &&
                 GetPriorityClass(process.h) == priority &&
                 (after.StateMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) ==
                     (desired.StateMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) &&
                 (after.ControlMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) ==
                     (desired.ControlMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED);
    if (!valid) {
        SetProcessInformation(process.h, ProcessPowerThrottling, &before, sizeof(before));
        SetPriorityClass(process.h, prior);
        return result(false, ERROR_INVALID_DATA,
                      L"The process did not retain the requested efficiency policy.");
    }
    if (enabled && saved == originals.end())
        originals[key] = {prior, before};
    else if (!enabled)
        originals.erase(key);
    return result(true, 0, L"");
}
std::wstring Processes::path(const Identity& id, DWORD* errorOut) {
    DWORD error = 0;
    Handle process{validated(id, 0, false, error)};
    std::wstring out;
    if (process.h) {
        std::vector<wchar_t> b(32768);
        DWORD n = static_cast<DWORD>(b.size());
        if (QueryFullProcessImageNameW(process.h, 0, b.data(), &n))
            out.assign(b.data(), n);
        else
            error = GetLastError();
    }
    if (errorOut)
        *errorOut = error;
    return out;
}
Result Processes::location(const Identity& id) {
    DWORD error = 0;
    auto file = path(id, &error);
    if (file.empty())
        return result(false, error, L"The executable path is unavailable.");
    PIDLIST_ABSOLUTE pidl = nullptr;
    HRESULT hr = SHParseDisplayName(file.c_str(), nullptr, &pidl, 0, nullptr);
    if (SUCCEEDED(hr)) {
        hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        CoTaskMemFree(pidl);
    }
    return result(SUCCEEDED(hr), static_cast<DWORD>(hr), L"Windows could not open the executable location.");
}
Result Processes::properties(const Identity& id) {
    DWORD error = 0;
    auto file = path(id, &error);
    if (file.empty())
        return result(false, error, L"The executable path is unavailable.");
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_INVOKEIDLIST | SEE_MASK_NOASYNC;
    info.lpVerb = L"properties";
    info.lpFile = file.c_str();
    info.nShow = SW_SHOWNORMAL;
    bool ok = ShellExecuteExW(&info) != FALSE;
    DWORD code = ok ? 0 : GetLastError();
    return result(ok, code, L"Windows could not open the file properties.");
}
} // namespace actions

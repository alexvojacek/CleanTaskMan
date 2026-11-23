#include "integration.hpp"
#include <sddl.h>
#include <vector>
#include <cwctype>
#include <cstdint>
#include <objbase.h>
#include <cstring>
namespace integration {
namespace {
struct HandoffData {
    DWORD magic = 0x43544d48, version = 1, length = 0;
    char bytes[262144];
};
bool validNonce(const std::wstring& value) {
    if (value.size() != 38 || value.front() != L'{' || value.back() != L'}')
        return false;
    for (size_t i = 1; i + 1 < value.size(); i++)
        if (!iswxdigit(value[i]) && value[i] != L'-')
            return false;
    return true;
}
std::wstring handoffName(const std::wstring& token, const wchar_t* suffix) {
    return L"Local\\CleanTaskMan.Settings." + token + suffix;
}
} // namespace
SettingsHandoff::~SettingsHandoff() {
    if (mapping)
        CloseHandle(mapping);
    if (ready)
        CloseHandle(ready);
    if (committed)
        CloseHandle(committed);
}
std::unique_ptr<SettingsHandoff> SettingsHandoff::create(const std::string& text) {
    if (text.size() > sizeof(HandoffData::bytes))
        return {};
    auto result = std::make_unique<SettingsHandoff>();
    GUID id{};
    if (FAILED(CoCreateGuid(&id)))
        return {};
    wchar_t token[40]{};
    StringFromGUID2(id, token, 40);
    result->nonce = token;
    result->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                         sizeof(HandoffData), handoffName(token, L".Data").c_str());
    result->ready = CreateEventW(nullptr, TRUE, FALSE, handoffName(token, L".Ready").c_str());
    result->committed = CreateEventW(nullptr, TRUE, FALSE, handoffName(token, L".Commit").c_str());
    if (!result->mapping || !result->ready || !result->committed)
        return {};
    auto* data =
        static_cast<HandoffData*>(MapViewOfFile(result->mapping, FILE_MAP_WRITE, 0, 0, sizeof(HandoffData)));
    if (!data)
        return {};
    data->magic = 0x43544d48;
    data->version = 1;
    data->length = static_cast<DWORD>(text.size());
    memcpy(data->bytes, text.data(), text.size());
    UnmapViewOfFile(data);
    return result;
}
bool SettingsHandoff::waitReady(unsigned ms) const {
    return WaitForSingleObject(ready, ms) == WAIT_OBJECT_0;
}
void SettingsHandoff::commit() const {
    SetEvent(committed);
}
std::optional<std::string> SettingsHandoff::receive(const std::wstring& token) {
    if (!validNonce(token))
        return std::nullopt;
    SettingsHandoff handles;
    handles.mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, handoffName(token, L".Data").c_str());
    handles.ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, handoffName(token, L".Ready").c_str());
    handles.committed = OpenEventW(SYNCHRONIZE, FALSE, handoffName(token, L".Commit").c_str());
    if (!handles.mapping || !handles.ready || !handles.committed)
        return std::nullopt;
    auto* data = static_cast<const HandoffData*>(
        MapViewOfFile(handles.mapping, FILE_MAP_READ, 0, 0, sizeof(HandoffData)));
    if (!data)
        return std::nullopt;
    std::optional<std::string> result;
    if (data->magic == 0x43544d48 && data->version == 1 && data->length <= sizeof(data->bytes))
        result = std::string(data->bytes, data->length);
    UnmapViewOfFile(data);
    if (!result)
        return std::nullopt;
    SetEvent(handles.ready);
    if (WaitForSingleObject(handles.committed, 6000) != WAIT_OBJECT_0)
        return std::nullopt;
    return result;
}
std::wstring executablePath() {
    std::wstring path(32768, L'\0');
    DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(size);
    return path;
}
std::wstring instanceClass() {
    auto path = executablePath();
    std::wstring longPath(32768, L'\0');
    DWORD length = GetLongPathNameW(path.c_str(), longPath.data(), static_cast<DWORD>(longPath.size()));
    if (length && length < longPath.size()) {
        longPath.resize(length);
        path = std::move(longPath);
    }
    for (auto& c : path)
        c = static_cast<wchar_t>(towlower(c));
    HANDLE token = nullptr;
    std::wstring identity;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        DWORD bytes = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
        std::vector<unsigned char> buffer(bytes);
        if (bytes && GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes)) {
            LPWSTR sid = nullptr;
            if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid)) {
                identity = sid;
                LocalFree(sid);
            }
        }
        CloseHandle(token);
    }
    uint64_t hash = 14695981039346656037ull;
    for (wchar_t c : path + L"|" + identity) {
        hash ^= static_cast<uint16_t>(c);
        hash *= 1099511628211ull;
    }
    return L"CleanTaskMan.Native.UI." + std::to_wstring(hash);
}
UINT restoreMessage() {
    return RegisterWindowMessageW(L"CleanTaskMan.RestoreWindow.1");
}
bool restoreExisting(const std::wstring& className, UINT message, unsigned attempts) {
    if (!message)
        return false;
    for (unsigned i = 0; i < attempts; i++) {
        if (HWND window = FindWindowW(className.c_str(), nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            AllowSetForegroundWindow(pid);
            DWORD_PTR accepted = 0;
            if (SendMessageTimeoutW(window, message, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &accepted) &&
                accepted == 1)
                return true;
        }
        if (i + 1 < attempts)
            Sleep(20);
    }
    return false;
}
TaskManager::TaskManager(std::wstring exe, HKEY root, std::wstring path)
    : executable(std::move(exe)), key(std::move(path)), hive(root) {}
std::wstring TaskManager::command() const {
    return L"\"" + executable + L"\" --task-manager-replacement";
}
ReplacementState TaskManager::state(const std::wstring& previous) const {
    HKEY opened = nullptr;
    auto error = RegOpenKeyExW(hive, key.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &opened);
    if (error == ERROR_FILE_NOT_FOUND)
        return {ReplacementKind::Default, 0, L""};
    if (error)
        return {ReplacementKind::Unavailable, static_cast<DWORD>(error), L""};
    DWORD bytes = 0, type = 0;
    error = RegQueryValueExW(opened, L"Debugger", nullptr, &type, nullptr, &bytes);
    if (error == ERROR_FILE_NOT_FOUND) {
        RegCloseKey(opened);
        return {ReplacementKind::Default, 0, L""};
    }
    if (error || bytes > 65536 || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        RegCloseKey(opened);
        return {ReplacementKind::Unavailable, error ? static_cast<DWORD>(error) : ERROR_INVALID_DATA, L""};
    }
    std::vector<wchar_t> text(bytes / sizeof(wchar_t) + 1, L'\0');
    error =
        RegQueryValueExW(opened, L"Debugger", nullptr, &type, reinterpret_cast<BYTE*>(text.data()), &bytes);
    RegCloseKey(opened);
    if (error)
        return {ReplacementKind::Unavailable, static_cast<DWORD>(error), L""};
    std::wstring value(text.data());
    bool owned = _wcsicmp(value.c_str(), command().c_str()) == 0 ||
                 (!previous.empty() && _wcsicmp(value.c_str(), previous.c_str()) == 0);
    return {owned ? ReplacementKind::Owned : ReplacementKind::Other, 0, value};
}
DWORD TaskManager::set(bool enabled, const std::wstring& previous) const {
    auto before = state(previous);
    if (before.kind == ReplacementKind::Unavailable)
        return before.error;
    if (before.kind == ReplacementKind::Other)
        return ERROR_ALREADY_EXISTS;
    if (!enabled && before.kind == ReplacementKind::Default)
        return ERROR_SUCCESS;
    HKEY opened = nullptr;
    LSTATUS error;
    if (enabled)
        error = RegCreateKeyExW(hive, key.c_str(), 0, nullptr, 0,
                                KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &opened, nullptr);
    else
        error =
            RegOpenKeyExW(hive, key.c_str(), 0, KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &opened);
    if (error)
        return static_cast<DWORD>(error);
    // Re-read through the write handle so a foreign replacement is preserved.
    DWORD bytes = 0, type = 0;
    error = RegQueryValueExW(opened, L"Debugger", nullptr, &type, nullptr, &bytes);
    bool matches = error == ERROR_FILE_NOT_FOUND && before.kind == ReplacementKind::Default;
    if (error == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && bytes <= 65536 &&
        before.kind == ReplacementKind::Owned) {
        std::vector<wchar_t> text(bytes / sizeof(wchar_t) + 1, L'\0');
        if (RegQueryValueExW(opened, L"Debugger", nullptr, nullptr, reinterpret_cast<BYTE*>(text.data()),
                             &bytes) == ERROR_SUCCESS) {
            std::wstring value(text.data());
            matches = value == before.debugger;
        }
    }
    if (!matches) {
        RegCloseKey(opened);
        return ERROR_ALREADY_EXISTS;
    }
    auto value = command();
    error = enabled
                ? RegSetValueExW(opened, L"Debugger", 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                 static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)))
                : RegDeleteValueW(opened, L"Debugger");
    RegCloseKey(opened);
    return error == ERROR_FILE_NOT_FOUND && !enabled ? ERROR_SUCCESS : static_cast<DWORD>(error);
}
} // namespace integration

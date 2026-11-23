#include "settings.hpp"
#include <algorithm>
#include <charconv>
#include <sstream>
#include <vector>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>

namespace settings {
std::string utf8(const std::wstring& s) {
    int n =
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    if (n)
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr,
                            nullptr);
    return out;
}
std::wstring wide(const std::string& s) {
    int n =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    if (n)
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(),
                            n);
    return out;
}
std::wstring executableFolder() {
    std::vector<wchar_t> b(32768);
    DWORD n = GetModuleFileNameW(nullptr, b.data(), static_cast<DWORD>(b.size()));
    std::wstring s(b.data(), n);
    auto p = s.find_last_of(L"\\/");
    return p == s.npos ? L"." : s.substr(0, p);
}
std::wstring filePath() {
    return executableFolder() + L"\\CleanTaskMan.ini";
}
std::string trim(std::string s) {
    auto first = s.find_first_not_of(" \t\r\n");
    if (first == s.npos)
        return "";
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}
Values parse(const std::string& input) {
    Values out;
    std::string text = input;
    if (text.rfind("\xef\xbb\xbf", 0) == 0)
        text.erase(0, 3);
    std::istringstream stream(text);
    std::string line, section;
    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        if (line[0] == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }
        auto eq = line.find('=');
        if (eq == line.npos || section.empty())
            continue;
        auto key = trim(line.substr(0, eq));
        if (key.empty())
            continue;
        auto value = trim(line.substr(eq + 1));
        if (key.size() > 256 || value.size() > 32768)
            continue;
        out[section + "." + key] = value;
    }
    return out;
}
std::string serialize(const Values& values) {
    std::string out = "; Clean Task Manager configuration (UTF-8)\r\n; Settings save when changed. Window "
                      "placement uses CleanTaskMan.window.ini.\r\n";
    std::string section;
    for (auto& [key, value] : values) {
        auto dot = key.find('.');
        if (dot == key.npos)
            continue;
        auto next = key.substr(0, dot);
        if (next != section) {
            section = next;
            out += "\r\n[" + section + "]\r\n";
        }
        std::string clean = value;
        clean.erase(std::remove(clean.begin(), clean.end(), '\r'), clean.end());
        clean.erase(std::remove(clean.begin(), clean.end(), '\n'), clean.end());
        out += key.substr(dot + 1) + "=" + clean + "\r\n";
    }
    return out;
}
Values load(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return {};
    LARGE_INTEGER size{};
    std::string data;
    if (GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart <= 262144) {
        data.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &read, nullptr))
            data.clear();
        else
            data.resize(read);
    }
    CloseHandle(h);
    return parse(data);
}
bool writeData(const std::wstring& path, const std::string& data, DWORD* error) {
    auto temp = path + L".tmp." + std::to_wstring(GetCurrentProcessId());
    HANDLE h =
        CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (error)
            *error = GetLastError();
        return false;
    }
    DWORD written = 0;
    bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
              written == data.size();
    DWORD code = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok && !code)
        code = ERROR_WRITE_FAULT;
    if (ok && !FlushFileBuffers(h)) {
        ok = false;
        code = GetLastError();
    }
    CloseHandle(h);
    if (ok && !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ok = false;
        code = GetLastError();
    }
    if (!ok)
        DeleteFileW(temp.c_str());
    if (error)
        *error = code;
    return ok;
}
bool save(const std::wstring& path, const Values& values, DWORD* error) {
    return writeData(path, serialize(values), error);
}
bool saveCompact(const std::wstring& path, const Values& values, DWORD* error) {
    auto data = serialize(values);
    auto first = data.find("[Window]");
    return writeData(path, first == data.npos ? data : data.substr(first), error);
}
std::wstring placementPath() {
    return executableFolder() + L"\\CleanTaskMan.window.ini";
}
Values placementValues(const Placement& p, const std::string& revision) {
    return {{"Window.Left", std::to_string(p.left)},    {"Window.Top", std::to_string(p.top)},
            {"Window.Width", std::to_string(p.width)},  {"Window.Height", std::to_string(p.height)},
            {"Window.Dpi", std::to_string(p.dpi)},      {"Window.Maximized", p.maximized ? "1" : "0"},
            {"Window.ConfigurationRevision", revision}, {"Window.Format", "2"}};
}
std::optional<Placement> readPlacement(const Values& values) {
    Placement p;
    p.left = integer(values, "Window.Left", -100001, -100000, 100000);
    p.top = integer(values, "Window.Top", -100001, -100000, 100000);
    p.width = integer(values, "Window.Width", 0, 320, 40000);
    p.height = integer(values, "Window.Height", 0, 240, 40000);
    p.dpi = integer(values, "Window.Dpi", 0, 48, 768);
    int max = integer(values, "Window.Maximized", -1, 0, 1);
    p.maximized = max == 1;
    if (p.left == -100001 || p.top == -100001 || !p.width || !p.height || !p.dpi || max < 0)
        return std::nullopt;
    return p;
}
bool overlayPlacement(Values& preferences, const Values& placement) {
    auto revision = get(preferences, "Configuration.Revision");
    // New placement files are independent of preference saves. Legacy files
    // retain their original revision check for a safe upgrade from 0.5.x.
    if (get(placement, "Window.Format") != "2" &&
        (revision.empty() || get(placement, "Window.ConfigurationRevision") != revision))
        return false;
    auto position = readPlacement(placement);
    if (!position)
        return false;
    auto values = placementValues(*position, "");
    for (auto& [key, value] : values)
        if (key != "Window.ConfigurationRevision" && key != "Window.Format")
            preferences[key] = value;
    return true;
}
int integer(const Values& v, const std::string& key, int fallback, int low, int high) {
    auto it = v.find(key);
    if (it == v.end())
        return fallback;
    int value = 0;
    auto r = std::from_chars(it->second.data(), it->second.data() + it->second.size(), value);
    return r.ec == std::errc{} && r.ptr == it->second.data() + it->second.size() && value >= low &&
                   value <= high
               ? value
               : fallback;
}
std::string get(const Values& v, const std::string& key, const std::string& fallback) {
    auto i = v.find(key);
    return i == v.end() ? fallback : i->second;
}
struct Writer::Impl {
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    bool stop = false, urgent = false, compact = false;
    uint64_t queued = 0, done = 0;
    std::wstring path;
    Values values;
    std::atomic<DWORD> error{0};
    void run() {
        std::unique_lock lock(mutex);
        for (;;) {
            wake.wait(lock, [&] { return stop || queued != done; });
            if (queued != done) {
                if (!stop && !urgent)
                    wake.wait_for(lock, std::chrono::milliseconds(200), [&] { return stop || urgent; });
                auto target = path;
                auto snapshot = values;
                auto generation = queued;
                bool small = compact;
                urgent = false;
                lock.unlock();
                DWORD code = 0;
                bool ok = small ? saveCompact(target, snapshot, &code) : save(target, snapshot, &code);
                if (!ok && !code)
                    code = ERROR_WRITE_FAULT;
                error = code;
                lock.lock();
                done = generation;
                if (done == queued)
                    urgent = false;
                wake.notify_all();
            }
            if (stop && queued == done)
                break;
        }
    }
};
Writer::Writer() : impl(std::make_unique<Impl>()) {}
Writer::~Writer() {
    if (impl->worker.joinable()) {
        {
            std::lock_guard lock(impl->mutex);
            impl->stop = true;
        }
        impl->wake.notify_all();
        impl->worker.join();
    }
}
void Writer::queue(const std::wstring& path, Values values) {
    std::lock_guard lock(impl->mutex);
    impl->path = path;
    impl->values = std::move(values);
    impl->compact = false;
    impl->queued++;
    if (!impl->worker.joinable())
        impl->worker = std::thread([this] { impl->run(); });
    impl->wake.notify_all();
}
void Writer::queueCompact(const std::wstring& path, Values values) {
    std::lock_guard lock(impl->mutex);
    impl->path = path;
    impl->values = std::move(values);
    impl->compact = true;
    impl->queued++;
    if (!impl->worker.joinable())
        impl->worker = std::thread([this] { impl->run(); });
    impl->wake.notify_all();
}
bool Writer::flush() {
    std::unique_lock lock(impl->mutex);
    if (impl->queued != impl->done)
        impl->urgent = true;
    impl->wake.notify_all();
    impl->wake.wait(lock, [&] { return impl->queued == impl->done; });
    return impl->error == 0;
}
DWORD Writer::error() const {
    return impl->error.load();
}
} // namespace settings

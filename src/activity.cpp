#include "activity.hpp"
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>
#include <objbase.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>
#include <vector>
#ifdef ACTIVITY_DIAGNOSTICS
#include <cstdio>
#endif

namespace activity {
namespace {
// Stable Windows TDH input-type values (Winmeta.h is absent in this MinGW SDK).
enum InputType : USHORT {
    InInt8 = 3,
    InUInt8 = 4,
    InInt16 = 5,
    InUInt16 = 6,
    InInt32 = 7,
    InUInt32 = 8,
    InInt64 = 9,
    InUInt64 = 10,
    InFloat = 11,
    InDouble = 12,
    InBoolean = 13,
    InGuid = 15,
    InPointer = 16,
    InFileTime = 17,
    InHex32 = 20,
    InHex64 = 21
};
constexpr GUID diskGuid{0x3d6fa8d4, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
constexpr GUID threadGuid{0x3d6fa8d1, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
constexpr GUID processGuid{0x3d6fa8d0, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
constexpr GUID tcpGuid{0x9a280ac0, 0xc8e0, 0x11d1, {0x84, 0xe2, 0x00, 0xc0, 0x4f, 0xb9, 0x98, 0xa2}};
constexpr GUID udpGuid{0xbf3a50c5, 0xa9c9, 0x4988, {0xa0, 0x05, 0x2d, 0xf0, 0xb7, 0xc8, 0x0f, 0x80}};
uint64_t ticks(FILETIME f) {
    return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}
uint64_t wallTime() {
    FILETIME t;
    GetSystemTimePreciseAsFileTime(&t);
    return ticks(t);
}
double clockTime() {
    LARGE_INTEGER n, f;
    QueryPerformanceCounter(&n);
    QueryPerformanceFrequency(&f);
    return static_cast<double>(n.QuadPart) / f.QuadPart;
}
void profilePrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return;
    TOKEN_PRIVILEGES p{};
    p.PrivilegeCount = 1;
    if (LookupPrivilegeValueW(nullptr, SE_SYSTEM_PROFILE_NAME, &p.Privileges[0].Luid)) {
        p.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(token, FALSE, &p, 0, nullptr, nullptr);
    }
    CloseHandle(token);
}
struct OwnedProcess {
    HANDLE handle = nullptr;
    uint64_t born = 0, exited = 0;
    ~OwnedProcess() {
        if (handle)
            CloseHandle(handle);
    }
};
struct Field {
    size_t offset = 0, size = 0;
};
struct SchemaKey {
    GUID provider{};
    USHORT id = 0;
    UCHAR version = 0, opcode = 0;
    bool ptr64 = false;
    bool operator<(const SchemaKey& b) const {
        int compare = memcmp(&provider, &b.provider, sizeof(provider));
        return compare ? compare < 0
                       : std::tie(id, version, opcode, ptr64) < std::tie(b.id, b.version, b.opcode, b.ptr64);
    }
};
} // namespace
bool elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION value{};
    DWORD got = 0;
    bool ok = GetTokenInformation(token, TokenElevation, &value, sizeof(value), &got) != FALSE;
    CloseHandle(token);
    return ok && value.TokenIsElevated;
}
struct Trace::Impl {
    std::atomic<bool> stopping{false};
    std::atomic<unsigned> interval{1000};
    HANDLE stopEvent = nullptr;
    TRACEHANDLE session = 0, consumer = INVALID_PROCESSTRACE_HANDLE;
    std::thread worker, eventThread;
    std::atomic<ULONG> consumerStatus{ERROR_IO_PENDING};
    std::function<void()> notify;
    mutable std::mutex publishedMutex;
    std::shared_ptr<const Frame> published = std::make_shared<Frame>();
    std::mutex dataMutex;
    std::map<Key, Bytes> totals;
    std::map<DWORD, std::unique_ptr<OwnedProcess>> identities;
    struct ThreadOwner {
        Key key{};
        uint64_t at = 0;
    };
    std::map<DWORD, ThreadOwner> threads;
    struct Issuer {
        Key key{};
        uint64_t at = 0;
    };
    std::map<uint64_t, Issuer> requests;
    std::map<SchemaKey, std::map<std::wstring, Field>> schemas;
    uint64_t schemaErrors = 0;
    std::wstring name;
    std::vector<unsigned char> properties;
    EVENT_TRACE_PROPERTIES* props() {
        return reinterpret_cast<EVENT_TRACE_PROPERTIES*>(properties.data());
    }
    void publish(std::shared_ptr<const Frame> value) {
        {
            std::lock_guard lock(publishedMutex);
            published = std::move(value);
        }
        if (!stopping && notify)
            notify();
    }
    Key identity(DWORD pid, uint64_t eventTime) {
        if (!pid)
            return {};
        auto it = identities.find(pid);
        if (it != identities.end() && WaitForSingleObject(it->second->handle, 0) == WAIT_OBJECT_0) {
            FILETIME b{}, e{}, k{}, u{};
            if (GetProcessTimes(it->second->handle, &b, &e, &k, &u))
                it->second->exited = ticks(e);
            if (!it->second->exited || eventTime > it->second->exited) {
                identities.erase(it);
                it = identities.end();
            }
        }
        if (it == identities.end()) {
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
            if (!h)
                return {};
            auto p = std::make_unique<OwnedProcess>();
            p->handle = h;
            bool exited = WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
            FILETIME born{}, exit{}, k{}, u{};
            if (!GetProcessTimes(h, &born, &exit, &k, &u))
                return {};
            p->born = ticks(born);
            p->exited = exited ? ticks(exit) : 0;
            it = identities.emplace(pid, std::move(p)).first;
        }
        auto& p = *it->second;
        if (!p.born || eventTime < p.born || (p.exited && eventTime > p.exited))
            return {};
        return {pid, p.born};
    }
    bool validKey(const Key& key, uint64_t time) {
        return key.second && identity(key.first, time) == key;
    }
    Key threadKey(DWORD tid, uint64_t time) {
        auto found = threads.find(tid);
        if (found != threads.end() && time >= found->second.at && validKey(found->second.key, time))
            return found->second.key;
        // Repair a missing thread-start/rundown record once, then cache ownership.
        HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, tid);
        if (!thread)
            return {};
        bool exited = WaitForSingleObject(thread, 0) == WAIT_OBJECT_0;
        FILETIME b{}, e{}, k{}, u{};
        DWORD pid = GetProcessIdOfThread(thread);
        bool measured = GetThreadTimes(thread, &b, &e, &k, &u) != FALSE;
        uint64_t born = ticks(b), exit = exited ? ticks(e) : 0;
        CloseHandle(thread);
        if (!measured || time < born || (exit && time > exit))
            return {};
        auto key = identity(pid, time);
        if (key.second)
            threads[tid] = {key, born};
        return key;
    }
    void seedThreads() {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return;
        THREADENTRY32 t{};
        t.dwSize = sizeof(t);
        uint64_t now = wallTime();
        if (Thread32First(snapshot, &t))
            do {
                auto key = identity(t.th32OwnerProcessID, now);
                if (key.second)
                    threads[t.th32ThreadID] = {key, now};
            } while (Thread32Next(snapshot, &t));
        CloseHandle(snapshot);
    }
    std::map<std::wstring, Field>& schema(EVENT_RECORD* event) {
        auto& header = event->EventHeader;
        bool ptr64 = (header.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) == 0;
        SchemaKey key{header.ProviderId, header.EventDescriptor.Id, header.EventDescriptor.Version,
                      header.EventDescriptor.Opcode, ptr64};
        auto found = schemas.find(key);
        if (found != schemas.end())
            return found->second;
        if (schemas.size() > 256)
            schemas.clear();
        auto& fields = schemas[key];
        ULONG bytes = 0;
        if (TdhGetEventInformation(event, 0, nullptr, nullptr, &bytes) != ERROR_INSUFFICIENT_BUFFER ||
            bytes > 1024 * 1024)
            return fields;
        std::vector<unsigned char> buffer(bytes);
        auto* info = reinterpret_cast<TRACE_EVENT_INFO*>(buffer.data());
        if (TdhGetEventInformation(event, 0, nullptr, info, &bytes) != ERROR_SUCCESS ||
            offsetof(TRACE_EVENT_INFO, EventPropertyInfoArray) +
                    static_cast<size_t>(info->TopLevelPropertyCount) * sizeof(EVENT_PROPERTY_INFO) >
                bytes)
            return fields;
        size_t offset = 0;
        for (ULONG i = 0; i < info->TopLevelPropertyCount; i++) {
            auto& property = info->EventPropertyInfoArray[i];
            if (property.NameOffset >= bytes ||
                property.Flags & (PropertyStruct | PropertyParamLength | PropertyParamCount))
                break;
            auto* n = reinterpret_cast<const wchar_t*>(buffer.data() + property.NameOffset);
            size_t max = (bytes - property.NameOffset) / sizeof(wchar_t), length = wcsnlen(n, max);
            if (length == max)
                break;
            std::wstring fieldName(n, length);
            size_t size = 0;
            switch (property.nonStructType.InType) {
            case InInt8:
            case InUInt8:
                size = 1;
                break;
            case InInt16:
            case InUInt16:
                size = 2;
                break;
            case InInt32:
            case InUInt32:
            case InHex32:
            case InFloat:
            case InBoolean:
                size = 4;
                break;
            case InInt64:
            case InUInt64:
            case InHex64:
            case InDouble:
            case InFileTime:
                size = 8;
                break;
            case InPointer:
                size = ptr64 ? 8 : 4;
                break;
            case InGuid:
                size = 16;
                break;
            default:
                break;
            }
            if (header.ProviderId == diskGuid && (fieldName == L"Irp" || fieldName == L"FileObject"))
                size = ptr64 ? 8 : 4;
            if (!size)
                break;
            size_t count = property.count ? property.count : 1;
            if (count > 1024 || size * count > event->UserDataLength ||
                offset + size * count > event->UserDataLength)
                break;
            if (count == 1)
                fields[fieldName] = {offset, size};
            offset += size * count;
        }
        return fields;
    }
    bool field(EVENT_RECORD* event, const wchar_t* name, uint64_t& value) {
        auto& fields = schema(event);
        auto it = fields.find(name);
        if (it != fields.end() && it->second.size <= 8 &&
            it->second.offset + it->second.size <= event->UserDataLength) {
            value = 0;
            memcpy(&value, static_cast<unsigned char*>(event->UserData) + it->second.offset, it->second.size);
            return true;
        }
        PROPERTY_DATA_DESCRIPTOR descriptor{};
        descriptor.PropertyName = reinterpret_cast<ULONGLONG>(name);
        descriptor.ArrayIndex = ULONG_MAX;
        ULONG size = 0;
        if (TdhGetPropertySize(event, 0, nullptr, 1, &descriptor, &size) != ERROR_SUCCESS || size > 8 ||
            !size)
            return false;
        value = 0;
        return TdhGetProperty(event, 0, nullptr, 1, &descriptor, size, reinterpret_cast<BYTE*>(&value)) ==
               ERROR_SUCCESS;
    }
    static void WINAPI callback(EVENT_RECORD* event) {
        auto* self = static_cast<Impl*>(event->UserContext);
        if (!self || self->stopping)
            return;
        try {
            std::lock_guard lock(self->dataMutex);
            self->consume(event);
        } catch (...) {
            std::lock_guard lock(self->dataMutex);
            self->schemaErrors++;
        }
    }
    void consume(EVENT_RECORD* event) {
        auto& h = event->EventHeader;
        auto op = h.EventDescriptor.Opcode;
        uint64_t time = static_cast<uint64_t>(h.TimeStamp.QuadPart);
        if (h.ProviderId == processGuid && (op == 1 || op == 3)) {
            uint64_t pid = 0;
            if (field(event, L"ProcessId", pid)) {
                if (op == 1)
                    identities.erase(static_cast<DWORD>(pid));
                identity(static_cast<DWORD>(pid), time);
            }
            return;
        }
        if (h.ProviderId == threadGuid && (op == 1 || op == 2 || op == 3 || op == 4)) {
            uint64_t pid = 0, tid = 0;
            if (field(event, L"ProcessId", pid) && field(event, L"TThreadId", tid)) {
                if (op == 1 || op == 3) {
                    auto key = identity(static_cast<DWORD>(pid), time);
                    if (key.second)
                        threads[static_cast<DWORD>(tid)] = {key, time};
                } else {
                    auto i = threads.find(static_cast<DWORD>(tid));
                    if (i != threads.end() && time >= i->second.at)
                        threads.erase(i);
                }
            }
            return;
        }
        if ((h.ProviderId == tcpGuid || h.ProviderId == udpGuid) &&
            (op == 10 || op == 11 || op == 26 || op == 27)) {
            uint64_t pid = 0, size = 0;
            if (!field(event, L"PID", pid) || !field(event, L"size", size)) {
                schemaErrors++;
                return;
            }
            auto key = identity(static_cast<DWORD>(pid), time);
            if (!key.second)
                return;
            auto& count = totals[key];
            if (op == 10 || op == 26)
                count.networkSend += size;
            else
                count.networkReceive += size;
            return;
        }
        if (h.ProviderId != diskGuid)
            return;
#ifdef ACTIVITY_DIAGNOSTICS
        static unsigned debugCount = 0;
        if (debugCount++ < 30) {
            wchar_t executable[32768]{};
            GetModuleFileNameW(nullptr, executable, 32768);
            std::wstring file = executable;
            file = file.substr(0, file.find_last_of(L"\\/")) + L"\\activity-schema.txt";
            FILE* debug = nullptr;
            _wfopen_s(&debug, file.c_str(), L"a");
            if (debug) {
                fwprintf(debug, L"DISK op=%u version=%u length=%u headerPID=%lu headerTID=%lu time=%llu\n",
                         op, h.EventDescriptor.Version, event->UserDataLength, h.ProcessId, h.ThreadId, time);
                for (auto& [name, description] : schema(event)) {
                    uint64_t value = 0;
                    field(event, name.c_str(), value);
                    fwprintf(debug, L"  %ls offset=%zu size=%zu value=%llu\n", name.c_str(),
                             description.offset, description.size, value);
                }
                fclose(debug);
            }
        }
#endif
        if (op == 12 || op == 13) {
            uint64_t irp = 0, tid = 0;
            if (!field(event, L"Irp", irp))
                return;
            field(event, L"IssuingThreadId", tid);
            if (!tid)
                tid = h.ThreadId;
            Key key{};
            key = threadKey(static_cast<DWORD>(tid), time);
            if (!key.second)
                key = identity(h.ProcessId, time);
            if (validKey(key, time)) {
                if (requests.size() > 65536)
                    requests.clear();
                requests[irp] = {key, time};
            }
            return;
        }
        if (op == 10 || op == 11) {
            uint64_t size = 0, irp = 0, tid = 0;
            if (!field(event, L"TransferSize", size)) {
                schemaErrors++;
                return;
            }
            field(event, L"Irp", irp);
            Key key{};
            auto request = requests.find(irp);
            if (request != requests.end()) {
                if (time >= request->second.at && time - request->second.at < 600000000)
                    key = request->second.key;
                requests.erase(request);
            }
            if (!key.second && field(event, L"IssuingThreadId", tid)) {
                key = threadKey(static_cast<DWORD>(tid), time);
            }
            if (validKey(key, time)) {
                auto& count = totals[key];
                if (op == 10)
                    count.diskRead += size;
                else
                    count.diskWrite += size;
            }
        }
    }
    void run() {
        if (stopping)
            return;
        profilePrivilege();
        GUID guid{};
        CoCreateGuid(&guid);
        wchar_t guidString[64]{};
        StringFromGUID2(guid, guidString, 64);
        name = L"CleanTaskMan.IO." + std::to_wstring(GetCurrentProcessId()) + L"." + guidString;
        properties.resize(sizeof(EVENT_TRACE_PROPERTIES) + (name.size() + 1) * sizeof(wchar_t));
        auto* p = props();
        p->Wnode.BufferSize = static_cast<ULONG>(properties.size());
        p->Wnode.Guid = guid;
        p->Wnode.ClientContext = 1;
        p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        p->BufferSize = 64;
        p->MinimumBuffers = 16;
        p->MaximumBuffers = 128;
        p->FlushTimer = 1;
        p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
        p->EnableFlags = EVENT_TRACE_FLAG_PROCESS | EVENT_TRACE_FLAG_THREAD | EVENT_TRACE_FLAG_DISK_IO |
                         EVENT_TRACE_FLAG_DISK_IO_INIT | EVENT_TRACE_FLAG_NETWORK_TCPIP;
        p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        memcpy(properties.data() + p->LoggerNameOffset, name.c_str(), (name.size() + 1) * sizeof(wchar_t));
        ULONG error = StartTraceW(&session, name.c_str(), p);
        if (error != ERROR_SUCCESS) {
            session = 0;
            auto f = std::make_shared<Frame>();
            f->error = error;
            f->session = name;
            publish(f);
            return;
        }
        seedThreads();
        EVENT_TRACE_LOGFILEW log{};
        log.LoggerName = name.data();
        log.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
        log.EventRecordCallback = callback;
        log.Context = this;
        consumer = OpenTraceW(&log);
        if (consumer == INVALID_PROCESSTRACE_HANDLE) {
            error = GetLastError();
            ControlTraceW(session, name.c_str(), props(), EVENT_TRACE_CONTROL_STOP);
            session = 0;
            auto f = std::make_shared<Frame>();
            f->error = error;
            publish(f);
            return;
        }
        eventThread = std::thread([this] { consumerStatus = ProcessTrace(&consumer, 1, nullptr, nullptr); });
        uint64_t epoch = 1, previousLost = 0, previousErrors = 0;
        while (!stopping) {
            if (WaitForSingleObject(stopEvent, interval.load()) != WAIT_TIMEOUT)
                break;
            auto f = std::make_shared<Frame>();
            f->active = true;
            f->valid = true;
            f->epoch = epoch;
            f->session = name;
            f->timestamp = clockTime();
            f->capturedFileTime = wallTime();
            if (consumerStatus != ERROR_IO_PENDING) {
                f->active = false;
                f->valid = false;
                f->error = consumerStatus ? consumerStatus.load() : ERROR_BROKEN_PIPE;
                publish(f);
                break;
            }
            if (interval.load() < 1000)
                ControlTraceW(session, name.c_str(), props(), EVENT_TRACE_CONTROL_FLUSH);
            auto status = ControlTraceW(session, name.c_str(), props(), EVENT_TRACE_CONTROL_QUERY);
            f->lost = static_cast<uint64_t>(props()->EventsLost) + props()->RealTimeBuffersLost;
            {
                std::lock_guard lock(dataMutex);
                if (status != ERROR_SUCCESS || f->lost != previousLost || schemaErrors != previousErrors) {
                    f->valid = false;
                    f->epoch = ++epoch;
                    f->error = status == ERROR_SUCCESS ? ERROR_INVALID_DATA : status;
                    // Re-establish ownership after an incomplete event interval.
                    requests.clear();
                    threads.clear();
                    identities.clear();
                    totals.clear();
                    seedThreads();
                }
                previousLost = f->lost;
                previousErrors = schemaErrors;
                f->totals = totals;
                if (totals.size() > 16384) {
                    for (auto i = totals.begin(); i != totals.end();)
                        if (!identities.count(i->first.first) ||
                            WaitForSingleObject(identities[i->first.first]->handle, 0) == WAIT_OBJECT_0)
                            i = totals.erase(i);
                        else
                            ++i;
                }
                for (auto i = requests.begin(); i != requests.end();)
                    if (f->capturedFileTime > i->second.at + 600000000)
                        i = requests.erase(i);
                    else
                        ++i;
                for (auto i = identities.begin(); i != identities.end();) {
                    auto& process = *i->second;
                    if (WaitForSingleObject(process.handle, 0) == WAIT_OBJECT_0) {
                        FILETIME b{}, e{}, k{}, u{};
                        if (GetProcessTimes(process.handle, &b, &e, &k, &u))
                            process.exited = ticks(e);
                        if (!process.exited || f->capturedFileTime > process.exited + 600000000) {
                            Key key{i->first, process.born};
                            totals.erase(key);
                            for (auto t = threads.begin(); t != threads.end();)
                                if (t->second.key == key)
                                    t = threads.erase(t);
                                else
                                    ++t;
                            i = identities.erase(i);
                            continue;
                        }
                    }
                    ++i;
                }
            }
            publish(f);
        }
        if (session) {
            ControlTraceW(session, name.c_str(), props(), EVENT_TRACE_CONTROL_STOP);
            session = 0;
        }
        if (consumer != INVALID_PROCESSTRACE_HANDLE) {
            CloseTrace(consumer);
        }
        if (eventThread.joinable())
            eventThread.join();
        consumer = INVALID_PROCESSTRACE_HANDLE;
    }
};
Trace::Trace() : impl(std::make_unique<Impl>()) {}
Trace::~Trace() {
    requestStop();
    join();
    if (impl->stopEvent)
        CloseHandle(impl->stopEvent);
}
void Trace::start(std::function<void()> notify) {
    if (impl->stopEvent || impl->stopping)
        return;
    impl->notify = std::move(notify);
    impl->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl->stopEvent)
        return;
    impl->worker = std::thread([this] {
        try {
            impl->run();
        } catch (...) {
            impl->stopping = true;
            if (impl->session)
                ControlTraceW(impl->session, impl->name.c_str(), impl->props(), EVENT_TRACE_CONTROL_STOP);
            if (impl->consumer != INVALID_PROCESSTRACE_HANDLE)
                CloseTrace(impl->consumer);
            if (impl->eventThread.joinable())
                impl->eventThread.join();
        }
    });
}
void Trace::requestStop() {
    impl->stopping = true;
    if (impl->stopEvent)
        SetEvent(impl->stopEvent);
}
void Trace::setUpdateInterval(unsigned ms) {
    impl->interval = (ms == 500 || ms == 1500) ? ms : 1000;
}
void Trace::join() {
    if (impl->worker.joinable())
        impl->worker.join();
}
std::shared_ptr<const Frame> Trace::snapshot() const {
    std::lock_guard lock(impl->publishedMutex);
    return impl->published;
}
} // namespace activity

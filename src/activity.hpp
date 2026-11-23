#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace activity {
using Key = std::pair<DWORD, uint64_t>;
struct Bytes {
    uint64_t diskRead = 0, diskWrite = 0, networkReceive = 0, networkSend = 0;
};
struct Frame {
    bool active = false, valid = false;
    DWORD error = 0;
    uint64_t epoch = 0, lost = 0, capturedFileTime = 0;
    double timestamp = 0;
    std::wstring session;
    std::map<Key, Bytes> totals;
};
class Trace {
  public:
    Trace();
    ~Trace();
    Trace(const Trace&) = delete;
    Trace& operator=(const Trace&) = delete;
    void start(std::function<void()> notify = {});
    void setUpdateInterval(unsigned milliseconds);
    void requestStop();
    void join();
    std::shared_ptr<const Frame> snapshot() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
bool elevated();
} // namespace activity

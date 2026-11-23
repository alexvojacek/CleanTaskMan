#include "cpu_parking.hpp"
#include <cstring>
namespace cpu_parking {
States parse(std::span<const unsigned char> bytes) {
    States states;
    size_t offset = 0;
    constexpr size_t flagsEnd = offsetof(SYSTEM_CPU_SET_INFORMATION, CpuSet.AllFlags) + 1;
    while (offset < bytes.size()) {
        if (bytes.size() - offset < sizeof(DWORD) * 2)
            return {};
        DWORD size = 0, type = 0;
        memcpy(&size, bytes.data() + offset, sizeof(size));
        memcpy(&type, bytes.data() + offset + 4, sizeof(type));
        if (size < 8 || size > bytes.size() - offset)
            return {};
        if (type == CpuSetInformation) {
            if (size < flagsEnd)
                return {};
            SYSTEM_CPU_SET_INFORMATION info{};
            memcpy(&info, bytes.data() + offset, flagsEnd);
            if (!states
                     .emplace(Key{info.CpuSet.Group, info.CpuSet.LogicalProcessorIndex},
                              info.CpuSet.Parked != 0)
                     .second)
                return {};
        }
        offset += size;
    }
    return states;
}
States Sampler::capture() {
    ULONG needed = 0;
    GetSystemCpuSetInformation(nullptr, 0, &needed, nullptr, 0);
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        if (!needed || needed > 1024 * 1024)
            return {};
        buffer.resize(needed);
        ULONG returned = 0;
        if (GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()),
                                       static_cast<ULONG>(buffer.size()), &returned, nullptr, 0)) {
            if (returned > buffer.size())
                return {};
            return parse(std::span(buffer.data(), returned));
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            return {};
        needed = returned;
    }
    return {};
}
} // namespace cpu_parking

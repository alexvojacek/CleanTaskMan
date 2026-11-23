#include "../src/telemetry.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>

int main() {
    int failures = 0;
    for (int trial = 1; trial <= 3; trial++) {
        telemetry::Collector collector;
        std::mutex firstMutex;
        std::shared_ptr<const telemetry::CoreSnapshot> firstPublished;
        double start = telemetry::clockSeconds();
        collector.start([&] {
            auto c = collector.core();
            if (c->ready) {
                std::lock_guard lock(firstMutex);
                if (!firstPublished)
                    firstPublished = c;
            }
        });
        double firstCore = -1, total = -1, process = -1, clock = -1, logical = -1, allLogical = -1;
        std::map<uint64_t, double> coreTimes, deviceTimes;
        bool firstBlank = false, ranges = true, stableSlots = true;
        size_t knownLogical = 0;
        while (telemetry::clockSeconds() - start < 8) {
            double elapsed = (telemetry::clockSeconds() - start) * 1000;
            auto c = collector.core();
            auto d = collector.devices();
            if (c->ready) {
                coreTimes.emplace(c->revision, c->timestamp);
                if (firstCore < 0) {
                    firstCore = elapsed;
                    std::lock_guard lock(firstMutex);
                    firstBlank = firstPublished && std::isnan(firstPublished->cpu);
                }
                if (total < 0 && std::isfinite(c->cpu))
                    total = elapsed;
                if (std::isfinite(c->cpu) && (c->cpu < 0 || c->cpu > 100))
                    ranges = false;
                for (auto& p : c->processes)
                    if (p.pid == GetCurrentProcessId() && std::isfinite(p.cpu) && process < 0)
                        process = elapsed;
            }
            if (d->ready) {
                deviceTimes.emplace(d->revision, d->timestamp);
                if (clock < 0 && std::isfinite(d->frequencyGHz) && d->frequencyGHz > 0)
                    clock = elapsed;
                auto good = std::count_if(d->logicalCpu.begin(), d->logicalCpu.end(), [](auto& p) {
                    return std::isfinite(p.usage) && p.usage >= 0 && p.usage <= 100;
                });
                knownLogical = good;
                stableSlots =
                    stableSlots && d->logicalCpu.size() == GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
                for (auto& cpu : d->logicalCpu)
                    if (std::isfinite(cpu.usage) && (cpu.usage < 0 || cpu.usage > 100))
                        ranges = false;
                if (logical < 0 && good)
                    logical = elapsed;
                if (allLogical < 0 && good == GetActiveProcessorCount(ALL_PROCESSOR_GROUPS))
                    allLogical = elapsed;
            }
            if (total >= 0 && process >= 0 && clock >= 0 && logical >= 0 && coreTimes.size() >= 4 &&
                deviceTimes.size() >= 3)
                break;
            Sleep(1);
        }
        collector.requestStop();
        collector.join();
        double cadence = coreTimes.count(3) && coreTimes.count(4) ? (coreTimes[4] - coreTimes[3]) * 1000 : 0;
        double deviceCadence =
            deviceTimes.count(2) && deviceTimes.count(3) ? (deviceTimes[3] - deviceTimes[2]) * 1000 : 0;
        // Windows can explicitly return PDH_CSTATUS_INVALID_DATA for an
        // individual instance. That slot must remain blank, not become zero.
        bool good = firstBlank && ranges && stableSlots && total >= 0 && process >= 0 && clock >= 0 &&
                    logical >= 0 && cadence >= 900 && deviceCadence >= 900;
        printf("TRIAL %d core_ms=%.1f cpu_ms=%.1f self_cpu_ms=%.1f speed_ms=%.1f logical_ms=%.1f "
               "all_logical_ms=%.1f steady_core_ms=%.1f steady_devices_ms=%.1f first_cpu_blank=%d %s\n",
               trial, firstCore, total, process, clock, logical, allLogical, cadence, deviceCadence,
               firstBlank, good ? "PASS" : "FAIL");
        fflush(stdout);
        printf("SOURCE known_logical=%zu total_slots=%lu (-1 all_logical_ms means not all instances became "
               "valid during this observation)\n",
               knownLogical, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
        if (!good)
            failures++;
    }
    printf("RESULT failures=%d; timings start at Collector.start(), exclude UI/UAC/loader, warm local runs\n",
           failures);
    return failures ? 1 : 0;
}

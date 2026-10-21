// Network probe used by Metrics. The UI receives NetEvent from the Start callback.
#pragma once

#include "metrics.h"

#include <cstdint>
#include <string>

namespace pulse {

// Optional diagnostics shared by Metrics and NetProbe. Install before Start and
// clear after all Metrics/NetProbe instances have stopped. Normal use reads only
// a null callback; QPC/GetThreadTimes are never called unless enabled.
namespace diagnostics {
using TimingCallback = void (*)(const wchar_t* category, double wall_ms, double cpu_ms, void* user);
void SetTimingCallback(TimingCallback callback, void* user);
class ScopedTiming {
public:
    explicit ScopedTiming(const wchar_t* category) noexcept;
    ~ScopedTiming() noexcept;
    ScopedTiming(const ScopedTiming&) = delete;
    ScopedTiming& operator=(const ScopedTiming&) = delete;
private:
    TimingCallback callback_ = nullptr;
    void* user_ = nullptr;
    const wchar_t* category_ = nullptr;
    std::int64_t start_ = 0;
    std::uint64_t cpu_ = 0;
};
} // namespace diagnostics

// Shared by recovery logs, notifications and the offline status in the UI.
std::wstring FormatOutageDuration(std::uint64_t seconds);

class NetProbe {
public:
    NetProbe();
    ~NetProbe();
    NetProbe(const NetProbe&) = delete;
    NetProbe& operator=(const NetProbe&) = delete;

    void Start(const Settings& settings, Metrics::EventCallback cb, void* user);
    void Stop();  // Signal the probe thread and wait until it exits.
    std::wstring LogPath() const;

    struct View {
        NetState state = NetState::Checking;
        double latency_ms = NAN;
        std::int64_t offline_since_unix = 0;
        bool offline_by_latency = false;
    };

    View Get() const;

    void SetSlowThreshold(int slow_ms); // Uses the same lock as result classification.

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace pulse

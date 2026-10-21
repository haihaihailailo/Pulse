// Foreground Present rate. The ETW session exists only after a successful Start.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstddef>
#include <cstdint>

namespace pulse {

// One process's Present count in the lagged window [now-2s, now-1s).
// frames is that event count, not a frame rate and not a sum across processes.
struct FpsPresent {
    DWORD pid = 0;
    std::uint64_t frames = 0;
};

// Start, Stop and Sample are called from the metrics thread.
// The consumer callback runs on a private thread and only appends a timestamp.
class FpsMonitor {
public:
    FpsMonitor();
    ~FpsMonitor();
    FpsMonitor(const FpsMonitor&) = delete;
    FpsMonitor& operator=(const FpsMonitor&) = delete;

    // False: Sample keeps reporting status 2 (access denied) or 4.
    bool Start();
    void Stop();
    // True while the ETW session and consumer are up. False does not touch the session.
    bool Active() const;
    unsigned long LastError() const; // Last startup/consumer failure, 0 on success.

    // pids[0] is the foreground process; the rest are its descendants.
    // The rate is that set's single busiest process. Present counts are not added
    // together: a browser can have several compositors, and summing them invents
    // a frame rate higher than the display.
    // Counts come from event timestamps in [now-2s, now-1s), not from how many
    // events happened to arrive between Sample calls. fps is NaN until the
    // session has been running for 2 seconds.
    // source is the process that supplied the rate, or 0 when no rate is produced.
    // top receives up to top_cap processes with the most Presents in that window
    // on the whole machine (any process, not only the foreground tree).
    void Sample(const DWORD* pids, std::size_t count, double& fps, int& status, DWORD& source,
                FpsPresent* top, int top_cap, int& top_count);

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace pulse

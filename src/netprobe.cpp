// WinHTTP probe. Notifications use the Metrics::Start callback.
// Log file: %LOCALAPPDATA%\Pulse\pulse_net.log.
// Shutdown signals the worker and joins it. Async requests are cancelled.
// Each stage has one deadline; callback contexts live until HANDLE_CLOSING.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0603
#endif

#include "netprobe.h"

#include <windows.h>
#include <winhttp.h>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <cwchar>
#include <memory>
#include <utility>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#pragma comment(lib, "winhttp.lib")
#endif

namespace pulse {
namespace diagnostics {
namespace {
std::atomic<TimingCallback> timing_callback{nullptr};
std::atomic<void*> timing_user{nullptr};
std::uint64_t ThreadCpuTicks() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
    return k.QuadPart + u.QuadPart;
}
double CounterFrequency() {
    static const double frequency = [] {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value) ? static_cast<double>(value.QuadPart) : 0.0;
    }();
    return frequency;
}
}
void SetTimingCallback(TimingCallback callback, void* user) {
    timing_user.store(user, std::memory_order_relaxed);
    timing_callback.store(callback, std::memory_order_release);
}
ScopedTiming::ScopedTiming(const wchar_t* category) noexcept {
    callback_ = timing_callback.load(std::memory_order_acquire);
    if (!callback_) return;
    user_ = timing_user.load(std::memory_order_relaxed);
    category_ = category;
    (void)CounterFrequency();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    start_ = now.QuadPart;
    cpu_ = ThreadCpuTicks();
}
ScopedTiming::~ScopedTiming() noexcept {
    if (!callback_) return;
    const std::uint64_t cpu = ThreadCpuTicks();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const double frequency = CounterFrequency();
    const double wall_ms = frequency > 0 ? (now.QuadPart - start_) * 1000.0 / frequency : 0.0;
    const double cpu_ms = cpu >= cpu_ ? static_cast<double>(cpu - cpu_) / 10000.0 : 0.0;
    try { callback_(category_, wall_ms, cpu_ms, user_); } catch (...) {}
}
} // namespace diagnostics

std::wstring FormatOutageDuration(std::uint64_t seconds) {
    if (seconds >= 3600) return std::to_wstring(seconds / 3600) + L" 小时 " +
                               std::to_wstring((seconds / 60) % 60) + L" 分";
    if (seconds >= 60) return std::to_wstring(seconds / 60) + L" 分 " +
                             std::to_wstring(seconds % 60) + L" 秒";
    return std::to_wstring(seconds) + L" 秒";
}

namespace {

const wchar_t* kDefaultUrls[] = {
    L"http://www.msftconnecttest.com/connecttest.txt",
    L"http://www.gstatic.com/generate_204",
    L"http://connect.rom.miui.com/generate_204",
};

enum class Display { Checking, Online, Slow, Offline, Unusable };

constexpr ULONGLONG kRoundTimeoutMs = 2500;
constexpr ULONGLONG kConfirmIntervalMs = 1000;
constexpr ULONGLONG kOfflineIntervalMs = 1500;
constexpr ULONGLONG kNicDebounceMs = 300;
constexpr ULONGLONG kResumeDelayMs = 3000;
constexpr ULONGLONG kSuspendGapMs = 60000;

std::int64_t ProbeClockNow() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

ULONGLONG ProbeElapsedMs(std::int64_t start, std::int64_t end) {
    if (end <= start) return 0;
    return static_cast<ULONGLONG>((end - start) / 1000000);
}

std::wstring GetEnv(const wchar_t* name) {
    const DWORD need = GetEnvironmentVariableW(name, nullptr, 0);
    if (need == 0) return L"";
    std::wstring value(need, L'\0');
    const DWORD got = GetEnvironmentVariableW(name, &value[0], need);
    if (got == 0 || got >= need) return L"";
    value.resize(got);
    return value;
}

std::wstring MakeLogPath() {
    std::wstring base = GetEnv(L"LOCALAPPDATA");
    if (base.empty()) {
        const std::wstring user = GetEnv(L"USERPROFILE");
        if (!user.empty()) base = user + L"\\AppData\\Local";
    }
    if (base.empty()) base = L".";
    return base + L"\\Pulse\\pulse_net.log";
}

void EnsureParent(const std::wstring& path) {
    const std::size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos || pos == 0) return;
    CreateDirectoryW(path.substr(0, pos).c_str(), nullptr);
}

bool WriteAll(HANDLE file, const void* buffer, DWORD count) {
    auto bytes = static_cast<const unsigned char*>(buffer);
    while (count) {
        DWORD written = 0;
        if (!WriteFile(file, bytes, count, &written, nullptr) || written == 0) return false;
        bytes += written;
        count -= written;
    }
    return true;
}

std::wstring LocalTimeText(const SYSTEMTIME& time, bool date) {
    wchar_t buffer[64];
    buffer[0] = L'\0';
    if (date) {
        std::swprintf(buffer, 64, L"%04u-%02u-%02u %02u:%02u:%02u",
                      static_cast<unsigned>(time.wYear), static_cast<unsigned>(time.wMonth),
                      static_cast<unsigned>(time.wDay), static_cast<unsigned>(time.wHour),
                      static_cast<unsigned>(time.wMinute), static_cast<unsigned>(time.wSecond));
    } else {
        std::swprintf(buffer, 64, L"%02u:%02u", static_cast<unsigned>(time.wHour),
                      static_cast<unsigned>(time.wMinute));
    }
    return buffer;
}

std::int64_t LocalSystemTimeToUnix(const SYSTEMTIME& local) {
    FILETIME local_ft{};
    FILETIME utc_ft{};
    if (!SystemTimeToFileTime(&local, &local_ft)) return 0;
    if (!LocalFileTimeToFileTime(&local_ft, &utc_ft)) return 0;
    ULARGE_INTEGER u;
    u.LowPart = utc_ft.dwLowDateTime;
    u.HighPart = utc_ft.dwHighDateTime;
    const unsigned long long epoch = 11644473600ULL;
    const unsigned long long secs = u.QuadPart / 10000000ULL;
    if (secs < epoch) return 0;
    return static_cast<std::int64_t>(secs - epoch);
}

int ClampInt(int value, int minimum, int maximum, int fallback) {
    if (value < minimum || value > maximum) return fallback;
    return value;
}

// Every outstanding callback keeps its context and this event alive. Closing
// losing requests never waits on a per-request timeout on the result path.
struct ProbeSignal {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~ProbeSignal() { if (event) CloseHandle(event); }
};
struct ProbeContext {
    std::shared_ptr<ProbeSignal> signal;
    std::atomic<bool> sent{false};
    std::atomic<bool> headers{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> timed_out{false};
    std::atomic<std::int64_t> response_tick{0};
    std::atomic<int> refs{1};
    explicit ProbeContext(std::shared_ptr<ProbeSignal> value) : signal(std::move(value)) {}
    void Release() {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
    }
};

struct ProbeRequest {
    HINTERNET request = nullptr;
    HINTERNET connection = nullptr;
    ProbeContext* probe = nullptr;
    bool receiving = false;
    ~ProbeRequest() {
        if (request) WinHttpCloseHandle(request); // Cancels pending async I/O.
        if (connection) WinHttpCloseHandle(connection);
        if (probe) probe->Release(); // HANDLE_CLOSING releases the callback reference.
    }
};

void CALLBACK ProbeCallback(HINTERNET, DWORD_PTR context, DWORD status, void* info, DWORD info_size) {
    if (!context) return;
    diagnostics::ScopedTiming timing(L"Network callback");
    auto* probe = reinterpret_cast<ProbeContext*>(context);
    switch (status) {
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
        probe->sent.store(true, std::memory_order_release);
        SetEvent(probe->signal->event);
        break;
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
        probe->response_tick.store(ProbeClockNow(), std::memory_order_relaxed);
        probe->headers.store(true, std::memory_order_release);
        SetEvent(probe->signal->event);
        break;
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
        if (info && info_size >= sizeof(WINHTTP_ASYNC_RESULT) &&
            static_cast<const WINHTTP_ASYNC_RESULT*>(info)->dwError == ERROR_WINHTTP_TIMEOUT)
            probe->timed_out.store(true, std::memory_order_relaxed);
        probe->failed.store(true, std::memory_order_release);
        SetEvent(probe->signal->event);
        break;
    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
        probe->Release();
        break;
    default:
        break;
    }
}

struct Config {
    int interval_s = 5;
    int timeout_ms = 1500;
    int fail_threshold = 3;
    int slow_ms = 500;
    int unusable_ms = 1000;
    std::vector<std::wstring> urls;
};

Config Normalize(const Settings& settings) {
    Config config;
    config.interval_s = ClampInt(settings.probe_interval_s, 1, 86400, 5);
    // Keep the append-only Settings interface; its legacy seconds value may
    // request 1 s, but no request is allowed to retain the old 4 s timeout.
    config.timeout_ms = settings.probe_timeout_s == 1 ? 1000 : 1500;
    config.fail_threshold = ClampInt(settings.fail_threshold, 1, 1000000, 3);
    config.slow_ms = ClampInt(settings.slow_ms, 1, 999, 500);
    config.unusable_ms = ClampInt(settings.unusable_ms, 2, 2147483647, 1000);
    if (config.slow_ms >= config.unusable_ms) {
        config.slow_ms = 500;
        config.unusable_ms = 1000;
    }
    if (settings.probe_urls.empty()) {
        for (const wchar_t* url : kDefaultUrls) config.urls.emplace_back(url);
    } else {
        config.urls = settings.probe_urls;
    }
    return config;
}

struct Status {
    Display display = Display::Checking;
    bool has_result = false;
    bool latest_ok = false;
    ULONGLONG latest_ms = 0;
    unsigned fail_count = 0;
    ULONGLONG first_failure_tick = 0;
    std::int64_t first_failure_unix = 0;
    SYSTEMTIME first_failure_time{};
    bool outage_active = false;
    std::int64_t outage_unix = 0;
    bool link_disconnected = false;
};

class CsLock {
public:
    explicit CsLock(CRITICAL_SECTION* cs) : cs_(cs) { EnterCriticalSection(cs_); }
    ~CsLock() { LeaveCriticalSection(cs_); }
    CsLock(const CsLock&) = delete;
    CsLock& operator=(const CsLock&) = delete;

private:
    CRITICAL_SECTION* cs_;
};

}  // namespace

struct NetProbe::Impl {
    Impl() {
        InitializeCriticalSection(&status_cs_);
        cs_ready_ = true;
        log_path_ = MakeLogPath();
    }

    ~Impl() {
        Stop();
        if (wake_event_) CloseHandle(wake_event_);
        if (network_event_) CloseHandle(network_event_);
        if (cs_ready_) {
            DeleteCriticalSection(&status_cs_);
            cs_ready_ = false;
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void Start(const Settings& settings, Metrics::EventCallback cb, void* user) {
        if (thread_) return;
        cb_ = cb;
        user_ = user;
        config_ = Normalize(settings);
        EnsureParent(log_path_);

        stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        unloaded_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_event_ || !unloaded_event_ || !wake_event_ || !network_event_) {
            CleanupStartFailure();
            return;
        }

        session_ = WinHttpOpen(L"Pulse/1.0.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
        if (!session_) {
            session_ = WinHttpOpen(L"Pulse/1.0.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
        }
        if (!session_) {
            AppendLog(L"\u68C0\u6D4B\u51FA\u9519\uFF1AWinHTTP \u4F1A\u8BDD\u521B\u5EFA\u5931\u8D25");
            CleanupStartFailure();
            return;
        }
        if (!WinHttpSetOption(session_, WINHTTP_OPTION_UNLOAD_NOTIFY_EVENT, &unloaded_event_,
                              sizeof(unloaded_event_))) {
            AppendLog(L"\u68C0\u6D4B\u51FA\u9519\uFF1AWinHTTP \u521D\u59CB\u5316\u5931\u8D25");
            CleanupStartFailure();
            return;
        }
        unload_set_ = true;

        const int full_ms = config_.timeout_ms;
        // Each internal timeout is bounded too; the outer stage/round deadlines
        // bound their sum. Do not shrink connect to 500 ms by splitting the budget:
        // that could reject otherwise usable (<1000 ms) connections.
        if (!WinHttpSetTimeouts(session_, full_ms, full_ms, full_ms, full_ms)) {
            AppendLog(L"\u68C0\u6D4B\u51FA\u9519\uFF1AWinHTTP \u521D\u59CB\u5316\u5931\u8D25");
            CleanupStartFailure();
            return;
        }

        thread_ = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
        if (!thread_) {
            AppendLog(L"\u68C0\u6D4B\u51FA\u9519\uFF1A\u540E\u53F0\u63A2\u6D4B\u542F\u52A8\u5931\u8D25");
            CleanupStartFailure();
        }
    }

    void Stop() {
        if (stop_event_) SetEvent(stop_event_);
        if (thread_) {
            WaitForSingleObject(thread_, INFINITE);
            CloseHandle(thread_);
            thread_ = nullptr;
        }
        CloseSession();
        if (stop_event_) {
            CloseHandle(stop_event_);
            stop_event_ = nullptr;
        }
        if (unloaded_event_) {
            CloseHandle(unloaded_event_);
            unloaded_event_ = nullptr;
        }
    }

    std::wstring LogPath() const { return log_path_; }

    NetProbe::View Get() const {
        if (!cs_ready_) return view_;
        CsLock lock(&status_cs_);
        return view_;
    }

    void SetSlowThreshold(int slow_ms) {
        CsLock lock(&status_cs_);
        config_.slow_ms = ClampInt(slow_ms, 1, config_.unusable_ms - 1, 500);
        // Classification and configuration share this lock with ApplyResult.
        // Do not change failure counts, outage timestamps, logs or notifications.
        if (status_.has_result && status_.latest_ok &&
            status_.latest_ms < static_cast<ULONGLONG>(config_.unusable_ms) &&
            (status_.display == Display::Online || status_.display == Display::Slow)) {
            status_.display = status_.latest_ms >= static_cast<ULONGLONG>(config_.slow_ms)
                                  ? Display::Slow : Display::Online;
        }
        view_ = MakeView(status_);
    }

    void OnSuspend() {
        CsLock lock(&status_cs_);
        if (suspended_) return;
        suspended_ = true;
        resume_pending_ = false;
        ++power_epoch_; // Invalidate an in-flight result before cancelling its I/O.
        if (status_.outage_active)
            AppendLog(L"进入睡眠，结束本次断网计时（已持续 " +
                      FormatOutageDuration((GetTickCount64() - status_.first_failure_tick) / 1000) + L"）");
        else
            AppendLog(L"进入睡眠，暂停网络探测");
        status_ = Status{};
        view_ = MakeView(status_);
        if (wake_event_) SetEvent(wake_event_);
    }

    void OnResume() {
        CsLock lock(&status_cs_);
        // Windows may send AUTOMATIC and RESUMESUSPEND for the same resume.
        if (!suspended_ && resume_pending_) return;
        ResetAfterResumeLocked(false);
    }

    void RequestProbe() {
        // This handle lives until Impl destruction, including before Start and
        // after Stop. Metrics deregisters its NIC callback before destroying us.
        network_pending_.store(true, std::memory_order_release);
        if (network_event_) SetEvent(network_event_);
    }

private:
    void ResetAfterResumeLocked(bool detected_gap) {
        if (detected_gap && status_.outage_active)
            AppendLog(L"检测到系统挂起：进入睡眠，结束本次断网计时");
        suspended_ = false;
        resume_pending_ = true;
        resume_not_before_ = GetTickCount64() + kResumeDelayMs;
        ++power_epoch_;
        status_ = Status{};
        view_ = MakeView(status_);
        AppendLog(detected_gap ? L"检测到系统挂起，3 秒后重新探测" : L"系统恢复，3 秒后重新探测");
        if (wake_event_) SetEvent(wake_event_);
    }

    void ResumeAfterGap(std::uint64_t epoch) {
        CsLock lock(&status_cs_);
        if (!suspended_ && epoch == power_epoch_) ResetAfterResumeLocked(true);
    }

    static DWORD WINAPI ThreadProc(LPVOID param) {
        auto* self = static_cast<Impl*>(param);
        try {
            self->Run();
        } catch (...) {
            try {
                self->AppendLog(L"\u68C0\u6D4B\u51FA\u9519\uFF1A\u540E\u53F0\u63A2\u6D4B\u53D1\u751F\u5F02\u5E38\uFF0C\u76D1\u6D4B\u5DF2\u505C\u6B62");
            } catch (...) {
            }
            self->CloseSession();
        }
        return 0;
    }

    bool Stopping() const {
        return stop_event_ && WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0;
    }

    void CleanupStartFailure() {
        CloseSession();
        if (stop_event_) {
            CloseHandle(stop_event_);
            stop_event_ = nullptr;
        }
        if (unloaded_event_) {
            CloseHandle(unloaded_event_);
            unloaded_event_ = nullptr;
        }
    }

    void CloseSession() {
        if (!session_) return;
        HINTERNET session = session_;
        session_ = nullptr;
        WinHttpCloseHandle(session);
        // WinHTTP owns this event until the last session callback has returned.
        // Do not close it after a short timeout while WinHTTP may still signal it.
        if (unload_set_ && unloaded_event_) WaitForSingleObject(unloaded_event_, INFINITE);
        unload_set_ = false;
    }

    void AppendLog(const std::wstring& message) const {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        const std::wstring line = L"[" + LocalTimeText(now, true) + L"] " + message + L"\r\n";
        const int count = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                                              nullptr, 0, nullptr, nullptr);
        if (count <= 0) return;
        std::string utf8(static_cast<std::size_t>(count), '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), &utf8[0], count,
                            nullptr, nullptr);
        EnsureParent(log_path_);
        HANDLE file = CreateFileW(log_path_.c_str(), FILE_APPEND_DATA | FILE_READ_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER size{};
        if (GetFileSizeEx(file, &size) && size.QuadPart == 0) {
            const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
            WriteAll(file, bom, sizeof(bom));
        }
        WriteAll(file, utf8.data(), static_cast<DWORD>(utf8.size()));
        CloseHandle(file);
    }

    bool Interrupted() const {
        return network_pending_.load(std::memory_order_acquire) || Stopping() ||
               (wake_event_ && WaitForSingleObject(wake_event_, 0) == WAIT_OBJECT_0);
    }

    bool PendingOrSuccessful(BOOL result) const {
        return result || GetLastError() == ERROR_IO_PENDING;
    }

    bool BeginRequest(const std::wstring& url, ProbeRequest& item,
                      const std::shared_ptr<ProbeSignal>& signal) {
        if (url.empty() || Interrupted() || !session_) return false;
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        parts.dwUrlPathLength = static_cast<DWORD>(-1);
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) ||
            (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) ||
            !parts.lpszHostName || !parts.dwHostNameLength ||
            parts.dwHostNameLength == static_cast<DWORD>(-1)) return false;
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring target;
        if (parts.dwUrlPathLength && parts.dwUrlPathLength != static_cast<DWORD>(-1))
            target.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (target.empty()) target = L"/";
        if (parts.dwExtraInfoLength && parts.dwExtraInfoLength != static_cast<DWORD>(-1) &&
            parts.lpszExtraInfo) target.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        const std::size_t fragment = target.find(L'#');
        if (fragment != std::wstring::npos) target.erase(fragment);

        item.probe = new ProbeContext(signal);
        item.connection = WinHttpConnect(session_, host.c_str(), parts.nPort, 0);
        if (!item.connection) return false;
        const DWORD flags = WINHTTP_FLAG_REFRESH |
            (parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        item.request = WinHttpOpenRequest(item.connection, L"GET", target.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!item.request) return false;
        DWORD disabled = WINHTTP_DISABLE_REDIRECTS;
        DWORD_PTR context = reinterpret_cast<DWORD_PTR>(item.probe);
        if (!WinHttpSetOption(item.request, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) ||
            !WinHttpSetOption(item.request, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)) ||
            !WinHttpAddRequestHeaders(item.request, L"Connection: close\r\n", static_cast<DWORD>(-1),
                WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE) || Interrupted()) return false;

        item.probe->refs.store(2, std::memory_order_relaxed);
        const DWORD callbacks = WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE |
            WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE | WINHTTP_CALLBACK_FLAG_REQUEST_ERROR |
            WINHTTP_CALLBACK_FLAG_HANDLES;
        if (WinHttpSetStatusCallback(item.request, ProbeCallback, callbacks, 0) ==
            WINHTTP_INVALID_STATUS_CALLBACK) {
            item.probe->refs.store(1, std::memory_order_relaxed);
            return false;
        }
        return PendingOrSuccessful(WinHttpSendRequest(item.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, context));
    }

    // Each stage has one deadline; the primary and parallel backups also share
    // a 2.5 s round deadline. A late callback can never extend either deadline.
    bool ProbeUrls(std::size_t first, std::size_t last, ULONGLONG& elapsed,
                   std::int64_t round_start, bool& timed_out) {
        diagnostics::ScopedTiming timing(last - first > 1 ? L"Network backups (parallel)" : L"Network primary");
        const std::int64_t start = ProbeClockNow();
        const ULONGLONG round_spent = ProbeElapsedMs(round_start, start);
        if (round_spent >= kRoundTimeoutMs) { timed_out = true; return false; }
        const ULONGLONG remaining = kRoundTimeoutMs - round_spent;
        const ULONGLONG budget = remaining < static_cast<ULONGLONG>(config_.timeout_ms)
                                   ? remaining : static_cast<ULONGLONG>(config_.timeout_ms);
        auto signal = std::make_shared<ProbeSignal>();
        if (!signal->event) return false;
        std::vector<std::unique_ptr<ProbeRequest>> requests;
        requests.reserve(last - first);
        for (std::size_t i = first; i < last; ++i) {
            if (Interrupted() || ProbeElapsedMs(start, ProbeClockNow()) >= budget) break;
            auto item = std::make_unique<ProbeRequest>();
            // Insert first so even an allocation exception leaves no live handle behind.
            requests.push_back(std::move(item));
            auto& current = *requests.back();
            if (!BeginRequest(config_.urls[i], current, signal) && current.probe)
                current.probe->failed.store(true, std::memory_order_release);
        }
        HANDLE events[] = {stop_event_, wake_event_, network_event_, signal->event};
        for (;;) {
            ResetEvent(signal->event); // Reset BEFORE inspecting flags: no lost wakeups.
            if (Interrupted()) return false;
            std::int64_t winner = 0;
            bool pending = false;
            for (const auto& item : requests) {
                auto* probe = item->probe;
                if (!probe) continue;
                if (probe->headers.load(std::memory_order_acquire)) {
                    const auto tick = probe->response_tick.load(std::memory_order_relaxed);
                    if (!winner || tick < winner) winner = tick;
                } else if (!probe->failed.load(std::memory_order_acquire)) {
                    pending = true;
                    // All WinHTTP API calls on this request stay on this thread.
                    // SendRequest has returned before ReceiveResponse is started.
                    if (!item->receiving && probe->sent.load(std::memory_order_acquire)) {
                        item->receiving = true;
                        if (!PendingOrSuccessful(WinHttpReceiveResponse(item->request, nullptr))) {
                            probe->failed.store(true, std::memory_order_release);
                            SetEvent(signal->event);
                        }
                    }
                } else if (probe->timed_out.load(std::memory_order_relaxed)) {
                    // failed's acquire above makes the callback's timeout flag visible.
                    timed_out = true;
                }
            }
            if (winner) {
                elapsed = ProbeElapsedMs(start, winner);
                if (elapsed > budget) { timed_out = true; return false; }
                return true; // RAII immediately cancels every losing request.
            }
            const ULONGLONG spent = ProbeElapsedMs(start, ProbeClockNow());
            if (!pending || spent >= budget) {
                if (spent >= budget) timed_out = true;
                return false;
            }
            const DWORD waited = WaitForMultipleObjects(4, events, FALSE, static_cast<DWORD>(budget - spent));
            // The flag survives an auto-reset event wait. A late signal for an
            // already consumed notification is just a wakeup, not an HTTP failure.
            if (waited == WAIT_OBJECT_0 + 2 && !network_pending_.load(std::memory_order_acquire)) continue;
            if (waited != WAIT_OBJECT_0 + 3) {
                if (waited == WAIT_TIMEOUT) timed_out = true;
                return false;
            }
        }
    }

    NetProbe::View MakeView(const Status& state) const {
        NetProbe::View view;
        switch (state.display) {
        case Display::Online:
            view.state = NetState::Online;
            break;
        case Display::Slow:
            view.state = NetState::Slow;
            break;
        case Display::Offline:
        case Display::Unusable:
            view.state = NetState::Offline;
            break;
        default:
            view.state = NetState::Checking;
            break;
        }
        if (state.has_result && state.latest_ok) view.latency_ms = static_cast<double>(state.latest_ms);
        else view.latency_ms = NAN;
        view.offline_by_latency = state.display == Display::Unusable;
        view.offline_since_unix = state.outage_active ? state.outage_unix : 0;
        return view;
    }

    // Caller holds status_cs_: power transitions and event publication stay ordered.
    void PublishEvent(const std::wstring& log, std::wstring notification, bool recovered) {
        if (!log.empty()) AppendLog(log);
        if (!notification.empty() && !Stopping() && cb_) {
            NetEvent event;
            event.recovered = recovered;
            event.message = std::move(notification);
            try { cb_(event, user_); } catch (...) {}
        }
    }

    bool ApplyLinkDisconnected(std::uint64_t epoch) {
        CsLock lock(&status_cs_);
        if (suspended_ || resume_pending_ || epoch != power_epoch_ ||
            network_pending_.load(std::memory_order_acquire)) return false;
        Status& state = status_;
        state.has_result = true;
        state.latest_ok = false;
        state.latest_ms = 0;
        state.display = Display::Offline;
        state.link_disconnected = true;
        state.fail_count = static_cast<unsigned>(config_.fail_threshold);
        if (state.outage_active) {
            view_ = MakeView(state);
            return false; // Preserve an existing outage's start and avoid duplicate cards.
        }
        SYSTEMTIME now{};
        GetLocalTime(&now);
        state.first_failure_tick = GetTickCount64();
        state.first_failure_unix = LocalSystemTimeToUnix(now);
        state.first_failure_time = now;
        state.outage_active = true;
        state.outage_unix = state.first_failure_unix;
        view_ = MakeView(state);
        PublishEvent(L"网络断开：网络连接已断开",
                     LocalTimeText(now, false) + L" 起网络连接已断开", false);
        return true;
    }

    void ApplyResult(bool ok, ULONGLONG ms, bool timed_out, std::uint64_t epoch) {
        CsLock lock(&status_cs_);
        if (suspended_ || epoch != power_epoch_ ||
            network_pending_.load(std::memory_order_acquire)) return;
        // A local proxy/TUN response cannot recover a confirmed physical outage.
        // This query is confined to recovery attempts after physical disconnection.
        if (ok && status_.link_disconnected && detail::AllPhysicalNicsDisconnected()) {
            ok = false;
            ms = 0;
        }
        if (network_pending_.load(std::memory_order_acquire)) return;
        resume_pending_ = false;
        SYSTEMTIME now{};
        GetLocalTime(&now);
        const ULONGLONG tick = GetTickCount64();
        std::wstring log;
        std::wstring notification;
        bool recovered = false;
        {
            Status& state = status_;
            state.has_result = true;
            state.latest_ok = ok;
            state.latest_ms = ms;
            if (ok && ms < static_cast<ULONGLONG>(config_.unusable_ms)) {
                if (state.outage_active) {
                    const ULONGLONG duration = tick - state.first_failure_tick;
                    log = L"\u7F51\u7EDC\u5DF2\u6062\u590D\uFF0C\u65AD\u7F51\u65F6\u957F " + FormatOutageDuration(duration / 1000);
                    notification = L"\u7F51\u7EDC\u5DF2\u6062\u590D \u00B7 " + LocalTimeText(now, false) +
                                   L" \u00B7 \u672C\u6B21\u65AD\u7F51 " + FormatOutageDuration(duration / 1000);
                    recovered = true;
                    state.outage_active = false;
                } else if (state.fail_count > 0) {
                    log = L"\u77ED\u6682\u6CE2\u52A8\uFF08\u5931\u8D25 " + std::to_wstring(state.fail_count) +
                          L" \u6B21\uFF09\u540E\u6062\u590D";
                }
                state.fail_count = 0;
                state.first_failure_tick = 0;
                state.first_failure_unix = 0;
                state.link_disconnected = false;
                state.display = ms >= static_cast<ULONGLONG>(config_.slow_ms) ? Display::Slow
                                                                               : Display::Online;
            } else {
                if (state.fail_count < 0xFFFFFFFFu) ++state.fail_count;
                if (state.fail_count == 1) {
                    state.first_failure_tick = tick;
                    state.first_failure_unix = LocalSystemTimeToUnix(now);
                    state.first_failure_time = now;
                }
                if (state.fail_count == static_cast<unsigned>(config_.fail_threshold)) {
                    state.outage_active = true;
                    state.outage_unix = state.first_failure_unix;
                    notification = LocalTimeText(state.first_failure_time, false) + L" 起无法访问网络";
                    if (ok) {
                        log = L"网络断开：延迟过高（" + std::to_wstring(ms) + L" ms）";
                        notification += L"（延迟 " + std::to_wstring(ms) + L" ms）";
                    } else {
                        log = timed_out ? L"网络断开：连接超时" : L"网络断开：无响应";
                        if (timed_out) notification += L"（连接超时）";
                    }
                }
                if (state.fail_count >= static_cast<unsigned>(config_.fail_threshold)) {
                    state.display = ok && !state.link_disconnected ? Display::Unusable : Display::Offline;
                } else if (ok) {
                    state.display = Display::Slow;
                }
            }
            view_ = MakeView(state);
        }
        // Serialize publication with OnSuspend/OnResume: after either returns,
        // the UI can safely drain every notification from the previous epoch.
        PublishEvent(log, std::move(notification), recovered);
    }

    void Run() {
        AppendLog(L"启动监测");
        std::uint64_t epoch = 0;
        ULONGLONG next_due = 0, last_start = 0, link_check_due = 0;
        bool link_was_disconnected = false;
        bool race_next = false;
        const ULONGLONG normal_interval = static_cast<ULONGLONG>(config_.interval_s) * 1000ULL;
        // Respect custom long intervals as well as the default 5 s cadence.
        const ULONGLONG gap_limit = normal_interval * 2 > kSuspendGapMs
            ? normal_interval * 2 : kSuspendGapMs;
        HANDLE events[] = {stop_event_, wake_event_, network_event_};
        while (!Stopping()) {
            // The flag also survives NIC events consumed inside an HTTP wait.
            const bool network_changed = network_pending_.exchange(false, std::memory_order_acq_rel);
            bool suspended = false, resuming = false, fast = false;
            {
                CsLock lock(&status_cs_);
                // Reset under the same lock as the power handlers, before
                // taking a snapshot, so a suspend/resume wakeup cannot be lost.
                ResetEvent(wake_event_);
                if (epoch != power_epoch_) {
                    epoch = power_epoch_;
                    next_due = resume_not_before_;
                    last_start = 0;
                    race_next = false;
                    link_check_due = 0;
                    link_was_disconnected = false;
                }
                suspended = suspended_;
                resuming = resume_pending_;
                fast = status_.fail_count != 0;
            }
            ULONGLONG now = GetTickCount64();
            if (!suspended && last_start && now - last_start > gap_limit) {
                ResumeAfterGap(epoch);
                continue;
            }
            if (network_changed && !suspended && !resuming) {
                race_next = true;
                link_was_disconnected = detail::AllPhysicalNicsDisconnected();
                link_check_due = GetTickCount64() + kNicDebounceMs;
                // Coalesce IPv4/IPv6 notification bursts; never start two rounds
                // less than 1 s apart. Changes do not reset failure counts.
                const ULONGLONG earliest = last_start && now - last_start < kConfirmIntervalMs
                    ? last_start + kConfirmIntervalMs : now;
                if (next_due > earliest) next_due = earliest;
                now = GetTickCount64();
            }
            if (!suspended && link_check_due && now >= link_check_due) {
                link_check_due = 0;
                const bool disconnected = detail::AllPhysicalNicsDisconnected();
                if (disconnected) {
                    if (link_was_disconnected) {
                        if (ApplyLinkDisconnected(epoch)) {
                            // The outage is published; resume HTTP at the existing
                            // offline cadence without three confirmations.
                            next_due = GetTickCount64() + kOfflineIntervalMs;
                        }
                    } else {
                        // Some drivers publish the IP notification before updating
                        // media state. Confirm that newly observed loss once more.
                        link_check_due = GetTickCount64() + kNicDebounceMs;
                    }
                }
                link_was_disconnected = disconnected;
                now = GetTickCount64();
            }
            if (suspended || link_check_due || now < next_due) {
                // A one-shot debounce shares this worker's existing event wait;
                // it does not add a timer, a thread or healthy-network polling.
                const ULONGLONG due = link_check_due ? link_check_due : next_due;
                const DWORD delay = suspended ? INFINITE :
                    due > now ? static_cast<DWORD>(due - now) : 0;
                const DWORD waited = WaitForMultipleObjects(3, events, FALSE, delay);
                if (waited == WAIT_OBJECT_0 || waited == WAIT_FAILED) break;
                continue;
            }

            last_start = now;
            const std::int64_t round_start = ProbeClockNow();
            ULONGLONG ms = 0;
            bool timed_out = false;
            // Once unavailable or after a NIC change, race all addresses within
            // one 1.5 s deadline. Steady healthy operation still uses only the
            // primary (and falls back only on failure), preserving its power use.
            const bool parallel = fast || race_next;
            race_next = false;
            bool ok = !config_.urls.empty() &&
                ProbeUrls(0, parallel ? config_.urls.size() : 1, ms, round_start, timed_out);
            if (!parallel && !ok && !Interrupted() && config_.urls.size() > 1)
                ok = ProbeUrls(1, config_.urls.size(), ms, round_start, timed_out);
            if (Stopping()) break;
            // Includes sleep that occurred inside an async WinHTTP wait, even
            // when WM_POWERBROADCAST was missed. Never apply that stale result.
            if (GetTickCount64() - last_start > kSuspendGapMs) {
                ResumeAfterGap(epoch);
                continue;
            }
            if (Interrupted()) continue;
            ApplyResult(ok, ok ? ms : 0, timed_out, epoch);
            {
                CsLock lock(&status_cs_);
                const ULONGLONG interval = status_.outage_active ? kOfflineIntervalMs :
                    status_.fail_count ? kConfirmIntervalMs :
                    static_cast<ULONGLONG>(config_.interval_s) * 1000ULL;
                // Cadence is measured start-to-start. If a round consumes its
                // interval, retry immediately instead of adding another delay.
                next_due = last_start + interval;
            }
        }
        AppendLog(L"退出监测");
        CloseSession();
    }

    Config config_{};
    Status status_{};
    NetProbe::View view_{};
    mutable CRITICAL_SECTION status_cs_{};
    bool cs_ready_ = false;
    std::wstring log_path_;
    Metrics::EventCallback cb_ = nullptr;
    void* user_ = nullptr;
    // Unlike stop_event_, these remain valid through Start/Stop and NIC callbacks.
    HANDLE wake_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE network_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::atomic<bool> network_pending_{false};
    bool suspended_ = false, resume_pending_ = false; // Protected by status_cs_.
    std::uint64_t power_epoch_ = 1;
    ULONGLONG resume_not_before_ = 0;
    HANDLE stop_event_ = nullptr;
    HANDLE unloaded_event_ = nullptr;
    HANDLE thread_ = nullptr;
    HINTERNET session_ = nullptr;
    bool unload_set_ = false;
};

NetProbe::NetProbe() : impl_(new Impl) {}

NetProbe::~NetProbe() { delete impl_; }

void NetProbe::Start(const Settings& settings, Metrics::EventCallback cb, void* user) {
    impl_->Start(settings, cb, user);
}

void NetProbe::Stop() { impl_->Stop(); }

std::wstring NetProbe::LogPath() const { return impl_->LogPath(); }

NetProbe::View NetProbe::Get() const { return impl_->Get(); }

void NetProbe::SetSlowThreshold(int slow_ms) { impl_->SetSlowThreshold(slow_ms); }

void NetProbe::OnSuspend() { impl_->OnSuspend(); }

void NetProbe::OnResume() { impl_->OnResume(); }

void NetProbe::RequestProbe() { impl_->RequestProbe(); }

}  // namespace pulse

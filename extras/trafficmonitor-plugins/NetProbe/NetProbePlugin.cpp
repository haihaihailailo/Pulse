// NetProbe for TrafficMonitor, API 8. Source encoding: UTF-8.
// Build with MinGW-w64 g++ (C++17); no MFC/ATL or MSVC extensions.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0603
#endif
#define NOMINMAX
#define __USE_MINGW_ANSI_STDIO 1

#include <windows.h>
#include <winhttp.h>
#include <mmsystem.h>
#include <shellapi.h>
#include "PluginInterface.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr UINT kNotifyMessage = WM_APP + 0x3A7;
constexpr COLORREF kNoLabelColor = 0xFFFFFFFFu;
constexpr wchar_t kWindowClass[] = L"NetProbe.NotificationWindow.1.0.0";
constexpr wchar_t kSample[] = L"不可用 4294967295 ms";
constexpr wchar_t kLabel[] = L"延迟 ";
constexpr wchar_t kSound[] = L"C:\\Windows\\Media\\Windows Notify System Generic.wav";

enum class Display : std::uint64_t { Checking, Online, Slow, Offline, Unusable };

std::int64_t ProbeClockNow() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

ULONGLONG ProbeElapsedMs(std::int64_t start, std::int64_t end) {
    return static_cast<ULONGLONG>((end - start) / 1000000);
}

std::uint64_t Pack(Display display, ULONGLONG ms) {
    return (std::min<ULONGLONG>(ms, 0xFFFFFFFFULL) << 3) |
           static_cast<std::uint64_t>(display);
}

Display DisplayOf(std::uint64_t value) { return static_cast<Display>(value & 7); }

std::wstring ValueText(std::uint64_t value) {
    const std::wstring ms = std::to_wstring(value >> 3) + L" ms";
    switch (DisplayOf(value)) {
    case Display::Online: return ms;
    case Display::Slow: return L"较慢 " + ms;
    case Display::Offline: return L"断网";
    case Display::Unusable: return L"不可用 " + ms;
    default: return L"检测中";
    }
}

COLORREF ValueColor(std::uint64_t value) {
    switch (DisplayOf(value)) {
    case Display::Online: return RGB(0x22, 0xC5, 0x5E);
    case Display::Slow: return RGB(0xF5, 0x9E, 0x0B);
    case Display::Offline:
    case Display::Unusable: return RGB(0xEF, 0x44, 0x44);
    default: return RGB(0x94, 0xA3, 0xB8);
    }
}

std::wstring LocalTimeText(const SYSTEMTIME& time, bool date) {
    wchar_t buffer[64]{};
    if (date) {
        std::swprintf(buffer, std::size(buffer), L"%04u-%02u-%02u %02u:%02u:%02u",
                      unsigned(time.wYear), unsigned(time.wMonth), unsigned(time.wDay),
                      unsigned(time.wHour), unsigned(time.wMinute), unsigned(time.wSecond));
    } else {
        std::swprintf(buffer, std::size(buffer), L"%02u:%02u",
                      unsigned(time.wHour), unsigned(time.wMinute));
    }
    return buffer;
}

// Matches Format-Duration in NetMonitor.ps1, including hour/minute truncation.
std::wstring DurationText(ULONGLONG milliseconds) {
    const ULONGLONG seconds = milliseconds / 1000;
    if (seconds >= 3600)
        return std::to_wstring(seconds / 3600) + L" 小时 " +
               std::to_wstring((seconds / 60) % 60) + L" 分";
    if (seconds >= 60)
        return std::to_wstring((seconds / 60) % 60) + L" 分 " +
               std::to_wstring(seconds % 60) + L" 秒";
    return std::to_wstring(seconds) + L" 秒";
}

std::wstring NotificationDuration(ULONGLONG milliseconds) {
    const ULONGLONG seconds = milliseconds / 1000;
    return std::to_wstring(seconds / 60) + L" 分 " +
           std::to_wstring(seconds % 60) + L" 秒";
}

std::wstring JoinPath(std::wstring directory, const wchar_t* filename) {
    if (!directory.empty() && directory.back() != L'\\' && directory.back() != L'/')
        directory += L'\\';
    return directory + filename;
}

bool WriteAll(HANDLE file, const void* buffer, DWORD count) {
    auto bytes = static_cast<const unsigned char*>(buffer);
    while (count) {
        DWORD written{};
        if (!WriteFile(file, bytes, count, &written, nullptr) || written == 0) return false;
        bytes += written;
        count -= written;
    }
    return true;
}

void PlayAlert() {
    const DWORD attributes = GetFileAttributesW(kSound);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        !PlaySoundW(kSound, nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT))
        MessageBeep(MB_ICONASTERISK);
}

struct Config {
    int intervalSeconds = 5;
    int timeoutSeconds = 4;
    int failThreshold = 3;
    int slowMs = 300;
    int unusableMs = 1000;
    std::array<std::wstring, 3> urls{{
        L"http://www.msftconnecttest.com/connecttest.txt",
        L"http://www.gstatic.com/generate_204",
        L"http://connect.rom.miui.com/generate_204"
    }};
};

int ReadInteger(const std::wstring& file, const wchar_t* key, int fallback,
                int minimum, int maximum) {
    wchar_t text[64]{};
    GetPrivateProfileStringW(L"NetProbe", key, std::to_wstring(fallback).c_str(),
                             text, DWORD(std::size(text)), file.c_str());
    wchar_t* end{};
    errno = 0;
    const long long value = std::wcstoll(text, &end, 10);
    if (end == text || errno == ERANGE) return fallback;
    while (*end && std::iswspace(*end)) ++end;
    if (*end || value < minimum || value > maximum) return fallback;
    return static_cast<int>(value);
}

bool WriteDefaultConfig(const std::wstring& file, const Config& config) {
    // A Unicode BOM makes the Win32 profile APIs preserve non-ASCII URLs.
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_EXISTS;
    const unsigned char bom[]{0xFF, 0xFE};
    bool ok = WriteAll(handle, bom, sizeof(bom));
    CloseHandle(handle);
    if (!ok) return false;
    const auto write = [&](const wchar_t* key, const std::wstring& value) {
        return WritePrivateProfileStringW(L"NetProbe", key, value.c_str(), file.c_str()) != FALSE;
    };
    ok = write(L"IntervalSeconds", std::to_wstring(config.intervalSeconds)) && ok;
    ok = write(L"TimeoutSeconds", std::to_wstring(config.timeoutSeconds)) && ok;
    ok = write(L"FailThreshold", std::to_wstring(config.failThreshold)) && ok;
    ok = write(L"SlowMs", std::to_wstring(config.slowMs)) && ok;
    ok = write(L"UnusableMs", std::to_wstring(config.unusableMs)) && ok;
    ok = write(L"Url1", config.urls[0]) && ok;
    ok = write(L"Url2", config.urls[1]) && ok;
    ok = write(L"Url3", config.urls[2]) && ok;
    return ok;
}

Config ReadConfig(const std::wstring& file) {
    Config config;
    config.intervalSeconds = ReadInteger(file, L"IntervalSeconds", 5, 1, 86400);
    config.timeoutSeconds = ReadInteger(file, L"TimeoutSeconds", 4, 1, 3600);
    config.failThreshold = ReadInteger(file, L"FailThreshold", 3, 1, 1000000);
    config.slowMs = ReadInteger(file, L"SlowMs", 300, 1, 2147483646);
    config.unusableMs = ReadInteger(file, L"UnusableMs", 1000, 2, 2147483647);
    if (config.slowMs >= config.unusableMs) {
        config.slowMs = 300;
        config.unusableMs = 1000;
    }
    const wchar_t* keys[]{L"Url1", L"Url2", L"Url3"};
    for (std::size_t i = 0; i < config.urls.size(); ++i) {
        wchar_t buffer[4096]{};
        GetPrivateProfileStringW(L"NetProbe", keys[i], config.urls[i].c_str(), buffer,
                                 DWORD(std::size(buffer)), file.c_str());
        std::wstring url(buffer);
        const auto first = url.find_first_not_of(L" \t\r\n");
        const auto last = url.find_last_not_of(L" \t\r\n");
        url = first == std::wstring::npos ? L"" : url.substr(first, last - first + 1);
        // Url1 always has a target; empty backups are intentionally disabled.
        if (!url.empty() || i != 0) config.urls[i] = std::move(url);
    }
    return config;
}

struct Status {
    std::uint64_t display = Pack(Display::Checking, 0);
    bool hasResult = false;
    bool latestOk = false;
    bool hasResponse = false;
    ULONGLONG latestMs = 0;
    ULONGLONG lastResponseMs = 0;
    unsigned int failCount = 0;
    SYSTEMTIME firstFailureTime{};
    ULONGLONG firstFailureTick = 0;
    bool hasOutage = false;
    bool outageActive = false;
    SYSTEMTIME lastOutageTime{};
    ULONGLONG lastOutageTick = 0;
    ULONGLONG lastOutageDuration = 0;
};

struct ProbeContext {
    HANDLE progress = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE closed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<bool> sent{false};
    std::atomic<bool> headers{false};
    std::atomic<bool> failed{false};
    std::atomic<std::int64_t> responseTick{0};
    ~ProbeContext() {
        if (progress) CloseHandle(progress);
        if (closed) CloseHandle(closed);
    }
};

class LatencyItem final : public IPluginItem {
public:
    LatencyItem(const std::atomic<std::uint64_t>& display,
                const std::atomic<COLORREF>& labelColor)
        : display_(display), labelColor_(labelColor) {}

    const wchar_t* GetItemName() const override { return L"网络探测延迟"; }
    const wchar_t* GetItemId() const override { return L"yftx.NetProbe.Latency"; }
    const wchar_t* GetItemLableText() const override { return L"延迟"; }
    const wchar_t* GetItemValueSampleText() const override { return kSample; }
    const wchar_t* GetItemValueText() const override {
        // Each caller owns its returned buffer; a worker never mutates this string.
        // Trivial TLS also avoids deferred C++ string destructors after DLL unload.
        thread_local wchar_t buffer[96]{};
        const std::wstring text = ValueText(display_.load(std::memory_order_acquire));
        std::wcsncpy(buffer, text.c_str(), std::size(buffer) - 1);
        buffer[std::size(buffer) - 1] = L'\0';
        return buffer;
    }
    bool IsCustomDraw() const override { return true; }
    int GetItemWidth() const override { return 250; }
    int GetItemWidthEx(void* dc) const override {
        if (!dc) return GetItemWidth();
        SIZE size{};
        const std::wstring sample = std::wstring(kLabel) + kSample;
        if (!GetTextExtentPoint32W(static_cast<HDC>(dc), sample.c_str(),
                                  static_cast<int>(sample.size()), &size))
            return MulDiv(GetItemWidth(), GetDeviceCaps(static_cast<HDC>(dc), LOGPIXELSX), 96);
        return size.cx + std::max(4L, size.cy / 2);
    }
    bool DrawItemEx(IPluginDrawer* drawer, int x, int y, int w, int h, bool dark) override {
        if (!drawer) return false;
        if (w <= 0 || h <= 0) return true;
        const auto value = display_.load(std::memory_order_acquire);
        const auto text = ValueText(value);
        int labelWidth{}, labelHeight{};
        drawer->GetTextExtent(kLabel, labelWidth, labelHeight);
        labelWidth = std::clamp(labelWidth, 0, w);
        drawer->DrawWindowText(x, y, labelWidth, h, kLabel, LabelColor(dark));
        if (w > labelWidth)
            drawer->DrawWindowText(x + labelWidth, y, w - labelWidth, h,
                                   text.c_str(), ValueColor(value));
        return true;
    }
    void DrawItem(void* context, int x, int y, int w, int h, bool dark) override {
        HDC dc = static_cast<HDC>(context);
        if (!dc || w <= 0 || h <= 0) return;
        const int saved = SaveDC(dc);
        if (!saved) return;
        IntersectClipRect(dc, x, y, x + w, y + h);
        SetBkMode(dc, TRANSPARENT);
        SIZE label{};
        GetTextExtentPoint32W(dc, kLabel, int(std::size(kLabel) - 1), &label);
        const int labelWidth = std::clamp(static_cast<int>(label.cx), 0, w);
        RECT rectangle{x, y, x + labelWidth, y + h};
        const UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS;
        SetTextColor(dc, LabelColor(dark));
        DrawTextW(dc, kLabel, -1, &rectangle, flags);
        const auto value = display_.load(std::memory_order_acquire);
        const auto text = ValueText(value);
        rectangle.left = x + labelWidth;
        rectangle.right = x + w;
        SetTextColor(dc, ValueColor(value));
        DrawTextW(dc, text.c_str(), -1, &rectangle, flags);
        RestoreDC(dc, saved);
    }

private:
    COLORREF LabelColor(bool dark) const {
        const COLORREF color = labelColor_.load(std::memory_order_relaxed);
        return color == kNoLabelColor ? (dark ? RGB(240, 240, 240) : RGB(32, 32, 32)) : color;
    }
    const std::atomic<std::uint64_t>& display_;
    const std::atomic<COLORREF>& labelColor_;
};

class NetProbePlugin final : public ITMPlugin {
public:
    NetProbePlugin() : item_(display_, labelColor_) {}
    ~NetProbePlugin() {
        Stop();
        if (window_) DestroyWindow(window_);
        if (windowClass_) UnregisterClassW(kWindowClass, module_);
    }

    IPluginItem* GetItem(int index) override { return index == 0 ? &item_ : nullptr; }
    void DataRequired() override {
        // Lock-free on x86_64: no networking, file I/O, UI calls, allocation or waiting.
        (void)display_.load(std::memory_order_acquire);
    }
    const wchar_t* GetInfo(PluginInfoIndex index) override {
        switch (index) {
        case TMI_NAME: return L"网络探测 NetProbe";
        case TMI_DESCRIPTION: return L"通过系统代理探测 HTTP 延迟，提供断网与恢复通知、提示音和日志。";
        case TMI_AUTHOR: return L"yftx";
        case TMI_COPYRIGHT: return L"Copyright (C) yftx";
        case TMI_VERSION: return L"1.0.0";
        default: return L"";
        }
    }
    const wchar_t* GetTooltipInfo() override {
        thread_local Status cached;
        thread_local wchar_t buffer[1024]{};
        std::wstring text;
        {
            std::unique_lock<std::mutex> lock(stateMutex_, std::try_to_lock);
            if (lock.owns_lock()) cached = status_;
        }
        text = L"最近延迟：";
        if (!cached.hasResult) text += L"检测中";
        else if (cached.latestOk) text += std::to_wstring(cached.latestMs) + L" ms";
        else {
            text += L"无响应";
            if (cached.hasResponse)
                text += L"（上次响应 " + std::to_wstring(cached.lastResponseMs) + L" ms）";
        }
        text += L"\n状态：";
        switch (DisplayOf(cached.display)) {
        case Display::Online: text += L"在线"; break;
        case Display::Slow: text += L"较慢"; break;
        case Display::Offline: text += L"断网"; break;
        case Display::Unusable: text += L"断网（响应延迟过高）"; break;
        default: text += L"检测中"; break;
        }
        text += L"\n连续不可用次数：" + std::to_wstring(cached.failCount);
        if (cached.hasOutage) {
            const ULONGLONG duration = cached.outageActive
                ? GetTickCount64() - cached.lastOutageTick : cached.lastOutageDuration;
            text += L"\n上次断网时间：" + LocalTimeText(cached.lastOutageTime, true);
            text += L"\n上次断网时长：" + DurationText(duration);
            if (cached.outageActive) text += L"（持续中）";
        } else text += L"\n上次断网时间：无\n上次断网时长：无";
        std::wcsncpy(buffer, text.c_str(), std::size(buffer) - 1);
        buffer[std::size(buffer) - 1] = L'\0';
        return buffer;
    }
    void OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) override {
        if (index != EI_LABEL_TEXT_COLOR || !data) return;
        wchar_t* end{};
        const int radix = (data[0] == L'0' && (data[1] == L'x' || data[1] == L'X')) ? 16 : 10;
        errno = 0;
        const unsigned long color = std::wcstoul(data, &end, radix);
        if (end != data && *end == L'\0' && errno != ERANGE && color <= 0xFFFFFFUL)
            labelColor_.store(static_cast<COLORREF>(color), std::memory_order_relaxed);
    }
    int GetCommandCount() override { return 3; }
    const wchar_t* GetCommandName(int index) override {
        switch (index) {
        case 0: return L"测试提醒";
        case 1: return L"打开日志";
        case 2: return L"打开配置文件";
        default: return nullptr;
        }
    }
    void OnPluginCommand(int index, void* owner, void*) override {
        if (index == 0) {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            ShowAlert(L"网络已恢复 · " + LocalTimeText(now, false) + L" · 这是一条测试提醒");
        } else if ((index == 1 || index == 2) && !configDirectory_.empty()) {
            const std::wstring& file = index == 1 ? logPath_ : iniPath_;
            const std::wstring argument = L"\"" + file + L"\"";
            const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
                static_cast<HWND>(owner), L"open", L"notepad.exe", argument.c_str(),
                configDirectory_.c_str(), SW_SHOWNORMAL));
            if (result <= 32 && app_) app_->ShowNotifyMessage(L"NetProbe：无法打开文件。请检查配置目录的权限。" );
        }
    }

    void OnInitialize(ITrafficMonitor* app) override {
        if (initialized_ || !app) return;
        initialized_ = true;
        app_ = app;
        const int api = app->GetAPIVersion();
        if (api < 0) return;
        const wchar_t* directory = app->GetPluginConfigDir();
        if (!directory || !*directory) {
            app->ShowNotifyMessage(L"NetProbe：宿主没有提供插件配置目录。" );
            return;
        }
        configDirectory_ = directory;
        iniPath_ = JoinPath(configDirectory_, L"NetProbe.ini");
        logPath_ = JoinPath(configDirectory_, L"NetProbe.log");
        // The host normally creates this directory; creating it again is harmless.
        CreateDirectoryW(configDirectory_.c_str(), nullptr);
        const bool saved = WriteDefaultConfig(iniPath_, Config{});
        config_ = ReadConfig(iniPath_);
        if (!saved) AppendLog(L"配置文件写入失败，缺失项使用默认值");

        module_ = ModuleHandle();
        WNDCLASSW type{};
        type.lpfnWndProc = WindowProcedure;
        type.hInstance = module_;
        type.lpszClassName = kWindowClass;
        windowClass_ = RegisterClassW(&type);
        if (!windowClass_) {
            app->ShowNotifyMessage(L"NetProbe：无法创建通知窗口。" );
            return;
        }
        // A child is destroyed before the host main window goes away, allowing
        // ordinary shutdown to finish outside the Windows loader lock.
        HWND parent = api >= 1 ? static_cast<HWND>(app->GetMainWindowHwnd()) : nullptr;
        const bool child = parent && GetWindowThreadProcessId(parent, nullptr) == GetCurrentThreadId();
        window_ = CreateWindowExW(0, kWindowClass, L"NetProbe", child ? WS_CHILD : 0,
                                  0, 0, 0, 0, child ? parent : HWND_MESSAGE,
                                  nullptr, module_, this);
        if (!window_ || !Start()) {
            Stop();
            AppendLog(L"检测出错：后台探测启动失败");
            app->ShowNotifyMessage(L"NetProbe：后台探测启动失败，请查看日志。" );
        }
    }

private:
    static HMODULE ModuleHandle() {
        HMODULE module{};
        // FROM_ADDRESS does not dereference this string. No reference is pinned.
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           kWindowClass, &module);
        return module;
    }
    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto self = reinterpret_cast<NetProbePlugin*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<NetProbePlugin*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self) {
            if (message == kNotifyMessage) {
                self->DeliverNotifications();
                return 0;
            }
            if (message == WM_DESTROY) self->Stop();
            if (message == WM_NCDESTROY) {
                self->window_ = nullptr;
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            }
        }
        return DefWindowProcW(window, message, wp, lp);
    }
    void AppendLog(const std::wstring& message) const {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        const std::wstring line = L"[" + LocalTimeText(now, true) + L"] " + message + L"\r\n";
        const int count = WideCharToMultiByte(CP_UTF8, 0, line.c_str(),
                                             static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
        if (count <= 0) return;
        std::string utf8(static_cast<std::size_t>(count), char{});
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                            utf8.data(), count, nullptr, nullptr);
        HANDLE file = CreateFileW(logPath_.c_str(), FILE_APPEND_DATA | FILE_READ_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER size{};
        if (GetFileSizeEx(file, &size) && size.QuadPart == 0) {
            const unsigned char bom[]{0xEF, 0xBB, 0xBF};
            WriteAll(file, bom, sizeof(bom));
        }
        WriteAll(file, utf8.data(), static_cast<DWORD>(utf8.size()));
        CloseHandle(file);
    }
    void ShowAlert(const std::wstring& message) {
        if (app_) app_->ShowNotifyMessage(message.c_str());
        PlayAlert();
    }
    void QueueNotification(std::wstring message) {
        {
            std::lock_guard<std::mutex> lock(notificationMutex_);
            notifications_.push_back(std::move(message));
        }
        if (window_) PostMessageW(window_, kNotifyMessage, 0, 0);
    }
    void DeliverNotifications() {
        std::vector<std::wstring> pending;
        {
            std::lock_guard<std::mutex> lock(notificationMutex_);
            pending.swap(notifications_);
        }
        for (const auto& message : pending) ShowAlert(message);
    }
    bool Stopping() const { return stopEvent_ && WaitForSingleObject(stopEvent_, 0) != WAIT_TIMEOUT; }

    bool Start() {
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        startedEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        doneEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        unloadedEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stopEvent_ || !startedEvent_ || !doneEvent_ || !unloadedEvent_) return false;
        session_ = WinHttpOpen(L"NetProbe/1.0.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
        if (!session_)
            session_ = WinHttpOpen(L"NetProbe/1.0.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
        if (!session_) { AppendLog(L"检测出错：WinHTTP 会话创建失败"); return false; }
        if (!WinHttpSetOption(session_, WINHTTP_OPTION_UNLOAD_NOTIFY_EVENT,
                              &unloadedEvent_, sizeof(unloadedEvent_))) return false;
        unloadNotificationSet_ = true;
        const int timeout = config_.timeoutSeconds * 1000;
        if (!WinHttpSetTimeouts(session_, timeout, timeout, timeout, timeout)) return false;

        // Prestart the private worker outside DllMain. Asynchronous WinHTTP waits
        // share the stop event and a total deadline, without a polling/sleep loop.
        pool_ = CreateThreadpool(nullptr);
        if (!pool_) return false;
        SetThreadpoolThreadMaximum(pool_, 1);
        if (!SetThreadpoolThreadMinimum(pool_, 1)) return false;
        InitializeThreadpoolEnvironment(&environment_);
        environmentInitialized_ = true;
        SetThreadpoolCallbackPool(&environment_, pool_);
        work_ = CreateThreadpoolWork(WorkerCallback, this, &environment_);
        if (!work_) return false;
        SubmitThreadpoolWork(work_);
        workSubmitted_ = true;
        return WaitForSingleObject(startedEvent_, INFINITE) == WAIT_OBJECT_0;
    }
    void Stop() {
        // Called on the initialization/UI thread (and, as a fallback, at unload).
        // No worker ever sends synchronous messages to that thread.
        if (stopEvent_) SetEvent(stopEvent_);
        if (workSubmitted_) {
            // This event is set by Windows AFTER WorkerCallback has returned.
            // Waiting for a std::thread HANDLE in DllMain would deadlock at detach.
            WaitForSingleObject(doneEvent_, INFINITE);
            workSubmitted_ = false;
        }
        if (work_) { CloseThreadpoolWork(work_); work_ = nullptr; }
        CloseSession();
        if (environmentInitialized_) {
            DestroyThreadpoolEnvironment(&environment_);
            environmentInitialized_ = false;
        }
        if (pool_) { CloseThreadpool(pool_); pool_ = nullptr; }
        for (HANDLE* event : {&startedEvent_, &doneEvent_, &stopEvent_, &unloadedEvent_}) {
            if (*event) { CloseHandle(*event); *event = nullptr; }
        }
    }
    static void CALLBACK WorkerCallback(PTP_CALLBACK_INSTANCE instance, void* context, PTP_WORK) {
        auto self = static_cast<NetProbePlugin*>(context);
        SetEventWhenCallbackReturns(instance, self->doneEvent_);
        SetEvent(self->startedEvent_);
        try {
            self->Run();
        } catch (...) {
            // No exception may escape a Win32 callback.
            try { self->AppendLog(L"检测出错：后台探测发生异常，监测已停止"); } catch (...) {}
        }
        self->CloseSession();
    }
    void CloseSession() {
        if (!session_) return;
        WinHttpCloseHandle(session_);
        session_ = nullptr;
        // HANDLE_CLOSING is the last request notification; the session event also
        // guarantees that every WinHTTP callback has RETURNED before DLL unload.
        if (unloadNotificationSet_) WaitForSingleObject(unloadedEvent_, INFINITE);
    }
    static void CALLBACK ProbeCallback(HINTERNET, DWORD_PTR context, DWORD status,
                                       void*, DWORD) {
        if (!context) return;
        auto probe = reinterpret_cast<ProbeContext*>(context);
        switch (status) {
        case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
            probe->sent.store(true, std::memory_order_release);
            SetEvent(probe->progress);
            break;
        case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
            probe->responseTick.store(ProbeClockNow(), std::memory_order_relaxed);
            probe->headers.store(true, std::memory_order_release);
            SetEvent(probe->progress);
            break;
        case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
            probe->failed.store(true, std::memory_order_release);
            SetEvent(probe->progress);
            break;
        case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
            // This is the final access to this context, in the last notification.
            SetEvent(probe->closed);
            break;
        default: break;
        }
    }
    bool AwaitPhase(ProbeContext& probe, const std::atomic<bool>& complete,
                    std::int64_t start, ULONGLONG budget) const {
        const HANDLE events[]{stopEvent_, probe.progress};
        for (;;) {
            // Reset BEFORE testing flags so a simultaneous notification is never lost.
            ResetEvent(probe.progress);
            if (Stopping() || probe.failed.load(std::memory_order_acquire)) return false;
            if (complete.load(std::memory_order_acquire)) return true;
            const ULONGLONG elapsed = ProbeElapsedMs(start, ProbeClockNow());
            if (elapsed >= budget) return false;
            if (WaitForMultipleObjects(DWORD(std::size(events)), events, FALSE,
                                       static_cast<DWORD>(budget - elapsed)) != WAIT_OBJECT_0 + 1)
                return false;
        }
    }
    static bool PendingOrSuccessful(BOOL result) {
        return result || GetLastError() == ERROR_IO_PENDING;
    }
    bool ProbeUrl(const std::wstring& url, ULONGLONG& elapsed) {
        if (Stopping()) return false;
        const std::int64_t start = ProbeClockNow(); // Reset for EVERY address.
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        parts.dwUrlPathLength = static_cast<DWORD>(-1);
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) ||
            (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS))
            return false;
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring target;
        if (parts.dwUrlPathLength) target.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (target.empty()) target = L"/";
        if (parts.dwExtraInfoLength) target.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        const auto fragment = target.find(L'#');
        if (fragment != std::wstring::npos) target.erase(fragment);
        ProbeContext probe;
        if (!probe.progress || !probe.closed) return false;
        HINTERNET connection = WinHttpConnect(session_, host.c_str(), parts.nPort, 0);
        if (!connection) return false;
        const DWORD flags = WINHTTP_FLAG_REFRESH |
            (parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        HINTERNET request = WinHttpOpenRequest(connection, L"GET", target.c_str(), nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!request) { WinHttpCloseHandle(connection); return false; }
        DWORD disabled = WINHTTP_DISABLE_REDIRECTS;
        DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&probe);
        const bool prepared = WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE,
                                               &disabled, sizeof(disabled)) &&
            WinHttpSetOption(request, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)) &&
            WinHttpAddRequestHeaders(request, L"Connection: close\r\n", static_cast<DWORD>(-1),
                                      WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        if (!prepared || Stopping()) {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            return false;
        }
        const DWORD callbacks = WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE |
                                WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE |
                                WINHTTP_CALLBACK_FLAG_REQUEST_ERROR |
                                WINHTTP_CALLBACK_FLAG_HANDLES;
        if (WinHttpSetStatusCallback(request, ProbeCallback, callbacks, 0) ==
            WINHTTP_INVALID_STATUS_CALLBACK) {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            return false;
        }
        const ULONGLONG budget = ULONGLONG(config_.timeoutSeconds) * 1000;
        bool ok = false;
        if (ProbeElapsedMs(start, ProbeClockNow()) < budget && !Stopping()) {
            if (PendingOrSuccessful(WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                                       WINHTTP_NO_REQUEST_DATA, 0, 0, context)) &&
                AwaitPhase(probe, probe.sent, start, budget) &&
                ProbeElapsedMs(start, ProbeClockNow()) < budget &&
                !Stopping() &&
                PendingOrSuccessful(WinHttpReceiveResponse(request, nullptr)) &&
                AwaitPhase(probe, probe.headers, start, budget)) {
                elapsed = ProbeElapsedMs(start, probe.responseTick.load(std::memory_order_acquire));
                // Any HTTP status counts. Do NOT read the body or follow redirects.
                ok = elapsed <= budget;
            }
        }
        // Cancel an asynchronous request on stop/timeout; its context remains alive
        // through HANDLE_CLOSING. No synchronous request is closed from another thread.
        WinHttpCloseHandle(request);
        WaitForSingleObject(probe.closed, INFINITE);
        WinHttpCloseHandle(connection);
        return ok && !Stopping();
    }
    void ApplyResult(bool ok, ULONGLONG ms) {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        const ULONGLONG tick = GetTickCount64();
        std::wstring log;
        std::wstring notification;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            auto& state = status_;
            state.hasResult = true;
            state.latestOk = ok;
            state.latestMs = ms;
            if (ok) { state.hasResponse = true; state.lastResponseMs = ms; }
            if (ok && ms < static_cast<ULONGLONG>(config_.unusableMs)) {
                if (state.outageActive) {
                    const ULONGLONG duration = tick - state.firstFailureTick;
                    log = L"网络已恢复，断网时长 " + DurationText(duration);
                    notification = L"网络已恢复 · " + LocalTimeText(now, false) +
                                   L" · 本次断网 " + NotificationDuration(duration);
                    state.outageActive = false;
                    state.lastOutageDuration = duration;
                } else if (state.failCount > 0) {
                    log = L"短暂波动（失败 " + std::to_wstring(state.failCount) + L" 次）后恢复";
                }
                state.failCount = 0;
                state.firstFailureTime = SYSTEMTIME{};
                state.firstFailureTick = 0;
                state.display = Pack(ms >= static_cast<ULONGLONG>(config_.slowMs)
                                         ? Display::Slow : Display::Online, ms);
            } else {
                if (state.failCount < 0xFFFFFFFFu) ++state.failCount;
                if (state.failCount == 1) {
                    state.firstFailureTime = now;
                    state.firstFailureTick = tick;
                }
                if (state.failCount == static_cast<unsigned>(config_.failThreshold)) {
                    state.hasOutage = true;
                    state.outageActive = true;
                    state.lastOutageTime = state.firstFailureTime;
                    state.lastOutageTick = state.firstFailureTick;
                    state.lastOutageDuration = 0;
                    log = ok ? L"网络断开：延迟过高（" + std::to_wstring(ms) + L" ms）"
                             : L"网络断开：无响应";
                    notification = ok ? L"网络断开（延迟过高 " + std::to_wstring(ms) + L" ms）"
                                      : L"网络断开（无响应）";
                }
                if (state.failCount >= static_cast<unsigned>(config_.failThreshold))
                    state.display = Pack(ok ? Display::Unusable : Display::Offline, ms);
                else if (ok) state.display = Pack(Display::Slow, ms);
                // Before confirmation a no-response result preserves the display,
                // exactly like Set-Result in NetMonitor.ps1 (but increments count).
            }
            display_.store(state.display, std::memory_order_release);
        }
        if (!log.empty()) AppendLog(log);
        if (!notification.empty() && !Stopping()) QueueNotification(std::move(notification));
    }
    void Run() {
        AppendLog(L"启动监测");
        while (!Stopping()) {
            const ULONGLONG start = GetTickCount64();
            ULONGLONG ms{};
            bool ok = false;
            for (const auto& url : config_.urls) {
                if (Stopping()) break;
                if (!url.empty() && ProbeUrl(url, ms)) { ok = true; break; }
            }
            if (Stopping()) break;
            ApplyResult(ok, ok ? ms : 0);
            // Cadence is measured from probe start; slow probes never overlap.
            const ULONGLONG elapsed = GetTickCount64() - start;
            const ULONGLONG interval = ULONGLONG(config_.intervalSeconds) * 1000;
            const DWORD remaining = elapsed >= interval ? 0 : static_cast<DWORD>(interval - elapsed);
            if (WaitForSingleObject(stopEvent_, remaining) != WAIT_TIMEOUT) break;
        }
        AppendLog(L"退出监测");
    }

    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::atomic<std::uint64_t> display_{Pack(Display::Checking, 0)};
    std::atomic<COLORREF> labelColor_{kNoLabelColor};
    LatencyItem item_;
    std::mutex stateMutex_;
    Status status_;
    std::mutex notificationMutex_;
    std::vector<std::wstring> notifications_;
    Config config_;
    std::wstring configDirectory_, iniPath_, logPath_;
    ITrafficMonitor* app_{}; // Used only on the host/UI thread.
    HWND window_{};
    HMODULE module_{};
    ATOM windowClass_{};
    HANDLE stopEvent_{}, startedEvent_{}, doneEvent_{}, unloadedEvent_{};
    HINTERNET session_{};
    PTP_POOL pool_{};
    PTP_WORK work_{};
    TP_CALLBACK_ENVIRON environment_{};
    bool environmentInitialized_{};
    bool unloadNotificationSet_{};
    bool workSubmitted_{};
    bool initialized_{};
};

NetProbePlugin* g_plugin{};
} // namespace

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance() {
    // The pointer has no CRT destructor. During process termination other threads
    // may already be gone while holding C++/WinHTTP locks; let Windows reclaim it.
    // Explicit DLL unload, in contrast, destroys the live object below.
    static NetProbePlugin* const plugin = new NetProbePlugin;
    g_plugin = plugin;
    return plugin;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        NetProbePlugin* plugin = g_plugin;
        g_plugin = nullptr;
        delete plugin;
    }
    return TRUE;
}

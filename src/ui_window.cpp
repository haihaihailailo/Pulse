#include "ui_window.h"
#include "ui_renderer.h"
#include "metrics.h"
#include "netprobe.h"
#include <windowsx.h>
#include <shellapi.h>
#ifndef SEE_MASK_NOCLOSEPROCESS
#define SEE_MASK_NOCLOSEPROCESS 0x00000040
#endif
#ifndef SEE_MASK_NOASYNC
#define SEE_MASK_NOASYNC 0x00000100
#endif
#ifndef SEE_MASK_FLAG_NO_UI
#define SEE_MASK_FLAG_NO_UI 0x00000400
#endif
#ifndef MSGFLT_ALLOW
#define MSGFLT_ALLOW 1
#endif
#include <shlobj.h>
#include <mmsystem.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <climits>
#include <cwchar>
#include <memory>
#include <utility>
#include <vector>

namespace pulse::ui {
namespace {
constexpr wchar_t MainClass[] = L"Pulse.Native.Main.v1";
constexpr wchar_t SingleInstanceMutex[] = L"Local\\Pulse.Native.SingleInstance.v1";
constexpr wchar_t RestartedArg[] = L"--restarted";
constexpr wchar_t ToastClass[] = L"Pulse.Native.Notification.v1";
constexpr wchar_t RunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr int kAppIconId = 1;
constexpr UINT SampleTimer = 1;
constexpr UINT TrayMessage = WM_APP + 1;
constexpr UINT NetMessage = WM_APP + 2;
constexpr UINT ShowMessage = WM_APP + 3;
constexpr UINT ReapToastMessage = WM_APP + 4;
constexpr UINT ArrangeToastMessage = WM_APP + 5;
constexpr UINT DpiFitMessage = WM_APP + 6;
constexpr UINT CmdFull = 101, CmdCompact = 102, CmdPin = 103, CmdStartup = 104;
constexpr UINT CmdLog = 105, CmdExit = 106, CmdDefaults = 107, TrayId = 1;
constexpr UINT CmdNetworkNotify = 108, CmdNotifySound = 109, CmdRestartAdmin = 110;
constexpr UINT CmdFullComponent = 200, CmdCompactComponent = 300;
constexpr UINT CmdRefresh = 400;
constexpr UINT CmdSlowThreshold = 450;
constexpr UINT CmdZoom = 500;
constexpr std::array<UINT, 3> RefreshIntervals{{500u, 1000u, 2000u}};
constexpr std::array<int, 7> SlowThresholds{{50, 100, 150, 200, 300, 500, 800}};
constexpr std::array<int, 6> ZoomLevels{{80, 100, 125, 150, 175, 200}};
constexpr DWORD MainStyle = WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX;
constexpr DWORD ToastStyle = WS_POPUP | WS_CAPTION | WS_THICKFRAME;

void AllowShowFromLowerIntegrity(HWND window);

void SetSampleTimer(HWND window, bool compact, UINT fullInterval) {
    using CoalescableTimerFn = UINT_PTR(WINAPI*)(HWND, UINT_PTR, UINT, TIMERPROC, ULONG);
    static const auto coalescable = reinterpret_cast<CoalescableTimerFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetCoalescableTimer"));
    const UINT interval = compact ? 2000u : fullInterval;
    // Full mode prioritizes response time; keep compact mode's existing coalescing.
    const ULONG tolerance = compact ? 300u : 0xffffffffu; // TIMERV_NO_COALESCING
    if (!coalescable || !coalescable(window, SampleTimer, interval, nullptr, tolerance))
        SetTimer(window, SampleTimer, interval, nullptr);
}

int Pixels(float dip, UINT dpi, int zoom = 100) {
    return static_cast<int>(std::ceil(dip * dpi / 96.0f * zoom / 100.0f));
}
int ClampCoordinate(int value, int start, int end, int size) {
    return std::max(start, std::min(value, std::max(start, end - size)));
}
RECT WorkArea(HMONITOR monitor) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) return info.rcWork;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    return work;
}
struct PaintSession {
    HWND window;
    PAINTSTRUCT state{};
    explicit PaintSession(HWND value) : window(value) { BeginPaint(window, &state); }
    ~PaintSession() { EndPaint(window, &state); }
};
std::wstring ExecutablePath() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!length) return {};
        if (length < buffer.size() - 1) return std::wstring(buffer.data(), length);
        if (buffer.size() >= 32768) return {};
        buffer.resize(std::min(size_t(32768), buffer.size() * 2));
    }
}
HICON LoadPulseIcon(int xMetric, int yMetric, UINT dpi) {
    using MetricsForDpiFn = int(WINAPI*)(int, UINT);
    static const auto metricsForDpi = reinterpret_cast<MetricsForDpiFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi"));
    const int cx = metricsForDpi ? metricsForDpi(xMetric, dpi) : GetSystemMetrics(xMetric);
    const int cy = metricsForDpi ? metricsForDpi(yMetric, dpi) : GetSystemMetrics(yMetric);
    return static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kAppIconId),
        IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR));
}
HICON IconOrDefault(HICON icon) {
    // Keep the shared fallback out of the owned icon handles.
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

struct Config {
    int x = INT_MIN, y = INT_MIN;
    UINT refresh_ms = 1000;
    int slow_ms = 500;
    int zoom = 100;
    bool compact = false, pinned = true;
    bool notify = true, notify_sound = true;
    std::wstring file;
    Components components;
    bool ReadComponent(const wchar_t* section, const ComponentInfo& info) const {
        wchar_t value[16]{};
        GetPrivateProfileStringW(section, info.key, L"", value, 16, file.c_str());
        if (std::wcscmp(value, L"0") == 0) return false;
        if (std::wcscmp(value, L"1") == 0) return true;
        return info.enabled;
    }
    bool Load() {
        wchar_t directory[MAX_PATH]{};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, nullptr, SHGFP_TYPE_CURRENT, directory))) return false;
        std::wstring folder = std::wstring(directory) + L"\\Pulse";
        if (!CreateDirectoryW(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
        file = folder + L"\\settings.ini";
        x = static_cast<int>(GetPrivateProfileIntW(L"Window", L"X", INT_MIN, file.c_str()));
        y = static_cast<int>(GetPrivateProfileIntW(L"Window", L"Y", INT_MIN, file.c_str()));
        compact = GetPrivateProfileIntW(L"Window", L"Compact", 0, file.c_str()) != 0;
        pinned = GetPrivateProfileIntW(L"Window", L"OnTop", 1, file.c_str()) != 0;
        wchar_t zoomValue[32]{};
        GetPrivateProfileStringW(L"Window", L"zoom", L"100", zoomValue, 32, file.c_str());
        wchar_t* zoomEnd = nullptr;
        const long zoomPercent = std::wcstol(zoomValue, &zoomEnd, 10);
        zoom = zoomEnd != zoomValue && *zoomEnd == L'\0' && zoomPercent >= 80 && zoomPercent <= 200 ?
            static_cast<int>(zoomPercent) : 100;
        const UINT interval = GetPrivateProfileIntW(L"Window", L"refresh_ms", 1000, file.c_str());
        refresh_ms = std::find(RefreshIntervals.begin(), RefreshIntervals.end(), interval) != RefreshIntervals.end() ? interval : 1000u;
        wchar_t slow[32]{};
        GetPrivateProfileStringW(L"Network", L"slow_ms", L"500", slow, 32, file.c_str());
        slow_ms = 500;
        for (const int threshold : SlowThresholds)
            if (std::to_wstring(threshold) == slow) slow_ms = threshold;
        notify = GetPrivateProfileIntW(L"Network", L"notify", 1, file.c_str()) != 0;
        notify_sound = GetPrivateProfileIntW(L"Network", L"notify_sound", 1, file.c_str()) != 0;
        for (size_t i = 0; i < FullComponents.size(); ++i)
            components.full[i] = ReadComponent(L"Full", FullComponents[i]);
        for (size_t i = 0; i < CompactComponents.size(); ++i)
            components.compact[i] = ReadComponent(L"Compact", CompactComponents[i]);
        return true;
    }
    void Save() const {
        if (file.empty()) return;
        const auto sx = std::to_wstring(x), sy = std::to_wstring(y);
        WritePrivateProfileStringW(L"Window", L"X", sx.c_str(), file.c_str());
        WritePrivateProfileStringW(L"Window", L"Y", sy.c_str(), file.c_str());
        WritePrivateProfileStringW(L"Window", L"Compact", compact ? L"1" : L"0", file.c_str());
        WritePrivateProfileStringW(L"Window", L"OnTop", pinned ? L"1" : L"0", file.c_str());
        const auto zoomValue = std::to_wstring(zoom);
        WritePrivateProfileStringW(L"Window", L"zoom", zoomValue.c_str(), file.c_str());
        const auto interval = std::to_wstring(refresh_ms);
        WritePrivateProfileStringW(L"Window", L"refresh_ms", interval.c_str(), file.c_str());
        const auto slow = std::to_wstring(slow_ms);
        WritePrivateProfileStringW(L"Network", L"slow_ms", slow.c_str(), file.c_str());
        WritePrivateProfileStringW(L"Network", L"notify", notify ? L"1" : L"0", file.c_str());
        WritePrivateProfileStringW(L"Network", L"notify_sound", notify_sound ? L"1" : L"0", file.c_str());
        for (size_t i = 0; i < FullComponents.size(); ++i)
            WritePrivateProfileStringW(L"Full", FullComponents[i].key, components.full[i] ? L"1" : L"0", file.c_str());
        for (size_t i = 0; i < CompactComponents.size(); ++i)
            WritePrivateProfileStringW(L"Compact", CompactComponents[i].key, components.compact[i] ? L"1" : L"0", file.c_str());
    }
};

class Toast {
public:
    explicit Toast(NetEvent event, HWND notify) : event_(std::move(event)), notify_(notify) {}
    ~Toast() { if (window_) DestroyWindow(window_); }
    bool Open(HINSTANCE instance, UINT dpi, int zoom) {
        if (FAILED(renderer_.Initialize())) return false;
        dpi_ = dpi;
        zoom_ = zoom;
        renderer_.SetZoom(zoom_);
        height_ = renderer_.ToastHeight(event_.message);
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
            ToastClass, L"Pulse · 网络提醒", ToastStyle, 0, 0, Width(dpi_), Height(dpi_),
            nullptr, nullptr, instance, this);
        return window_ != nullptr;
    }
    HWND Window() const { return window_; }
    int Width(UINT dpi) const { return Pixels(width_, dpi, zoom_); }
    int Height(UINT dpi) const { return Pixels(height_, dpi, zoom_); }
    void SetScale(UINT dpi, int zoom, int maxWidth) {
        const float scale = dpi / 96.0f * zoom / 100.0f;
        const float width = std::min(300.0f, std::max(1.0f, maxWidth / scale));
        if (dpi_ == dpi && zoom_ == zoom && width_ == width) return;
        dpi_ = dpi;
        zoom_ = zoom;
        renderer_.SetDpi(dpi_);
        renderer_.SetZoom(zoom_);
        width_ = width;
        height_ = renderer_.ToastHeight(event_.message, width_);
        if (window_) InvalidateRect(window_, nullptr, FALSE);
    }
    void Place(int x, int y, int height, UINT dpi) {
        dpi_ = dpi;
        renderer_.SetDpi(dpi);
        SetWindowPos(window_, HWND_TOPMOST, x, y, Width(dpi), height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (!shown_) {
            shown_ = true;
            ArmExpiry(8000);
        }
        if (CursorInside()) PauseExpiry();
        else if (hovered_) {
            hovered_ = false;
            ArmExpiry(3000); // Arrangement/DPI changes may move the card away.
        }
    }
    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<Toast*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Toast*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, w, l);
        try {
        switch (message) {
        case WM_CREATE:
            self->dpi_ = WindowDpi(window);
            self->renderer_.SetWindow(window, self->dpi_);
            StyleWindow(window);
            SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA);
            return 0;
        case WM_NCCALCSIZE: return 0;
        case WM_NCHITTEST: return HTCLIENT;
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE;
        case WM_PAINT: {
            PaintSession paint(window);
            self->renderer_.PaintToast(self->event_);
            return 0;
        }
        case WM_SIZE: self->renderer_.Resize(); return 0;
        case WM_DPICHANGED: {
            self->dpi_ = HIWORD(w);
            self->renderer_.SetDpi(self->dpi_);
            const auto* r = reinterpret_cast<RECT*>(l);
            SetWindowPos(window, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(window, nullptr, FALSE);
            PostMessageW(self->notify_, ArrangeToastMessage, 0, 0);
            return 0;
        }
        case WM_MOUSEMOVE:
            self->PauseExpiry();
            return 0;
        case WM_MOUSELEAVE:
            if (self->hovered_) {
                self->hovered_ = false;
                self->ArmExpiry(3000);
            }
            return 0;
        case WM_TIMER:
            self->OnTimer(w);
            return 0;
        case WM_DWMCOMPOSITIONCHANGED: StyleWindow(window); return 0;
        case WM_LBUTTONUP: case WM_CLOSE: DestroyWindow(window); return 0;
        case WM_NCDESTROY:
            KillTimer(window, ExpireTimer);
            KillTimer(window, FadeTimer);
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            self->window_ = nullptr;
            self->renderer_.DiscardTarget();
            PostMessageW(self->notify_, ReapToastMessage, 0, 0);
            break;
        }
        return DefWindowProcW(window, message, w, l);
        } catch (...) {
            if (message == WM_NCCREATE) return FALSE;
            if (message == WM_CREATE) return -1;
            PostMessageW(window, WM_CLOSE, 0, 0);
            return 0;
        }
    }
private:
    static constexpr UINT_PTR ExpireTimer = 1, FadeTimer = 2;
    static constexpr ULONGLONG FadeDurationMs = 240;

    bool CursorInside() const {
        POINT cursor{};
        RECT rect{};
        return GetCursorPos(&cursor) && GetWindowRect(window_, &rect) &&
            PtInRect(&rect, cursor) && WindowFromPoint(cursor) == window_;
    }
    void ArmExpiry(UINT delay) {
        KillTimer(window_, ExpireTimer);
        KillTimer(window_, FadeTimer);
        fading_ = false;
        SetLayeredWindowAttributes(window_, 0, 255, LWA_ALPHA);
        expires_at_ = GetTickCount64() + delay;
        if (!SetTimer(window_, ExpireTimer, delay, nullptr)) DestroyWindow(window_);
    }
    void PauseExpiry() {
        if (hovered_) return;
        hovered_ = true;
        KillTimer(window_, ExpireTimer);
        KillTimer(window_, FadeTimer);
        fading_ = false;
        SetLayeredWindowAttributes(window_, 0, 255, LWA_ALPHA);
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
        TrackMouseEvent(&track);
    }
    void OnTimer(UINT_PTR timer) {
        if (timer != ExpireTimer && timer != FadeTimer) return;
        // Check the actual cursor too: a card may appear under a stationary
        // pointer, or a queued WM_TIMER may arrive before WM_MOUSEMOVE.
        if (CursorInside()) { PauseExpiry(); return; }
        if (hovered_) return;
        const ULONGLONG now = GetTickCount64();
        if (timer == ExpireTimer) {
            KillTimer(window_, ExpireTimer); // Idle cards use one deadline, no polling.
            if (now < expires_at_) {
                SetTimer(window_, ExpireTimer, static_cast<UINT>(expires_at_ - now), nullptr);
                return;
            }
            fading_ = true;
            fade_started_ = now;
            if (!SetTimer(window_, FadeTimer, 30, nullptr)) DestroyWindow(window_);
            return;
        }
        if (!fading_) return; // Ignore a queued timer cancelled by a hover.
        const ULONGLONG elapsed = now - fade_started_;
        if (elapsed >= FadeDurationMs) {
            DestroyWindow(window_);
            return;
        }
        const BYTE alpha = static_cast<BYTE>(255 * (FadeDurationMs - elapsed) / FadeDurationMs);
        if (!SetLayeredWindowAttributes(window_, 0, alpha, LWA_ALPHA)) DestroyWindow(window_);
    }

    NetEvent event_;
    HWND window_ = nullptr, notify_ = nullptr;
    Renderer renderer_;
    UINT dpi_ = 96;
    int zoom_ = 100;
    float width_ = 300, height_ = 96;
    bool shown_ = false, hovered_ = false, fading_ = false;
    ULONGLONG expires_at_ = 0, fade_started_ = 0;
};

class Application {
public:
    explicit Application(HINSTANCE instance) : instance_(instance) {}
    ~Application() {
        callbackWindow_.store(nullptr, std::memory_order_release);
        metrics_.reset();
        toasts_.clear();
        if (window_ && IsWindow(window_)) DestroyWindow(window_);
        RemoveTray();
        if (registeredMain_) UnregisterClassW(MainClass, instance_);
        if (registeredToast_) UnregisterClassW(ToastClass, instance_);
        if (icon_) DestroyIcon(icon_);
        if (smallIcon_) DestroyIcon(smallIcon_);
        if (classIcon_) DestroyIcon(classIcon_);
        if (classSmallIcon_) DestroyIcon(classSmallIcon_);
    }
    int Execute(int showCommand, bool restarted) {
        config_.Load();
        notificationState_.store(config_.notify ? 1u : 0u, std::memory_order_release);
        if (FAILED(renderer_.Initialize())) {
            MessageBoxW(nullptr, L"无法初始化 Direct2D / DirectWrite。", L"Pulse", MB_OK | MB_ICONERROR);
            return 1;
        }
        POINT initial{config_.x == INT_MIN ? 0 : config_.x, config_.y == INT_MIN ? 0 : config_.y};
        const auto monitor = MonitorFromPoint(initial, config_.x == INT_MIN ? MONITOR_DEFAULTTOPRIMARY : MONITOR_DEFAULTTONEAREST);
        const RECT work = WorkArea(monitor);
        dpi_ = MonitorDpi(monitor);
        classIcon_ = LoadPulseIcon(SM_CXICON, SM_CYICON, dpi_);
        classSmallIcon_ = LoadPulseIcon(SM_CXSMICON, SM_CYSMICON, dpi_);
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.style = CS_DBLCLKS;
        cls.lpfnWndProc = Procedure;
        cls.hInstance = instance_;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hIcon = IconOrDefault(classIcon_);
        cls.hIconSm = IconOrDefault(classSmallIcon_);
        cls.lpszClassName = MainClass;
        if (!RegisterClassExW(&cls)) return 1;
        registeredMain_ = true;
        cls.lpfnWndProc = Toast::Procedure;
        cls.lpszClassName = ToastClass;
        if (!RegisterClassExW(&cls)) return 1;
        registeredToast_ = true;
        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
        renderer_.SetZoom(config_.zoom);
        renderer_.SetComponents(config_.components, config_.compact);
        model_.Update(Snapshot{}, config_.compact, config_.components);
        renderer_.SetCompactEtaVisible(model_.Data().compactEtaVisible);
        const auto size = FittedSize(work);
        const int width = static_cast<int>(size.cx), height = static_cast<int>(size.cy);
        if (config_.x == INT_MIN || config_.y == INT_MIN) {
            config_.x = work.right - width - Pixels(24, dpi_, config_.zoom);
            config_.y = work.top + Pixels(24, dpi_, config_.zoom);
        }
        config_.x = ClampCoordinate(config_.x, work.left, work.right, width);
        config_.y = ClampCoordinate(config_.y, work.top, work.bottom, height);
        window_ = CreateWindowExW(WS_EX_APPWINDOW, MainClass, L"Pulse", MainStyle,
            config_.x, config_.y, width, height, nullptr, nullptr, instance_, this);
        if (!window_) return 1;
        AllowShowFromLowerIntegrity(window_);
        callbackWindow_.store(window_, std::memory_order_release);
        Fit(false);
        SetWindowPos(window_, config_.pinned ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        AddTray();
        metrics_ = std::make_unique<Metrics>();
        const Wants initialWants = CollectionWants(config_.components, config_.compact, powerMode_);
        metrics_->SetWants(initialWants); // Always before Start and the first Sample, including --restarted.
        if (initialWants.fps) {
            diagnostics::WriteLog(L"FPS", L"ui startup wants=1 compact=" +
                std::to_wstring(config_.compact ? 1 : 0) + L" restarted=" + std::to_wstring(restarted ? 1 : 0) +
                L" full_fps=" + std::to_wstring(config_.components.Enabled(FullComponent::Fps) ? 1 : 0) +
                L" compact_fps=" + std::to_wstring(config_.components.Enabled(CompactComponent::Fps) ? 1 : 0));
        }
        Settings settings;
        settings.slow_ms = config_.slow_ms;
        metrics_->Start(settings, NetworkEvent, this);
        running_ = true;
        const bool startMinimized = showCommand == SW_SHOWMINIMIZED || showCommand == SW_MINIMIZE || showCommand == SW_SHOWMINNOACTIVE;
        ShowWindow(window_, startMinimized ? SW_SHOWMINIMIZED : SW_SHOWNORMAL);
        Sampling(true);
        UpdateWindow(window_);
        MSG message{};
        BOOL result;
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return result < 0 ? 1 : static_cast<int>(message.wParam);
    }
    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, w, l);
        try { return self->Handle(message, w, l); }
        catch (...) {
            if (message == WM_NCCREATE) return FALSE;
            if (message == WM_CREATE) return -1;
            // Never unwind a C++ exception through a Win32 window procedure.
            PostMessageW(window, WM_CLOSE, 0, 0);
            return 0;
        }
    }
private:
    static void NetworkEvent(const NetEvent& event, void* user) noexcept {
        auto* app = static_cast<Application*>(user);
        const UINT_PTR notificationState = app->notificationState_.load(std::memory_order_acquire);
        if (!(notificationState & 1u)) return;
        const HWND window = app->callbackWindow_.load(std::memory_order_acquire);
        if (!window) return;
        try {
            auto copy = std::make_unique<NetEvent>(event);
            if (PostMessageW(window, NetMessage, notificationState, reinterpret_cast<LPARAM>(copy.get()))) copy.release();
        } catch (...) {}
    }
    float ContentWidth() const { return config_.compact ? renderer_.CompactWidth() : 320; }
    float ContentHeight() const { return config_.compact ? 40 : FullLayout(config_.components).height; }
    float Scale() const { return dpi_ / 96.0f * config_.zoom / 100.0f; }
    SIZE FittedSize(const RECT& work) const {
        return {std::min(Pixels(ContentWidth(), dpi_, config_.zoom), std::max(1, static_cast<int>(work.right - work.left))),
                std::min(Pixels(ContentHeight(), dpi_, config_.zoom), std::max(1, static_cast<int>(work.bottom - work.top)))};
    }
    float CurrentWidth() const {
        RECT r{};
        GetClientRect(window_, &r);
        return r.right / Scale();
    }
    Button Hit(LPARAM l) const {
        return renderer_.HitTest(GET_X_LPARAM(l) / Scale(), GET_Y_LPARAM(l) / Scale(), config_.compact, CurrentWidth());
    }
    void CapturePosition() {
        if (!window_ || IsIconic(window_)) return;
        RECT r{};
        if (GetWindowRect(window_, &r)) { config_.x = r.left; config_.y = r.top; }
    }
    void Save() { CapturePosition(); config_.Save(); }
    void Fit(bool anchor, const RECT* suggestion = nullptr, bool topRight = false) {
        if (!window_ || fitting_) return;
        if (IsIconic(window_)) {
            if (topRight && !zoomFitPending_) {
                WINDOWPLACEMENT placement{};
                placement.length = sizeof(placement);
                if (GetWindowPlacement(window_, &placement)) {
                    zoomAnchor_ = placement.rcNormalPosition;
                    // Main windows use workspace coordinates in WINDOWPLACEMENT.
                    const auto monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
                    MONITORINFO info{};
                    info.cbSize = sizeof(info);
                    if (GetMonitorInfoW(monitor, &info))
                        OffsetRect(&zoomAnchor_, info.rcWork.left - info.rcMonitor.left,
                                   info.rcWork.top - info.rcMonitor.top);
                    zoomFitPending_ = true;
                }
            }
            return;
        }
        topRight = topRight || zoomFitPending_;
        RECT old{};
        GetWindowRect(window_, &old);
        RECT base = zoomFitPending_ ? zoomAnchor_ : suggestion ? *suggestion : old;
        zoomFitPending_ = false;
        const auto monitor = MonitorFromRect(&base, MONITOR_DEFAULTTONEAREST);
        const RECT work = WorkArea(monitor);
        const auto size = FittedSize(work);
        const int width = static_cast<int>(size.cx), height = static_cast<int>(size.cy);
        int x = base.left, y = base.top;
        if (topRight) x = base.right - width;
        else if (anchor && !suggestion) {
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            const RECT screen = GetMonitorInfoW(monitor, &info) ? info.rcMonitor : work;
            if (static_cast<long long>(old.left) + old.right > static_cast<long long>(screen.left) + screen.right) x = old.right - width;
            if (static_cast<long long>(old.top) + old.bottom > static_cast<long long>(screen.top) + screen.bottom) y = old.bottom - height;
        }
        x = ClampCoordinate(x, work.left, work.right, width);
        y = ClampCoordinate(y, work.top, work.bottom, height);
        ClampScroll((config_.compact ? width : height) / Scale());
        if (old.left == x && old.top == y && old.right - old.left == width && old.bottom - old.top == height) return;
        fitting_ = true;
        SetWindowPos(window_, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        fitting_ = false;
        CapturePosition();
    }
    void ClampScroll(float viewport = 0) {
        if (viewport <= 0) {
            RECT client{};
            GetClientRect(window_, &client);
            viewport = (config_.compact ? client.right : client.bottom) / Scale();
        }
        const float extent = config_.compact ? ContentWidth() : ContentHeight();
        scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, extent - viewport));
        renderer_.SetScroll(scroll_);
    }
    Snapshot ReadMetrics() {
        auto sample = metrics_->Sample(!config_.compact);
        fpsStatus_ = sample.fps_status;
        if (powerMode_ != sample.mode) {
            powerMode_ = sample.mode;
            metrics_->SetWants(CollectionWants(config_.components, config_.compact, powerMode_));
        }
        return sample;
    }
    void Refresh() {
        if (!running_ || !active_ || !metrics_) return;
        const auto sample = ReadMetrics();
        if (model_.Update(sample, config_.compact, config_.components) || renderLost_) repaintPending_ = true;
        if (renderer_.SetCompactEtaVisible(model_.Data().compactEtaVisible)) Fit(true);
        // Keep sampling while covered, then draw the newest view when exposed.
        // Checking once per sample also recovers if no exposure WM_PAINT arrives.
        if (repaintPending_ && renderer_.CanRender()) {
            InvalidateRect(window_, nullptr, FALSE);
        }
    }
    void Sampling(bool sampleNow, bool resetTimer = false) {
        if (!running_ || !metrics_) return;
        const bool active = !systemSuspended_ && IsWindowVisible(window_) && !IsIconic(window_);
        const bool changed = active_ != active;
        if (changed || resetTimer || !active) KillTimer(window_, SampleTimer);
        metrics_->SetPaused(!active);
        active_ = active;
        if (!active) {
            // Pausing stops the timer, so Take would otherwise never get a chance
            // to release an active FPS session until the window was shown again.
            if (changed) metrics_->Sample(!config_.compact);
            return;
        }
        if (changed) model_.BreakHistory();
        if (changed || resetTimer) SetSampleTimer(window_, config_.compact, config_.refresh_ms);
        if (sampleNow || changed) Refresh();
    }
    void Reconfigure(bool resetHistory, bool visibleChanged = true) {
        if (metrics_) metrics_->SetWants(CollectionWants(config_.components, config_.compact, powerMode_));
        if (!visibleChanged) { Save(); return; }
        hover_ = pressed_ = Button::None;
        scroll_ = 0;
        renderer_.SetScroll(0);
        renderer_.SetComponents(config_.components, config_.compact);
        if (resetHistory) model_.BreakHistory();
        const Snapshot sample = metrics_ ? ReadMetrics() : Snapshot{};
        model_.Update(sample, config_.compact, config_.components);
        renderer_.SetCompactEtaVisible(model_.Data().compactEtaVisible);
        repaintPending_ = true;
        Fit(true);
        Sampling(false, true);
        Save();
        if (active_) InvalidateRect(window_, nullptr, FALSE);
    }
    void SetCompact(bool compact) {
        if (config_.compact == compact) return;
        config_.compact = compact;
        Reconfigure(true);
    }
    void SetZoom(int percent) {
        percent = std::clamp(percent, 80, 200);
        if (config_.zoom == percent) return;
        config_.zoom = percent;
        renderer_.SetZoom(percent);
        hover_ = pressed_ = Button::None;
        dragging_ = dragCandidate_ = false;
        if (GetCapture() == window_) ReleaseCapture();
        repaintPending_ = true;
        Fit(false, nullptr, true); // Preserve the right/top edges, then clamp to this monitor.
        ArrangeToasts();
        Save();
        if (!IsIconic(window_)) InvalidateRect(window_, nullptr, FALSE);
    }
    void TogglePin() {
        config_.pinned = !config_.pinned;
        SetWindowPos(window_, config_.pinned ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        Save();
        if (active_) InvalidateRect(window_, nullptr, FALSE);
    }
    void Show() {
        ShowWindow(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOW);
        Fit(false);
        SetForegroundWindow(window_);
        Sampling(true);
    }
    void ToggleVisibility() {
        if (IsWindowVisible(window_) && !IsIconic(window_)) {
            Save();
            ShowWindow(window_, SW_HIDE);
            Sampling(false);
        } else Show();
    }
    void UpdateIcons() {
        const HICON oldIcon = std::exchange(icon_, LoadPulseIcon(SM_CXICON, SM_CYICON, dpi_));
        const HICON oldSmallIcon = std::exchange(smallIcon_, LoadPulseIcon(SM_CXSMICON, SM_CYSMICON, dpi_));
        SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(IconOrDefault(icon_)));
        SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(IconOrDefault(smallIcon_)));
        if (tray_) {
            NOTIFYICONDATAW data{};
            data.cbSize = sizeof(data);
            data.hWnd = window_;
            data.uID = TrayId;
            data.uFlags = NIF_ICON;
            data.hIcon = IconOrDefault(smallIcon_);
            Shell_NotifyIconW(NIM_MODIFY, &data);
        }
        if (oldIcon) DestroyIcon(oldIcon);
        if (oldSmallIcon) DestroyIcon(oldSmallIcon);
    }
    void AddTray() {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = TrayId;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = TrayMessage;
        data.hIcon = IconOrDefault(smallIcon_);
        std::swprintf(data.szTip, sizeof(data.szTip) / sizeof(data.szTip[0]), L"%ls", L"Pulse · 双击窗口切换模式");
        tray_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
        if (tray_) {
            data.uVersion = NOTIFYICON_VERSION_4;
            trayVersion4_ = Shell_NotifyIconW(NIM_SETVERSION, &data) != FALSE;
        }
    }
    void RemoveTray() {
        if (!tray_) return;
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = TrayId;
        Shell_NotifyIconW(NIM_DELETE, &data);
        tray_ = false;
    }
    bool StartupEnabled() const {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, RunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
        DWORD type = 0, bytes = 0;
        const LSTATUS status = RegQueryValueExW(key, L"Pulse", nullptr, &type, nullptr, &bytes);
        bool enabled = false;
        if (status == ERROR_SUCCESS && type == REG_SZ && bytes >= sizeof(wchar_t)) {
            std::vector<wchar_t> value(bytes / sizeof(wchar_t) + 1, 0);
            if (RegQueryValueExW(key, L"Pulse", nullptr, nullptr, reinterpret_cast<BYTE*>(value.data()), &bytes) == ERROR_SUCCESS) {
                const auto expected = L"\"" + ExecutablePath() + L"\"";
                enabled = _wcsicmp(value.data(), expected.c_str()) == 0;
            }
        }
        RegCloseKey(key);
        return enabled;
    }
    void ToggleStartup() {
        const bool disable = StartupEnabled();
        HKEY key = nullptr;
        LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, RunKey, 0, nullptr, 0,
                                         KEY_SET_VALUE, nullptr, &key, nullptr);
        if (status == ERROR_SUCCESS) {
            if (disable) {
                status = RegDeleteValueW(key, L"Pulse");
                if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
            } else {
                const auto executable = ExecutablePath();
                const auto command = L"\"" + executable + L"\"";
                status = executable.empty() ? ERROR_FILE_NOT_FOUND : RegSetValueExW(key, L"Pulse", 0, REG_SZ,
                    reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
            }
            RegCloseKey(key);
        }
        if (status != ERROR_SUCCESS) MessageBoxW(window_, L"无法更新当前用户的开机自启设置。", L"Pulse", MB_OK | MB_ICONWARNING);
    }
    void OpenLog() {
        if (!metrics_) return;
        const auto log = metrics_->LogPath();
        if (log.empty()) {
            MessageBoxW(window_, L"日志路径尚未就绪。", L"Pulse", MB_OK | MB_ICONINFORMATION);
            return;
        }
        auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", log.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) {
            const auto parameter = L"\"" + log + L"\"";
            result = reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", L"notepad.exe", parameter.c_str(), nullptr, SW_SHOWNORMAL));
        }
        if (result <= 32) MessageBoxW(window_, L"无法打开日志，日志文件可能尚未生成。", L"Pulse", MB_OK | MB_ICONINFORMATION);
    }
    bool FpsNeedsAdmin() const {
        if (fpsStatus_ != 2) return false;
        return config_.compact ? config_.components.Enabled(CompactComponent::Fps)
                               : config_.components.Enabled(FullComponent::Fps);
    }
    void RestartElevated() {
        const auto path = ExecutablePath();
        if (path.empty() || !window_) return;
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        // NOASYNC returns after the new process is created, not after it exits.
        // Do not wait on hProcess: the new instance waits for this mutex.
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        info.hwnd = window_;
        info.lpVerb = L"runas";
        info.lpFile = path.c_str();
        info.lpParameters = RestartedArg;
        info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info)) return;
        if (info.hProcess) CloseHandle(info.hProcess);
        SendMessageW(window_, WM_CLOSE, 0, 0);
    }
    void Menu(POINT point) {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING | (!config_.compact ? MF_CHECKED : 0), CmdFull, L"完整模式");
        AppendMenuW(menu, MF_STRING | (config_.compact ? MF_CHECKED : 0), CmdCompact, L"迷你模式");
        HMENU zoom = CreatePopupMenu();
        if (zoom) {
            for (size_t i = 0; i < ZoomLevels.size(); ++i) {
                auto label = std::to_wstring(ZoomLevels[i]) + L"%";
                if (ZoomLevels[i] == 100) label += L"（默认）";
                AppendMenuW(zoom, MF_STRING, CmdZoom + static_cast<UINT>(i), label.c_str());
            }
            const auto selected = std::find(ZoomLevels.begin(), ZoomLevels.end(), config_.zoom);
            if (selected != ZoomLevels.end())
                CheckMenuRadioItem(zoom, CmdZoom, CmdZoom + static_cast<UINT>(ZoomLevels.size()) - 1,
                                   CmdZoom + static_cast<UINT>(selected - ZoomLevels.begin()), MF_BYCOMMAND);
            const auto label = selected == ZoomLevels.end() ?
                L"界面大小（自定义 " + std::to_wstring(config_.zoom) + L"%）" : std::wstring(L"界面大小");
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(zoom), label.c_str());
        }
        HMENU refresh = CreatePopupMenu();
        if (refresh) {
            const wchar_t* labels[] = {L"0.5 秒", L"1 秒（默认）", L"2 秒"};
            for (size_t i = 0; i < RefreshIntervals.size(); ++i)
                AppendMenuW(refresh, MF_STRING | (config_.refresh_ms == RefreshIntervals[i] ? MF_CHECKED : 0),
                            CmdRefresh + static_cast<UINT>(i), labels[i]);
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(refresh), L"刷新间隔");
        }
        HMENU slow = CreatePopupMenu();
        if (slow) {
            const wchar_t* labels[] = {L"50 ms", L"100 ms", L"150 ms", L"200 ms",
                                       L"300 ms", L"500 ms（默认）", L"800 ms"};
            static_assert(sizeof(labels) / sizeof(labels[0]) == SlowThresholds.size(), "labels");
            UINT selected = CmdSlowThreshold + 5;
            for (size_t i = 0; i < SlowThresholds.size(); ++i) {
                const UINT id = CmdSlowThreshold + static_cast<UINT>(i);
                AppendMenuW(slow, MF_STRING, id, labels[i]);
                if (config_.slow_ms == SlowThresholds[i]) selected = id;
            }
            CheckMenuRadioItem(slow, CmdSlowThreshold,
                              CmdSlowThreshold + static_cast<UINT>(SlowThresholds.size()) - 1,
                              selected, MF_BYCOMMAND);
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(slow), L"延迟较慢阈值");
        }
        AppendMenuW(menu, MF_STRING | (config_.notify ? MF_CHECKED : 0), CmdNetworkNotify, L"断网提醒");
        AppendMenuW(menu, MF_STRING | (config_.notify_sound ? MF_CHECKED : 0) |
                    (config_.notify ? 0 : MF_GRAYED), CmdNotifySound, L"提醒提示音");
        HMENU contents = CreatePopupMenu();
        HMENU full = CreatePopupMenu();
        HMENU compact = CreatePopupMenu();
        if (contents && full && compact) {
            for (size_t i = 0; i < FullComponents.size(); ++i)
                AppendMenuW(full, MF_STRING | (config_.components.full[i] ? MF_CHECKED : 0),
                            CmdFullComponent + static_cast<UINT>(i), FullComponents[i].label);
            for (size_t i = 0; i < CompactComponents.size(); ++i)
                AppendMenuW(compact, MF_STRING | (config_.components.compact[i] ? MF_CHECKED : 0),
                            CmdCompactComponent + static_cast<UINT>(i), CompactComponents[i].label);
            AppendMenuW(contents, MF_POPUP, reinterpret_cast<UINT_PTR>(full), L"完整模式");
            AppendMenuW(contents, MF_POPUP, reinterpret_cast<UINT_PTR>(compact), L"迷你模式");
            AppendMenuW(contents, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(contents, MF_STRING, CmdDefaults, L"恢复默认");
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(contents), L"显示内容");
        } else {
            if (contents) DestroyMenu(contents);
            if (full) DestroyMenu(full);
            if (compact) DestroyMenu(compact);
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (config_.pinned ? MF_CHECKED : 0), CmdPin, L"窗口置顶");
        AppendMenuW(menu, MF_STRING | (StartupEnabled() ? MF_CHECKED : 0), CmdStartup, L"开机自启");
        AppendMenuW(menu, MF_STRING, CmdLog, L"打开日志");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        if (FpsNeedsAdmin())
            AppendMenuW(menu, MF_STRING, CmdRestartAdmin, L"以管理员身份重启（显示帧数需要）");
        AppendMenuW(menu, MF_STRING, CmdExit, L"退出");
        SetForegroundWindow(window_);
        const UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                              point.x, point.y, window_, nullptr);
        DestroyMenu(menu);
        PostMessageW(window_, WM_NULL, 0, 0);
        if (command) Command(command);
    }
    void Command(UINT command) {
        if (command >= CmdZoom && command < CmdZoom + static_cast<UINT>(ZoomLevels.size())) {
            zoomWheelDelta_ = 0;
            SetZoom(ZoomLevels[command - CmdZoom]);
            return;
        }
        if (command >= CmdSlowThreshold && command < CmdSlowThreshold + static_cast<UINT>(SlowThresholds.size())) {
            config_.slow_ms = SlowThresholds[command - CmdSlowThreshold];
            if (metrics_) metrics_->SetSlowThreshold(config_.slow_ms);
            Refresh(); // Repaint the reclassified result immediately; no new HTTP probe.
            Save();
            return;
        }
        if (command >= CmdRefresh && command < CmdRefresh + static_cast<UINT>(RefreshIntervals.size())) {
            config_.refresh_ms = RefreshIntervals[command - CmdRefresh];
            if (!config_.compact) Sampling(false, true);
            Save();
            return;
        }
        if (command >= CmdFullComponent && command < CmdFullComponent + static_cast<UINT>(FullComponents.size())) {
            const size_t i = command - CmdFullComponent;
            config_.components.full[i] = !config_.components.full[i];
            Reconfigure(false, !config_.compact);
            return;
        }
        if (command >= CmdCompactComponent && command < CmdCompactComponent + static_cast<UINT>(CompactComponents.size())) {
            const size_t i = command - CmdCompactComponent;
            config_.components.compact[i] = !config_.components.compact[i];
            Reconfigure(false, config_.compact);
            return;
        }
        switch (command) {
        case CmdNetworkNotify:
            config_.notify = !config_.notify;
            notificationState_.fetch_add(1u, std::memory_order_acq_rel);
            ClearNetworkNotifications(); // Dismiss cards and discard queued events on either toggle.
            if (!config_.notify) PlaySoundW(nullptr, nullptr, 0);
            Save();
            break;
        case CmdNotifySound:
            if (config_.notify) {
                config_.notify_sound = !config_.notify_sound;
                if (!config_.notify_sound) PlaySoundW(nullptr, nullptr, 0);
                Save();
            }
            break;
        case CmdDefaults: config_.components.Reset(); Reconfigure(true); break;
        case CmdFull: SetCompact(false); break;
        case CmdCompact: SetCompact(true); break;
        case CmdPin: TogglePin(); break;
        case CmdStartup: ToggleStartup(); break;
        case CmdLog: OpenLog(); break;
        case CmdRestartAdmin: RestartElevated(); break;
        case CmdExit: SendMessageW(window_, WM_CLOSE, 0, 0); break;
        }
    }
    void ActivateButton(Button button) {
        switch (button) {
        case Button::Mode: SetCompact(!config_.compact); break;
        case Button::Pin: TogglePin(); break;
        case Button::Minimize: ShowWindow(window_, SW_MINIMIZE); Sampling(false); break;
        case Button::Close: SendMessageW(window_, WM_CLOSE, 0, 0); break;
        default: break;
        }
    }
    void ArrangeToasts() {
        const auto monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
        const RECT work = WorkArea(monitor);
        const UINT dpi = MonitorDpi(monitor);
        const int margin = Pixels(16, dpi, config_.zoom), gap = Pixels(10, dpi, config_.zoom);
        int bottom = work.bottom - margin, column = 0;
        for (auto i = toasts_.rbegin(); i != toasts_.rend(); ++i) {
            auto& toast = *i;
            if (!toast->Window()) continue;
            toast->SetScale(dpi, config_.zoom, std::max(1, static_cast<int>(work.right - work.left) - 2 * margin));
            const int width = toast->Width(dpi);
            const int availableHeight = static_cast<int>(work.bottom - work.top) - 2 * margin;
            const int height = std::min(toast->Height(dpi), std::max(1, availableHeight));
            if (bottom - height < work.top + margin && bottom != work.bottom - margin) {
                ++column;
                bottom = work.bottom - margin;
            }
            const int x = ClampCoordinate(work.right - margin - width - column * (width + gap), work.left, work.right, width);
            const int y = ClampCoordinate(bottom - height, work.top, work.bottom, height);
            toast->Place(x, y, height, dpi);
            bottom = y - gap;
        }
    }
    void Notify(const NetEvent& event) {
        if (!config_.notify) return;
        if (config_.notify_sound)
            PlaySoundW(L"C:\\Windows\\Media\\Windows Notify System Generic.wav", nullptr,
                       SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
        auto toast = std::make_unique<Toast>(event, window_);
        if (toast->Open(instance_, dpi_, config_.zoom)) {
            toasts_.push_back(std::move(toast));
            ArrangeToasts();
        }
    }
    void ClearNetworkNotifications() {
        MSG message{};
        while (PeekMessageW(&message, window_, NetMessage, NetMessage, PM_REMOVE))
            delete reinterpret_cast<NetEvent*>(message.lParam);
        toasts_.clear();
    }
    void Shutdown() {
        if (stopping_) return;
        stopping_ = true;
        Save();
        running_ = active_ = false;
        KillTimer(window_, SampleTimer);
        callbackWindow_.store(nullptr, std::memory_order_release);
        if (metrics_) metrics_->SetPaused(true);
        metrics_.reset(); // Joins callbacks while HWND and Application are still alive.
        ClearNetworkNotifications();
        RemoveTray();
        PostQuitMessage(0);
    }
    LRESULT Handle(UINT message, WPARAM w, LPARAM l) {
        if (taskbarCreated_ && message == taskbarCreated_) { tray_ = false; AddTray(); return 0; }
        switch (message) {
        case WM_CREATE:
            dpi_ = WindowDpi(window_);
            UpdateIcons();
            renderer_.SetWindow(window_, dpi_);
            StyleWindow(window_);
            return 0;
        case WM_NCCALCSIZE: return 0;
        case WM_NCHITTEST: return HTCLIENT;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PaintSession paint(window_);
            if (renderer_.CanRender()) {
                const HRESULT hr = renderer_.Paint(model_.Data(), config_.compact, config_.pinned, hover_, pressed_);
                renderLost_ = FAILED(hr);
                repaintPending_ = hr != S_OK;
            } else repaintPending_ = true;
            return 0;
        }
        case WM_SIZE:
            if (w != SIZE_MINIMIZED) {
                renderer_.Resize();
                ClampScroll();
                if (running_ && !fitting_) Fit(true);
            }
            Sampling(false);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(l);
            const RECT work = WorkArea(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST));
            const auto size = FittedSize(work);
            limits->ptMinTrackSize = {size.cx, size.cy};
            limits->ptMaxTrackSize = limits->ptMinTrackSize;
            return 0;
        }
        case WM_POWERBROADCAST:
            if (w == PBT_APMSUSPEND) {
                systemSuspended_ = true;
                if (metrics_) metrics_->OnSuspend();
                // Publication is serialized with OnSuspend, so this also removes
                // notifications queued immediately before the power broadcast.
                ClearNetworkNotifications();
                Sampling(false);
                return TRUE;
            }
            if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND) {
                if (metrics_) metrics_->OnResume();
                ClearNetworkNotifications();
                systemSuspended_ = false;
                model_.BreakHistory();
                Sampling(true, true); // Show Checking; HTTP remains delayed by 3 s.
                return TRUE;
            }
            break;
        case WM_SHOWWINDOW: Sampling(w != 0); break;
        case WM_TIMER: if (w == SampleTimer) Refresh(); return 0;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(w);
            UpdateIcons();
            renderer_.SetDpi(dpi_);
            // SetWindowPos can synchronously change DPI while Fit is already in progress.
            if (fitting_) {
                if (!dpiFitPending_) {
                    dpiFitPending_ = true;
                    PostMessageW(window_, DpiFitMessage, 0, 0);
                }
            } else Fit(false, reinterpret_cast<RECT*>(l));
            ArrangeToasts();
            Save();
            if (!IsIconic(window_)) InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        case WM_DISPLAYCHANGE:
            Fit(false);
            ArrangeToasts();
            if (active_) InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_SETTINGCHANGE:
            if (w == SPI_SETWORKAREA) { Fit(false); ArrangeToasts(); }
            return 0;
        case WM_EXITSIZEMOVE: Fit(false); Save(); return 0;
        case WM_DWMCOMPOSITIONCHANGED: StyleWindow(window_); return 0;
        case WM_SYSCOMMAND:
            if ((w & 0xfff0) == SC_MAXIMIZE || (w & 0xfff0) == SC_SIZE) return 0;
            break;
        case WM_MOUSEWHEEL: {
            POINT cursor{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ScreenToClient(window_, &cursor);
            RECT client{};
            GetClientRect(window_, &client);
            if (!PtInRect(&client, cursor)) { zoomWheelDelta_ = 0; return 0; }
            if (GET_KEYSTATE_WPARAM(w) & MK_CONTROL) {
                zoomWheelDelta_ += GET_WHEEL_DELTA_WPARAM(w);
                const int steps = zoomWheelDelta_ / WHEEL_DELTA;
                zoomWheelDelta_ %= WHEEL_DELTA;
                if (steps) SetZoom(config_.zoom + steps * 10);
                return 0;
            }
            zoomWheelDelta_ = 0;
            const float previous = scroll_;
            scroll_ -= static_cast<float>(GET_WHEEL_DELTA_WPARAM(w)) / WHEEL_DELTA * 66;
            ClampScroll();
            if (scroll_ != previous) InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            pressed_ = Hit(l);
            if (pressed_ != Button::None) InvalidateRect(window_, nullptr, FALSE);
            else {
                GetCursorPos(&dragOrigin_);
                GetWindowRect(window_, &dragRect_);
                dragCandidate_ = true;
            }
            SetCapture(window_);
            return 0;
        }
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
            TrackMouseEvent(&track);
            if (dragCandidate_ && GetCapture() == window_) {
                POINT cursor{};
                GetCursorPos(&cursor);
                const int dx = cursor.x - dragOrigin_.x, dy = cursor.y - dragOrigin_.y;
                if (dragging_ || std::abs(dx) >= GetSystemMetrics(SM_CXDRAG) || std::abs(dy) >= GetSystemMetrics(SM_CYDRAG)) {
                    dragging_ = true;
                    SetWindowPos(window_, nullptr, dragRect_.left + dx, dragRect_.top + dy, 0, 0,
                                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                }
            } else {
                const auto hover = Hit(l);
                if (hover != hover_) { hover_ = hover; InvalidateRect(window_, nullptr, FALSE); }
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            const auto pressed = pressed_;
            const bool click = pressed != Button::None && Hit(l) == pressed;
            const bool moved = dragging_;
            pressed_ = Button::None;
            dragging_ = dragCandidate_ = false;
            if (GetCapture() == window_) ReleaseCapture();
            if (moved) { Fit(false); Save(); }
            if (pressed != Button::None) InvalidateRect(window_, nullptr, FALSE);
            if (click) ActivateButton(pressed);
            return 0;
        }
        case WM_LBUTTONDBLCLK:
            if (Hit(l) == Button::None) SetCompact(!config_.compact);
            return 0;
        case WM_MOUSELEAVE:
            zoomWheelDelta_ = 0;
            if (hover_ != Button::None) { hover_ = Button::None; InvalidateRect(window_, nullptr, FALSE); }
            return 0;
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE: {
            const bool redraw = pressed_ != Button::None;
            if (dragging_) { Fit(false); Save(); }
            dragging_ = dragCandidate_ = false;
            pressed_ = Button::None;
            if (redraw && active_) InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT) {
                POINT p{};
                GetCursorPos(&p);
                ScreenToClient(window_, &p);
                const auto button = renderer_.HitTest(p.x / Scale(), p.y / Scale(), config_.compact, CurrentWidth());
                SetCursor(LoadCursorW(nullptr, button == Button::None ? IDC_ARROW : IDC_HAND));
                return TRUE;
            }
            break;
        case WM_CONTEXTMENU: {
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            if (p.x == -1 && p.y == -1) GetCursorPos(&p);
            Menu(p);
            return 0;
        }
        case WM_COMMAND: Command(LOWORD(w)); return 0;
        case TrayMessage:
            if (LOWORD(l) == NIN_SELECT || LOWORD(l) == NIN_KEYSELECT ||
                (!trayVersion4_ && LOWORD(l) == WM_LBUTTONUP)) ToggleVisibility();
            else if (LOWORD(l) == WM_CONTEXTMENU || LOWORD(l) == WM_RBUTTONUP) {
                POINT p{};
                GetCursorPos(&p);
                Menu(p);
            }
            return 0;
        case NetMessage: {
            std::unique_ptr<NetEvent> event(reinterpret_cast<NetEvent*>(l));
            if (event && !stopping_ && !systemSuspended_ &&
                w == notificationState_.load(std::memory_order_acquire)) Notify(*event);
            return 0;
        }
        case ShowMessage: Show(); return 0;
        case ReapToastMessage:
            toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [](const auto& t) { return !t->Window(); }), toasts_.end());
            ArrangeToasts();
            return 0;
        case ArrangeToastMessage: ArrangeToasts(); return 0;
        case DpiFitMessage:
            dpiFitPending_ = false;
            Fit(false);
            Save();
            if (active_) InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_QUERYENDSESSION: Save(); return TRUE;
        case WM_ENDSESSION: if (w) { Save(); DestroyWindow(window_); } return 0;
        case WM_CLOSE: Save(); DestroyWindow(window_); return 0;
        case WM_DESTROY: Shutdown(); return 0;
        case WM_NCDESTROY: {
            const HWND old = window_;
            SetWindowLongPtrW(old, GWLP_USERDATA, 0);
            window_ = nullptr;
            return DefWindowProcW(old, message, w, l);
        }
        }
        return DefWindowProcW(window_, message, w, l);
    }

    HINSTANCE instance_;
    HWND window_ = nullptr;
    HICON icon_ = nullptr, smallIcon_ = nullptr;
    // Class icons must outlive every main/notification window and both classes.
    HICON classIcon_ = nullptr, classSmallIcon_ = nullptr;
    std::atomic<HWND> callbackWindow_{nullptr};
    // Low bit enables reminders; every toggle advances the value so an old
    // callback cannot replay its card after reminders have been re-enabled.
    std::atomic<UINT_PTR> notificationState_{1};
    Renderer renderer_;
    Model model_;
    Config config_;
    std::unique_ptr<Metrics> metrics_;
    std::vector<std::unique_ptr<Toast>> toasts_;
    UINT dpi_ = 96, taskbarCreated_ = 0;
    PowerMode powerMode_ = PowerMode::Unknown;
    int fpsStatus_ = 0;
    bool active_ = false, running_ = false, stopping_ = false, fitting_ = false, dpiFitPending_ = false;
    bool zoomFitPending_ = false;
    bool systemSuspended_ = false;
    bool tray_ = false, trayVersion4_ = false;
    bool registeredMain_ = false, registeredToast_ = false, renderLost_ = false;
    float scroll_ = 0;
    int zoomWheelDelta_ = 0;
    bool repaintPending_ = true;
    bool dragCandidate_ = false, dragging_ = false;
    POINT dragOrigin_{};
    RECT dragRect_{};
    RECT zoomAnchor_{};
    Button hover_ = Button::None, pressed_ = Button::None;
};

struct HandleCloser { void operator()(void* handle) const { if (handle) CloseHandle(handle); } };

bool CommandHasRestarted(const wchar_t* commandLine) {
    if (!commandLine) return false;
    constexpr std::size_t restartedLen = sizeof(RestartedArg) / sizeof(wchar_t) - 1;
    const wchar_t* p = commandLine;
    while (*p) {
        while (*p == L' ' || *p == L'\t') ++p;
        if (!*p) break;
        const bool quoted = *p == L'"';
        if (quoted) ++p;
        const wchar_t* start = p;
        if (quoted) while (*p && *p != L'"') ++p;
        else while (*p && *p != L' ' && *p != L'\t') ++p;
        if (static_cast<std::size_t>(p - start) == restartedLen &&
            std::wmemcmp(start, RestartedArg, restartedLen) == 0) {
            return true;
        }
        if (quoted && *p == L'"') ++p;
    }
    return false;
}

void ActivateExistingInstance() {
    HWND previous = FindWindowW(MainClass, nullptr);
    if (!previous) return;
    DWORD process = 0;
    GetWindowThreadProcessId(previous, &process);
    AllowSetForegroundWindow(process);
    PostMessageW(previous, ShowMessage, 0, 0);
}

void AllowShowFromLowerIntegrity(HWND window) {
    using FilterFn = BOOL (WINAPI*)(HWND, UINT, DWORD, void*);
    const auto filter = reinterpret_cast<FilterFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "ChangeWindowMessageFilterEx"));
    if (filter) filter(window, ShowMessage, MSGFLT_ALLOW, nullptr);
}
struct ComApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};
} // namespace

void EnableDarkMode() {
    // Ordinal 135 changed its signature in Windows 10 1903. Get the real
    // build number without depending on the application's version manifest.
    using GetVersionNumbersFn = void(WINAPI*)(DWORD*, DWORD*, DWORD*);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return;
    const auto getVersionNumbers = reinterpret_cast<GetVersionNumbersFn>(
        GetProcAddress(ntdll, "RtlGetNtVersionNumbers"));
    if (!getVersionNumbers) return;
    DWORD major = 0, minor = 0, build = 0;
    getVersionNumbers(&major, &minor, &build);
    build &= 0x0fffffffu; // RtlGetNtVersionNumbers includes flags in the high bits.
    if (major < 10 || build < 17763) return;

    const HMODULE theme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!theme) return;
    using FlushMenuThemesFn = void(WINAPI*)();
    const auto setAppMode = GetProcAddress(theme, MAKEINTRESOURCEA(135));
    const auto flushMenuThemes = reinterpret_cast<FlushMenuThemesFn>(
        GetProcAddress(theme, MAKEINTRESOURCEA(136)));
    if (!setAppMode || !flushMenuThemes) {
        FreeLibrary(theme);
        return;
    }
    if (build >= 18362) {
        enum class PreferredAppMode : int { ForceDark = 2 };
        using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
        reinterpret_cast<SetPreferredAppModeFn>(setAppMode)(PreferredAppMode::ForceDark);
    } else {
        using AllowDarkModeForAppFn = bool(WINAPI*)(bool);
        reinterpret_cast<AllowDarkModeForAppFn>(setAppMode)(true);
    }
    flushMenuThemes();
    // Called once from wWinMain before any windows/menus. Retain the DLL for
    // the process lifetime so unloading it cannot discard the app's theme state.
}

void EnableDpiAwareness() {
    using SetContextFn = BOOL(WINAPI*)(HANDLE);
    auto setContext = reinterpret_cast<SetContextFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (setContext && setContext(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) return; // Per-Monitor V2.
    using SetAwarenessFn = HRESULT(WINAPI*)(int);
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        const auto setAwareness = reinterpret_cast<SetAwarenessFn>(GetProcAddress(shcore, "SetProcessDpiAwareness"));
        const bool success = setAwareness && SUCCEEDED(setAwareness(2));
        FreeLibrary(shcore);
        if (success) return;
    }
    SetProcessDPIAware();
}

int Run(HINSTANCE instance, int showCommand, const wchar_t* commandLine) {
    const bool restarted = CommandHasRestarted(commandLine);
    HANDLE mutex = CreateMutexW(nullptr, FALSE, SingleInstanceMutex);
    if (!mutex) {
        const DWORD openError = GetLastError();
        // An elevated instance's mutex can refuse a normal launch. Still one instance.
        if (openError == ERROR_ACCESS_DENIED) ActivateExistingInstance();
        return openError == ERROR_ACCESS_DENIED ? 0 : 1;
    }
    DWORD error = GetLastError();
    if (restarted && error == ERROR_ALREADY_EXISTS) {
        const ULONGLONG deadline = GetTickCount64() + 5000;
        while (error == ERROR_ALREADY_EXISTS) {
            if (GetTickCount64() >= deadline) {
                if (mutex) CloseHandle(mutex);
                ActivateExistingInstance();
                return 0;
            }
            if (mutex) CloseHandle(mutex);
            mutex = nullptr;
            Sleep(50);
            mutex = CreateMutexW(nullptr, FALSE, SingleInstanceMutex);
            if (!mutex) {
                error = ERROR_ALREADY_EXISTS;
                continue;
            }
            error = GetLastError();
        }
    }
    std::unique_ptr<void, HandleCloser> uniqueInstance(mutex);
    if (error == ERROR_ALREADY_EXISTS) {
        ActivateExistingInstance();
        return 0;
    }
    ComApartment apartment;
    if (FAILED(apartment.result)) return 1;
    Application app(instance);
    return app.Execute(showCommand, restarted);
}
} // namespace pulse::ui

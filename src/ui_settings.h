#pragma once
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
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace pulse::ui {
struct SettingOption {
    std::wstring label;
    UINT command = 0;
    bool checked = false, enabled = true;
};
struct SettingRow {
    enum class Kind { Toggle, Choice, Action, Note, Text, Heading };
    std::wstring label;
    Kind kind = Kind::Action;
    std::vector<SettingOption> options;
};
struct SettingsPage { std::wstring title; std::vector<SettingRow> rows; std::wstring description; };

// LiteMonitor WinForms settings UI; values and actions remain owned by Application.
class SettingsWindow {
public:
    using Read = std::function<std::vector<SettingsPage>()>;
    using Apply = std::function<bool(UINT,const std::wstring&)>;
    ~SettingsWindow();
    bool Open(HWND owner, Read read, Apply apply);
    void Close();
    void Refresh();
    void SetDark(bool dark);
    bool Translate(MSG& message);
    HWND Handle() const { return window_; }
#ifdef PULSE_DEVELOPMENT
    void SelectPageForTest(size_t page);
    HWND ControlForTest(UINT command) const;
    void CaptureForTest(const wchar_t* path);
    void CommitForTest();
    void SettleForTest();
    bool ScrollForTest();
#endif
private:
    static LRESULT CALLBACK Procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT Message(HWND, UINT, WPARAM, LPARAM);
    void SendModel();
    void Drain();
    void Pump(DWORD milliseconds, bool startup=false);
    void Reap();
    HWND window_ = nullptr, bridge_ = nullptr,owner_=nullptr;
    HANDLE process_=nullptr, job_=nullptr, payload_=INVALID_HANDLE_VALUE;
    DWORD pid_=0;
    ULONGLONG openingDeadline_=0;
    std::wstring payloadPath_;
    std::vector<SettingsPage> pages_;
    std::vector<std::pair<UINT,std::wstring>> pending_;
    Read read_;
    Apply apply_;
    unsigned revision_=0, acknowledged_=0;
    bool dark_=false, saveOk_=true, closing_=false, draining_=false;
};
}

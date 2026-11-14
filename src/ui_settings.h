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

// Native modeless controls; values and actions remain owned by Application.
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
#endif
private:
    struct Control { HWND window; size_t row; int y, height; bool label; };
    static LRESULT CALLBACK Procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT Message(HWND, UINT, WPARAM, LPARAM);
    HWND Add(const wchar_t* type, const std::wstring& text, DWORD style, int id, HWND parent);
    void Rebuild(bool navigation);
    void UpdateValues();
    void Layout();
    void Scroll(int position);
    void Invoke(int id, int notification);
    void CommitText();
    void PaintSurface(HWND,HDC);
    int Px(int value) const;
    HWND window_ = nullptr, navigation_ = nullptr, body_ = nullptr, title_ = nullptr;
    HWND status_ = nullptr, close_ = nullptr, brand_ = nullptr, subtitle_ = nullptr;
    HFONT font_ = nullptr, headingFont_ = nullptr;
    std::vector<SettingsPage> pages_;
    std::vector<Control> controls_;
    struct Card { int y,height; };
    std::vector<Card> cards_;
    Read read_;
    Apply apply_;
    size_t page_ = 0;
    int scroll_ = 0, contentHeight_ = 0;
    UINT dpi_ = 96;
    bool rebuilding_ = false, applying_ = false, saveOk_ = true;
    bool dark_=false;
    HBRUSH background_=nullptr, surface_=nullptr;
    COLORREF Background() const {return dark_?RGB(28,28,28):RGB(246,246,246);}
    COLORREF Surface() const {return dark_?RGB(39,39,39):RGB(255,255,255);}
    COLORREF Foreground() const {return dark_?RGB(242,242,242):RGB(28,28,28);}
    COLORREF Muted() const {return dark_?RGB(180,180,180):RGB(96,96,96);}
};
}

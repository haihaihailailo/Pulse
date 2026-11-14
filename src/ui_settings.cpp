#include "ui_settings.h"
#include "i18n.h"
#include <algorithm>
#include <stdexcept>
#include <dwmapi.h>

namespace pulse::ui {
namespace {
constexpr wchar_t ClassName[] = L"Pulse.Settings.v1";
constexpr UINT RefreshMessage = WM_APP + 81;
constexpr int NavId = 10, CloseId = 11, RowBase = 100;
const wchar_t* Words(const wchar_t* zh, const wchar_t* en) {
    return i18n::CurrentLanguage() == i18n::Language::English ? en : zh;
}
void LightControl(HWND window) {
    // The process uses dark popup menus. Standard settings controls use a
    // consistent light system surface, including detached combo dropdowns.
    static HMODULE theme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using Allow = BOOL(WINAPI*)(HWND, BOOL);
    static auto allow = theme ? reinterpret_cast<Allow>(GetProcAddress(theme, MAKEINTRESOURCEA(133))) : nullptr;
    if (allow) allow(window, FALSE);
}
}
SettingsWindow::~SettingsWindow() { Close();if(background_)DeleteObject(background_);if(surface_)DeleteObject(surface_); }
void SettingsWindow::SetDark(bool dark) {
    if(dark_==dark&&background_)return;
    CommitText();
    dark_=dark;
    if(background_)DeleteObject(background_);background_=CreateSolidBrush(Background());
    if(surface_)DeleteObject(surface_);surface_=CreateSolidBrush(Surface());
    if(window_) {
        BOOL enabled=dark_;DwmSetWindowAttribute(window_,20,&enabled,sizeof(enabled));
        Rebuild(true);RedrawWindow(window_,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
    }
}
int SettingsWindow::Px(int value) const { return MulDiv(value, dpi_, 96); }
HWND SettingsWindow::Add(const wchar_t* type, const std::wstring& text, DWORD style, int id, HWND parent) {
    if(std::wstring(type)==L"STATIC")style|=SS_NOPREFIX;
    HWND result = CreateWindowExW(0, type, text.c_str(), WS_CHILD | WS_VISIBLE | style,
        0, 0, 1, 1, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (!result) throw std::runtime_error("Cannot create settings control");
    LightControl(result);
    // Classic checkbox text honors WM_CTLCOLORBTN; retain standard interaction.
    if(dark_ && std::wstring(type)==L"BUTTON" && (style&BS_AUTOCHECKBOX)==BS_AUTOCHECKBOX) {
        static HMODULE theme=LoadLibraryExW(L"uxtheme.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        using SetTheme=HRESULT(WINAPI*)(HWND,LPCWSTR,LPCWSTR);
        auto fn=theme?reinterpret_cast<SetTheme>(GetProcAddress(theme,"SetWindowTheme")):nullptr;if(fn)fn(result,L"",L"");
    }
    SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    return result;
}
bool SettingsWindow::Open(HWND owner, Read read, Apply apply) {
    if (window_) { Refresh(); ShowWindow(window_, SW_RESTORE); SetForegroundWindow(window_); return true; }
    read_ = std::move(read); apply_ = std::move(apply); saveOk_ = true;
    dpi_ = GetDpiForWindow(owner);
    if (!dpi_) dpi_ = 96;
    WNDCLASSW cls{}; cls.lpfnWndProc = Procedure; cls.hInstance = GetModuleHandleW(nullptr);
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    cls.lpszClassName = ClassName;
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor);
    int width = std::min(Px(920), static_cast<int>(monitor.rcWork.right - monitor.rcWork.left));
    int height = std::min(Px(740), static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top));
    HWND result = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_CONTROLPARENT, ClassName, L"Pulse",
        WS_OVERLAPPEDWINDOW, monitor.rcWork.left + (monitor.rcWork.right-monitor.rcWork.left-width)/2,
        monitor.rcWork.top + (monitor.rcWork.bottom-monitor.rcWork.top-height)/2,
        width, height, owner, nullptr, cls.hInstance, this);
    if (!result) { Close(); return false; }
    ShowWindow(result, SW_SHOWNORMAL); UpdateWindow(result); SetForegroundWindow(result);
    return true;
}
void SettingsWindow::CommitText() {
    if(rebuilding_||applying_||pages_.empty())return;
    for(const auto& control:controls_)if(!control.label&&control.window==GetFocus()&&pages_[page_].rows[control.row].kind==SettingRow::Kind::Text) {
        Invoke(GetDlgCtrlID(control.window),EN_KILLFOCUS);break;
    }
}
void SettingsWindow::Close() { if (window_) {CommitText();DestroyWindow(window_);} }
bool SettingsWindow::Translate(MSG& message) {
    if (!window_ || !IsDialogMessageW(window_, &message)) return false;
    HWND focus=GetFocus();
    for(const auto& c:controls_) if(c.window==focus) {
        RECT rect{};GetClientRect(body_,&rect);int view=MulDiv(rect.bottom,96,dpi_);
        if(c.y<scroll_)Scroll(c.y);
        else if(c.y+c.height>scroll_+view)Scroll(c.y+c.height-view);
        break;
    }
    return true;
}
void SettingsWindow::Refresh() { if (window_) PostMessageW(window_, RefreshMessage, 0, 0); }
void SettingsWindow::UpdateValues() {
    auto next = read_();
    bool same = next.size() == pages_.size() && page_ < next.size();
    if(same) {
        const auto p=page_;
        same = next[p].title == pages_[p].title && next[p].rows.size() == pages_[p].rows.size();
        for (size_t r=0; same && r<next[p].rows.size(); ++r) {
            const auto& a=next[p].rows[r]; const auto& b=pages_[p].rows[r];
            same = a.label==b.label && a.kind==b.kind && a.options.size()==b.options.size();
            for (size_t o=0; same && o<a.options.size(); ++o)
                same = a.options[o].command==b.options[o].command && a.options[o].label==b.options[o].label;
        }
    }
    if (!same) {
        // A device arrival must not destroy a name being edited.
        for(const auto& c:controls_)if(c.window==GetFocus()&&pages_[page_].rows[c.row].kind==SettingRow::Kind::Text)return;
        Rebuild(true);return;
    }
    pages_=std::move(next); rebuilding_=true;
    for (const auto& c:controls_) {
        if(c.label)continue;
        const auto& row=pages_[page_].rows[c.row];
        if(row.kind==SettingRow::Kind::Note || row.kind==SettingRow::Kind::Heading)continue;
        bool enabled=false;
        for(size_t i=0;i<row.options.size();++i) {
            const auto& option=row.options[i]; enabled=enabled||option.enabled;
            if(row.kind==SettingRow::Kind::Choice && option.checked)SendMessageW(c.window,CB_SETCURSEL,i,0);
        }
        if(row.kind==SettingRow::Kind::Toggle && !row.options.empty())
            SendMessageW(c.window,BM_SETCHECK,row.options.front().checked?BST_CHECKED:BST_UNCHECKED,0);
        EnableWindow(c.window,enabled);
    }
    rebuilding_=false;
    SetWindowTextW(status_,saveOk_?Words(L"更改立即生效并自动保存",L"Changes apply immediately and save automatically"):
        Words(L"设置未能保存，原配置已保留。请检查目录权限后重试。",L"Settings could not be saved. The previous file is preserved. Check folder access and retry."));
}
void SettingsWindow::Rebuild(bool navigation) {
    rebuilding_ = true;
    int focusId = GetDlgCtrlID(GetFocus());
    for (auto& c : controls_) DestroyWindow(c.window);
    controls_.clear();
    pages_ = read_();
    if (pages_.empty()) { rebuilding_ = false; return; }
    page_ = std::min(page_, pages_.size()-1);
    SetWindowTextW(window_, Words(L"Pulse · 设置", L"Pulse · Settings"));
    SetWindowTextW(close_, Words(L"关闭", L"Close"));
    if (navigation) {
        SendMessageW(navigation_, LB_RESETCONTENT, 0, 0);
        for (const auto& page : pages_) SendMessageW(navigation_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(page.title.c_str()));
        SendMessageW(navigation_, LB_SETCURSEL, page_, 0);
    }
    SetWindowTextW(title_, pages_[page_].title.c_str());
    SetWindowTextW(subtitle_,pages_[page_].description.c_str());
    SetWindowTextW(status_, saveOk_ ? Words(L"更改立即生效并自动保存", L"Changes apply immediately and save automatically") :
        Words(L"设置未能保存。请检查配置目录权限，再重试更改。", L"Settings could not be saved. Check folder access and retry."));
    int y = 8;
    for (size_t index = 0; index < pages_[page_].rows.size(); ++index) {
        const auto& row = pages_[page_].rows[index];
        const int id = RowBase + static_cast<int>(index);
        HWND control = nullptr;
        bool enabled = !row.options.empty() && row.options.front().enabled;
        if (row.kind == SettingRow::Kind::Note || row.kind == SettingRow::Kind::Heading) {
            control = Add(L"STATIC", row.label, SS_LEFT, id, body_);
            if(row.kind==SettingRow::Kind::Heading)SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
            controls_.push_back({control,index,y,48,false}); y += 58; continue;
        }
        if(row.kind==SettingRow::Kind::Text) {
            HWND label=Add(L"STATIC",row.label,SS_LEFT,0,body_);controls_.push_back({label,index,y,24,true});
            control=Add(L"EDIT",row.options.front().label,WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL,id,body_);
            SendMessageW(control,EM_SETLIMITTEXT,96,0);EnableWindow(control,enabled);
            controls_.push_back({control,index,y+26,32,false});y+=68;continue;
        }
        if (row.kind == SettingRow::Kind::Toggle) {
            control = Add(L"BUTTON", row.label, WS_TABSTOP | BS_AUTOCHECKBOX | BS_MULTILINE, id, body_);
            SendMessageW(control, BM_SETCHECK, row.options.front().checked ? BST_CHECKED : BST_UNCHECKED, 0);
        } else if (row.kind == SettingRow::Kind::Choice) {
            HWND label = Add(L"STATIC", row.label, SS_LEFT, 0, body_);
            controls_.push_back({label,index,y+6,26,true});
            control = Add(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, id, body_);
            enabled = false;
            int selected = -1;
            for (size_t i = 0; i < row.options.size(); ++i) {
                const auto& option = row.options[i];
                // Keep an unavailable saved selection visible, but never dispatch it.
                SendMessageW(control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(option.label.c_str()));
                if (option.checked) selected = static_cast<int>(i);
                enabled = enabled || option.enabled;
            }
            SendMessageW(control, CB_SETCURSEL, selected, 0);
            SendMessageW(control, CB_SETDROPPEDWIDTH, Px(370), 0);
        } else control = Add(L"BUTTON", row.label, WS_TABSTOP | BS_PUSHBUTTON | BS_MULTILINE, id, body_);
        EnableWindow(control, enabled);
        controls_.push_back({control,index,y,34,false});
        y += row.kind == SettingRow::Kind::Toggle ? 36 : 44;
    }
    contentHeight_ = y;
    rebuilding_ = false;
    Layout();
    if (focusId >= RowBase) {
        HWND focus = GetDlgItem(body_, focusId);
        if (focus && IsWindowEnabled(focus)) SetFocus(focus);
    }
}
void SettingsWindow::Layout() {
    if (!window_ || !body_) return;
    RECT rect{}; GetClientRect(window_, &rect);
    int width = rect.right, height = rect.bottom;
    MoveWindow(brand_,Px(24),Px(28),Px(164),Px(36),TRUE);
    MoveWindow(navigation_, Px(12), Px(86), Px(188), std::max(1,height-Px(156)), TRUE);
    MoveWindow(title_, Px(240), Px(28), std::max(1,width-Px(264)), Px(40), TRUE);
    MoveWindow(subtitle_,Px(240),Px(72),std::max(1,width-Px(264)),Px(42),TRUE);
    MoveWindow(body_, Px(228), Px(118), std::max(1,width-Px(244)), std::max(1,height-Px(188)), TRUE);
    MoveWindow(status_, Px(20), height-Px(47), std::max(1,width-Px(150)), Px(36), TRUE);
    MoveWindow(close_, width-Px(112), height-Px(49), Px(92), Px(32), TRUE);
    Scroll(scroll_);
}
void SettingsWindow::Scroll(int position) {
    RECT rect{}; GetClientRect(body_, &rect);
    const int width=MulDiv(rect.right,96,dpi_);
    const bool stacked=width<530;
    HDC dc=GetDC(body_);auto old=SelectObject(dc,font_);
    auto textHeight=[&](const std::wstring& text,int available) {
        RECT r{0,0,Px(std::max(40,available)),0};
        DrawTextW(dc,text.c_str(),-1,&r,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
        return std::max(20,MulDiv(r.bottom,96,dpi_));
    };
    cards_.clear();int y=0;
    for(size_t row=0;row<pages_[page_].rows.size();++row) {
        const auto& item=pages_[page_].rows[row];const auto kind=item.kind;
        int h=0,labelHeight=textHeight(item.label,width-64);
        if(kind==SettingRow::Kind::Heading)h=36;
        else if(kind==SettingRow::Kind::Note)h=labelHeight+24;
        else if(kind==SettingRow::Kind::Choice)h=stacked?labelHeight+60:std::max(60,textHeight(item.label,width-300)+24);
        else if(kind==SettingRow::Kind::Text)h=labelHeight+62;
        else h=std::max(54,labelHeight+28);
        if(kind!=SettingRow::Kind::Note && kind!=SettingRow::Kind::Heading)cards_.push_back({y,h});
        for(auto& c:controls_)if(c.row==row) {
            c.y=y+12;c.height=h-24;
            if(kind==SettingRow::Kind::Choice) {
                if(stacked){c.y=y+(c.label?12:labelHeight+20);c.height=c.label?labelHeight:30;}
                else {c.y=y+(c.label?12:(h-30)/2);c.height=c.label?h-24:30;}
            } else if(kind==SettingRow::Kind::Text){c.y=y+(c.label?12:labelHeight+20);c.height=c.label?labelHeight:30;}
            else if(kind==SettingRow::Kind::Heading){c.y=y+12;c.height=24;}
        }
        y+=h+(kind==SettingRow::Kind::Heading?4:8);
    }
    SelectObject(dc,old);ReleaseDC(body_,dc);contentHeight_=y;
    int view = MulDiv(rect.bottom,96,dpi_);
    scroll_ = std::clamp(position, 0, std::max(0,contentHeight_-view));
    SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS,0,std::max(0,contentHeight_-1),static_cast<UINT>(view),scroll_,0};
    SetScrollInfo(body_, SB_VERT, &info, TRUE);
    GetClientRect(body_,&rect);
    for (const auto& c : controls_) {
        const auto kind = pages_[page_].rows[c.row].kind;
        int x=24, w=width-56;
        if (kind == SettingRow::Kind::Choice && !stacked) { x=c.label?24:width-256; w=c.label?width-300:224; }
        int h = kind == SettingRow::Kind::Choice && !c.label ? 280 : c.height;
        MoveWindow(c.window,Px(x),Px(c.y-scroll_),Px(std::max(1,w)),Px(h),TRUE);
    }
    InvalidateRect(body_,nullptr,TRUE);
}
void SettingsWindow::PaintSurface(HWND window,HDC dc) {
    RECT r{};GetClientRect(window,&r);FillRect(dc,&r,background_?background_:GetSysColorBrush(COLOR_WINDOW));
    HPEN pen=CreatePen(PS_SOLID,1,dark_?RGB(55,55,55):RGB(226,226,226));
    auto oldPen=SelectObject(dc,pen);auto oldBrush=SelectObject(dc,surface_?surface_:GetSysColorBrush(COLOR_WINDOW));
    if(window==body_) {
        for(const auto& card:cards_)RoundRect(dc,Px(8),Px(card.y-scroll_),r.right-Px(8),Px(card.y+card.height-scroll_),Px(8),Px(8));
    } else {
        MoveToEx(dc,Px(212),Px(16),nullptr);LineTo(dc,Px(212),r.bottom-Px(66));
        MoveToEx(dc,Px(20),r.bottom-Px(64),nullptr);LineTo(dc,r.right-Px(20),r.bottom-Px(64));
    }
    SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(pen);
}
void SettingsWindow::Invoke(int id, int notification) {
    if (rebuilding_ || applying_ || id < RowBase || pages_.empty()) return;
    size_t rowIndex = static_cast<size_t>(id-RowBase);
    if (rowIndex >= pages_[page_].rows.size()) return;
    const auto row = pages_[page_].rows[rowIndex];
    size_t option=0;
    std::wstring text;
    if(row.kind==SettingRow::Kind::Text) {
        if(notification!=EN_KILLFOCUS)return;
        wchar_t value[97]{};GetDlgItemTextW(body_,id,value,97);text=value;
        if(text==row.options.front().label){Refresh();return;}
    } else
    if (row.kind == SettingRow::Kind::Choice) {
        if (notification != CBN_SELCHANGE) return;
        auto selected=SendDlgItemMessageW(body_,id,CB_GETCURSEL,0,0);
        if (selected < 0) return;
        option=static_cast<size_t>(selected);
    } else if (notification != BN_CLICKED) return;
    if (option >= row.options.size() || !row.options[option].enabled) { Refresh(); return; }
    applying_=true;
    saveOk_=apply_(row.options[option].command,text);
    applying_=false;
    // Rebuild after native control notification unwinds, preserving focus.
    Refresh();
}
LRESULT CALLBACK SettingsWindow::Procedure(HWND window,UINT message,WPARAM w,LPARAM l) {
    auto* self=reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if (message==WM_NCCREATE) {
        self=static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        if (!self->window_) self->window_=window; else self->body_=window;
    }
    if (self) {
        try { return self->Message(window,message,w,l); }
        catch (...) {
            if (message==WM_CREATE) return -1;
            self->rebuilding_=self->applying_=false;
            if (self->status_) SetWindowTextW(self->status_,Words(L"无法更新设置页面，请关闭后重试。",L"Cannot update settings. Close and try again."));
            return 0;
        }
    }
    return DefWindowProcW(window,message,w,l);
}
LRESULT SettingsWindow::Message(HWND window,UINT message,WPARAM w,LPARAM l) {
    if(message==WM_ERASEBKGND) {PaintSurface(window,reinterpret_cast<HDC>(w));return 1;}
    if(message==WM_PAINT){PAINTSTRUCT ps{};HDC dc=BeginPaint(window,&ps);PaintSurface(window,dc);EndPaint(window,&ps);return 0;}
    if(message==WM_CTLCOLORSTATIC||message==WM_CTLCOLORBTN||message==WM_CTLCOLORLISTBOX||message==WM_CTLCOLOREDIT) {
        HWND child=reinterpret_cast<HWND>(l);bool card=window==body_;bool muted=child==subtitle_||child==status_;
        for(const auto& c:controls_)if(c.window==child) {
            const auto kind=pages_[page_].rows[c.row].kind;
            if(kind==SettingRow::Kind::Note||kind==SettingRow::Kind::Heading){card=false;muted=kind==SettingRow::Kind::Note;}
            if(kind==SettingRow::Kind::Choice&&!c.label) {
                SetTextColor(reinterpret_cast<HDC>(w),GetSysColor(COLOR_WINDOWTEXT));SetBkColor(reinterpret_cast<HDC>(w),GetSysColor(COLOR_WINDOW));return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
            }
        }
        if(message==WM_CTLCOLORLISTBOX && child!=navigation_){SetTextColor(reinterpret_cast<HDC>(w),GetSysColor(COLOR_WINDOWTEXT));SetBkColor(reinterpret_cast<HDC>(w),GetSysColor(COLOR_WINDOW));return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));}
        SetTextColor(reinterpret_cast<HDC>(w),!IsWindowEnabled(child)?Muted():!saveOk_&&child==status_?RGB(215,65,75):muted?Muted():Foreground());
        SetBkColor(reinterpret_cast<HDC>(w),card?Surface():Background());
        return reinterpret_cast<LRESULT>(card?surface_:background_);
    }
    if (window==body_) {
        if (message==WM_COMMAND) { Invoke(LOWORD(w),HIWORD(w)); return 0; }
        if (message==WM_VSCROLL) {
            SCROLLINFO info{sizeof(info),SIF_ALL}; GetScrollInfo(body_,SB_VERT,&info);
            int p=scroll_;
            switch (LOWORD(w)) { case SB_LINEUP:p-=36;break;case SB_LINEDOWN:p+=36;break;
                case SB_PAGEUP:p-=info.nPage;break;case SB_PAGEDOWN:p+=info.nPage;break;
                case SB_THUMBTRACK:p=info.nTrackPos;break; }
            Scroll(p);return 0;
        }
        if (message==WM_MOUSEWHEEL) { Scroll(scroll_-GET_WHEEL_DELTA_WPARAM(w)/WHEEL_DELTA*108);return 0; }
        if (message==WM_CTLCOLORSTATIC || message==WM_CTLCOLORBTN) {
            SetTextColor(reinterpret_cast<HDC>(w),GetSysColor(COLOR_WINDOWTEXT));
            SetBkColor(reinterpret_cast<HDC>(w),GetSysColor(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        return DefWindowProcW(window,message,w,l);
    }
    switch (message) {
    case WM_CREATE: {
        if(!background_)background_=CreateSolidBrush(Background());if(!surface_)surface_=CreateSolidBrush(Surface());
        LightControl(window); BOOL dark=dark_; DwmSetWindowAttribute(window,20,&dark,sizeof(dark));
        font_=CreateFontW(-Px(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        headingFont_=CreateFontW(-Px(22),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        brand_=Add(L"STATIC",L"Pulse",SS_LEFT,0,window);SendMessageW(brand_,WM_SETFONT,reinterpret_cast<WPARAM>(headingFont_),TRUE);
        navigation_=Add(L"LISTBOX",L"",WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED,NavId,window);
        SendMessageW(navigation_,LB_SETITEMHEIGHT,0,Px(46));
        title_=Add(L"STATIC",L"",SS_LEFT,0,window);
        SendMessageW(title_,WM_SETFONT,reinterpret_cast<WPARAM>(headingFont_),TRUE);
        subtitle_=Add(L"STATIC",L"",SS_LEFT,0,window);
        body_=CreateWindowExW(WS_EX_CONTROLPARENT,ClassName,L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|WS_CLIPCHILDREN,
            0,0,1,1,window,nullptr,GetModuleHandleW(nullptr),this);
        status_=Add(L"STATIC",L"",SS_LEFT,0,window);
        close_=Add(L"BUTTON",L"",WS_TABSTOP|BS_DEFPUSHBUTTON,CloseId,window);
        Rebuild(true); SetFocus(navigation_); return 0;
    }
    case WM_SIZE: Layout();return 0;
    case WM_GETMINMAXINFO: {
        auto* info=reinterpret_cast<MINMAXINFO*>(l);info->ptMinTrackSize={Px(700),Px(460)};return 0;
    }
    case WM_DPICHANGED: {
        dpi_=HIWORD(w);
        HFONT old=font_,oldHeading=headingFont_;
        font_=CreateFontW(-Px(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        headingFont_=CreateFontW(-Px(22),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        for (auto control:{navigation_,status_,close_,subtitle_}) SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
        SendMessageW(navigation_,LB_SETITEMHEIGHT,0,Px(46));SendMessageW(brand_,WM_SETFONT,reinterpret_cast<WPARAM>(headingFont_),TRUE);
        SendMessageW(title_,WM_SETFONT,reinterpret_cast<WPARAM>(headingFont_),TRUE);
        auto* rect=reinterpret_cast<RECT*>(l);SetWindowPos(window,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE);
        Rebuild(true);DeleteObject(old);DeleteObject(oldHeading);return 0;
    }
    case RefreshMessage: UpdateValues();return 0;
    case WM_MEASUREITEM: reinterpret_cast<MEASUREITEMSTRUCT*>(l)->itemHeight=Px(46);return TRUE;
    case WM_DRAWITEM: {
        auto* draw=reinterpret_cast<DRAWITEMSTRUCT*>(l);if(draw->CtlID!=NavId||draw->itemID>=pages_.size())break;
        RECT r=draw->rcItem;FillRect(draw->hDC,&r,background_);InflateRect(&r,-Px(4),-Px(3));
        if(draw->itemState&ODS_SELECTED){HBRUSH selected=CreateSolidBrush(dark_?RGB(52,52,52):RGB(228,234,240));FillRect(draw->hDC,&r,selected);DeleteObject(selected);
            RECT bar{r.left,r.top+Px(10),r.left+Px(3),r.bottom-Px(10)};HBRUSH accent=CreateSolidBrush(dark_?RGB(109,185,255):RGB(0,103,192));FillRect(draw->hDC,&bar,accent);DeleteObject(accent);}
        r.left+=Px(16);SetBkMode(draw->hDC,TRANSPARENT);SetTextColor(draw->hDC,Foreground());SelectObject(draw->hDC,font_);
        DrawTextW(draw->hDC,pages_[draw->itemID].title.c_str(),-1,&r,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
        if((draw->itemState&ODS_FOCUS)&&!(draw->itemState&ODS_NOFOCUSRECT))DrawFocusRect(draw->hDC,&r);return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(w)==NavId && HIWORD(w)==LBN_SELCHANGE) {
            auto selected=SendMessageW(navigation_,LB_GETCURSEL,0,0);
            if (selected>=0) {page_=static_cast<size_t>(selected);scroll_=0;Rebuild(false);} return 0;
        }
        if (LOWORD(w)==CloseId || LOWORD(w)==IDCANCEL || LOWORD(w)==IDOK) {Close();return 0;}
        return 0;
    case WM_CLOSE: Close();return 0;
    case WM_CTLCOLORSTATIC:
        SetBkColor(reinterpret_cast<HDC>(w),GetSysColor(COLOR_WINDOW));
        SetTextColor(reinterpret_cast<HDC>(w),!saveOk_ && reinterpret_cast<HWND>(l)==status_ ? RGB(180,30,30):GetSysColor(COLOR_WINDOWTEXT));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    case WM_NCDESTROY:
        controls_.clear();cards_.clear();pages_.clear();window_=navigation_=body_=title_=status_=close_=brand_=subtitle_=nullptr;
        if(font_)DeleteObject(font_);if(headingFont_)DeleteObject(headingFont_);font_=headingFont_=nullptr;
        SetWindowLongPtrW(window,GWLP_USERDATA,0);break;
    }
    return DefWindowProcW(window,message,w,l);
}
#ifdef PULSE_DEVELOPMENT
void SettingsWindow::SelectPageForTest(size_t page) {
    if(page>=pages_.size())throw std::runtime_error("Settings page missing");
    SetFocus(navigation_); // Match real navigation; do not restore a different page's row ID.
    page_=page;scroll_=0;Rebuild(true);
}
HWND SettingsWindow::ControlForTest(UINT command) const {
    for(const auto& c:controls_) if(!c.label) for(const auto& option:pages_[page_].rows[c.row].options)
        if(option.command==command)return c.window;
    return nullptr;
}
void SettingsWindow::CaptureForTest(const wchar_t* path) {
    RECT r{};GetWindowRect(window_,&r);int width=r.right-r.left,height=r.bottom-r.top;
    if(width<=0||height<=0||width>2400||height>2400)throw std::runtime_error("Settings capture size");
    HDC screen=GetDC(window_),dc=CreateCompatibleDC(screen);HBITMAP bitmap=CreateCompatibleBitmap(screen,width,height);
    auto previous=SelectObject(dc,bitmap);BOOL printed=PrintWindow(window_,dc,0);
    BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=width;bi.bmiHeader.biHeight=-height;
    bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
    std::vector<unsigned char> pixels(static_cast<size_t>(width)*height*4);
    SelectObject(dc,previous);int lines=GetDIBits(dc,bitmap,0,height,pixels.data(),&bi,DIB_RGB_COLORS);
    DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(window_,screen);
    if(!printed||lines!=height)throw std::runtime_error("Settings capture failed: print="+std::to_string(printed)+", lines="+std::to_string(lines));
    BITMAPFILEHEADER file{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);file.bfSize=file.bfOffBits+static_cast<DWORD>(pixels.size());
    HANDLE output=CreateFileW(path,GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);DWORD written=0;
    if(output==INVALID_HANDLE_VALUE)throw std::runtime_error("Settings capture path failed");
    bool ok=WriteFile(output,&file,sizeof(file),&written,nullptr)&&written==sizeof(file);
    ok=WriteFile(output,&bi.bmiHeader,sizeof(BITMAPINFOHEADER),&written,nullptr)&&written==sizeof(BITMAPINFOHEADER)&&ok;
    ok=WriteFile(output,pixels.data(),static_cast<DWORD>(pixels.size()),&written,nullptr)&&written==pixels.size()&&ok;
    CloseHandle(output);if(!ok)throw std::runtime_error("Settings capture write failed");
}
#endif
}

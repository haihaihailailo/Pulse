#include "ui_settings.h"
#include "i18n.h"
#include "development.h"
#include <algorithm>
#include <stdexcept>
#include <cstring>
namespace pulse::ui {
namespace {
constexpr wchar_t ClassName[]=L"Pulse.SettingsBridge.v2";
constexpr UINT RefreshMessage=WM_APP+81,ApplyMessage=WM_APP+82;
constexpr UINT SelectPage=WM_APP+200,FindControl=WM_APP+201,Commit=WM_APP+202,ScrollCheck=WM_APP+203;
const wchar_t* Words(const wchar_t* zh,const wchar_t* en){return i18n::CurrentLanguage()==i18n::Language::English?en:zh;}
LRESULT Call(HWND window,UINT message,WPARAM w=0,LPARAM l=0){
    DWORD_PTR result=0;
    if(!window||!SendMessageTimeoutW(window,message,w,l,SMTO_ABORTIFHUNG,2000,&result))return 0;
    return static_cast<LRESULT>(result);
}
void Number(std::vector<unsigned char>& out,unsigned value){
    const auto* b=reinterpret_cast<const unsigned char*>(&value);out.insert(out.end(),b,b+4);
}
void Text(std::vector<unsigned char>& out,const std::wstring& value){
    if(value.size()>16384)throw std::runtime_error("Settings text exceeds protocol bound");
    Number(out,static_cast<unsigned>(value.size()));
    const auto* b=reinterpret_cast<const unsigned char*>(value.data());out.insert(out.end(),b,b+value.size()*2);
}
}
SettingsWindow::~SettingsWindow(){Close();}
bool SettingsWindow::Translate(MSG&){return false;}
void SettingsWindow::SetDark(bool value){if(dark_!=value){dark_=value;Refresh();}}
void SettingsWindow::Refresh(){if(bridge_&&!closing_)PostMessageW(bridge_,RefreshMessage,0,0);}
void SettingsWindow::Drain(){
    if(draining_)return;draining_=true;
    while(!pending_.empty()){
        auto commands=std::move(pending_);pending_.clear();
        for(const auto& c:commands)if(apply_)saveOk_=apply_(c.first,c.second);
    }
    draining_=false;
}
void SettingsWindow::Pump(DWORD milliseconds,bool startup){
    if(!bridge_)return;
    const auto until=GetTickCount64()+milliseconds;
    do{
        MSG m{};while(PeekMessageW(&m,bridge_,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}
        Drain();
        if(process_&&WaitForSingleObject(process_,0)==WAIT_OBJECT_0)break;
        if(startup&&window_&&acknowledged_==revision_&&revision_)return;
        MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
    }while(GetTickCount64()<until);
}
void SettingsWindow::Reap(){
    if(process_){CloseHandle(process_);process_=nullptr;}
    if(job_){CloseHandle(job_);job_=nullptr;}
    if(payload_!=INVALID_HANDLE_VALUE){CloseHandle(payload_);payload_=INVALID_HANDLE_VALUE;}
    if(!payloadPath_.empty()){DeleteFileW(payloadPath_.c_str());payloadPath_.clear();}
    if(bridge_){KillTimer(bridge_,1);DestroyWindow(bridge_);bridge_=nullptr;}
    window_=nullptr;pid_=0;pages_.clear();pending_.clear();
}
void SettingsWindow::Close(){
    if(closing_)return;closing_=true;
    if(window_&&IsWindow(window_))Call(window_,WM_CLOSE);
    Pump(1500);Drain();
    if(process_&&WaitForSingleObject(process_,0)!=WAIT_OBJECT_0){if(job_)TerminateJobObject(job_,1);WaitForSingleObject(process_,1000);}
    Reap();closing_=false;
}
bool SettingsWindow::Open(HWND owner,Read read,Apply apply){
    if(process_&&!window_&&GetTickCount64()<openingDeadline_&&WaitForSingleObject(process_,0)==WAIT_TIMEOUT)return true;
    if(window_&&IsWindow(window_)&&process_&&WaitForSingleObject(process_,0)==WAIT_TIMEOUT){
        ShowWindow(window_,SW_RESTORE);SetForegroundWindow(window_);return true;
    }
    Close();owner_=owner;read_=std::move(read);apply_=std::move(apply);saveOk_=true;revision_=acknowledged_=0;
    try{
        WNDCLASSW wc{};wc.lpfnWndProc=Procedure;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=ClassName;
        if(!RegisterClassW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw std::runtime_error("Settings bridge registration failed");
        bridge_=CreateWindowExW(0,ClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,wc.hInstance,this);
        if(!bridge_)throw std::runtime_error("Settings bridge creation failed");
        const auto resource=FindResourceW(nullptr,MAKEINTRESOURCEW(301),RT_RCDATA);
        const DWORD bytes=resource?SizeofResource(nullptr,resource):0;
        const auto data=resource?LockResource(LoadResource(nullptr,resource)):nullptr;
        if(!data||bytes<1024||bytes>1024*1024)throw std::runtime_error("Settings module unavailable");
        std::wstring directory;
#ifdef PULSE_DEVELOPMENT
        directory=development::DataDirectory();
#else
        wchar_t temporary[32768]{};DWORD count=GetTempPathW(32768,temporary);
        if(!count||count>=32768)throw std::runtime_error("Temporary directory unavailable");
        directory=temporary;
#endif
        GUID id{};wchar_t guid[40]{};
        if(FAILED(CoCreateGuid(&id))||!StringFromGUID2(id,guid,40))throw std::runtime_error("Settings module identity failed");
        payloadPath_=directory+L"\\Pulse.Settings."+guid+L".exe";
        HANDLE output=CreateFileW(payloadPath_.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY,nullptr);
        if(output==INVALID_HANDLE_VALUE){payloadPath_.clear();throw std::runtime_error("Settings module creation failed");}
        DWORD written=0;const bool saved=WriteFile(output,data,bytes,&written,nullptr)&&written==bytes&&FlushFileBuffers(output);
        CloseHandle(output);if(!saved)throw std::runtime_error("Settings module write failed");
        payload_=CreateFileW(payloadPath_.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(payload_==INVALID_HANDLE_VALUE)throw std::runtime_error("Settings module lock failed");
        std::vector<unsigned char> check(bytes);DWORD received=0;
        if(GetFileSize(payload_,nullptr)!=bytes||!ReadFile(payload_,check.data(),bytes,&received,nullptr)||received!=bytes||std::memcmp(check.data(),data,bytes))
            throw std::runtime_error("Settings module identity changed");
        job_=CreateJobObjectW(nullptr,nullptr);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if(!job_||!SetInformationJobObject(job_,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))throw std::runtime_error("Settings lifetime guard failed");
        std::wstring command=L"\""+payloadPath_+L"\" "+std::to_wstring(reinterpret_cast<UINT_PTR>(bridge_))+L" "+std::to_wstring(GetCurrentProcessId());
#ifdef PULSE_DEVELOPMENT
        command+=L" test";
#else
        command+=L" normal";
#endif
        STARTUPINFOW start{};start.cb=sizeof(start);PROCESS_INFORMATION child{};
        wchar_t desktop[256]{};DWORD needed=0;
        if(!GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,desktop,sizeof(desktop),&needed))throw std::runtime_error("Settings desktop unavailable");
        start.lpDesktop=desktop;
        if(!CreateProcessW(payloadPath_.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED,nullptr,directory.c_str(),&start,&child))
            throw std::runtime_error("Settings module launch failed");
        process_=child.hProcess;pid_=child.dwProcessId;
        if(!AssignProcessToJobObject(job_,process_)){TerminateProcess(process_,1);CloseHandle(child.hThread);throw std::runtime_error("Settings lifetime assignment failed");}
        ResumeThread(child.hThread);CloseHandle(child.hThread);openingDeadline_=GetTickCount64()+8000;
        if(!SetTimer(bridge_,1,500,nullptr))throw std::runtime_error("Settings lifecycle timer failed");
#ifdef PULSE_DEVELOPMENT
        // Only the isolated acceptance harness waits synchronously for controls.
        Pump(8000,true);
        if(!window_||!IsWindow(window_)||acknowledged_!=revision_||!revision_)throw std::runtime_error("Settings initialization failed");
#endif
        return true;
    }catch(...){Close();return false;}
}
void SettingsWindow::SendModel(){
    if(!window_||!IsWindow(window_)||closing_||draining_)return;
    pages_=read_();std::vector<unsigned char> bytes;Number(bytes,++revision_);Number(bytes,dark_);
    Text(bytes,Words(L"Pulse 设置",L"Pulse Settings"));
    Text(bytes,saveOk_?Words(L"更改后自动保存",L"Changes are saved automatically"):Words(L"保存失败，请重试",L"Save failed; please retry"));
    Number(bytes,static_cast<unsigned>(pages_.size()));
    for(const auto& page:pages_){Text(bytes,page.title);Text(bytes,page.description);Number(bytes,static_cast<unsigned>(page.rows.size()));
        for(const auto& row:page.rows){Number(bytes,static_cast<unsigned>(row.kind));Text(bytes,row.label);Number(bytes,static_cast<unsigned>(row.options.size()));
            for(const auto& o:row.options){Number(bytes,o.command);Number(bytes,o.enabled);Number(bytes,o.checked);Text(bytes,o.label);}}}
    if(bytes.size()>524288)throw std::runtime_error("Settings model too large");
    COPYDATASTRUCT packet{601,static_cast<DWORD>(bytes.size()),bytes.data()};
    Call(window_,WM_COPYDATA,reinterpret_cast<WPARAM>(bridge_),reinterpret_cast<LPARAM>(&packet));
}
LRESULT CALLBACK SettingsWindow::Procedure(HWND window,UINT message,WPARAM w,LPARAM l){
    auto* self=reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){self=static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
    return self?self->Message(window,message,w,l):DefWindowProcW(window,message,w,l);
}
LRESULT SettingsWindow::Message(HWND window,UINT message,WPARAM w,LPARAM l){
    if(message==WM_TIMER&&w==1&&!closing_){
        const bool exited=process_&&WaitForSingleObject(process_,0)==WAIT_OBJECT_0;
        const bool failed=!acknowledged_&&(exited||GetTickCount64()>openingDeadline_);
        if(exited||failed){Close();
#ifndef PULSE_DEVELOPMENT
            if(failed)MessageBoxW(owner_,Words(L"设置界面未能启动，请重试。",L"Settings could not start. Please retry."),L"Pulse",MB_OK|MB_ICONWARNING);
#endif
        }
        return 0;
    }
    if(message==RefreshMessage){MSG m{};while(PeekMessageW(&m,bridge_,RefreshMessage,RefreshMessage,PM_REMOVE)){}Drain();SendModel();return 0;}
    if(message==ApplyMessage){Drain();Refresh();return 0;}
    if(message==WM_COPYDATA){
        DWORD sender=0;GetWindowThreadProcessId(reinterpret_cast<HWND>(w),&sender);
        const auto* p=reinterpret_cast<const COPYDATASTRUCT*>(l);
        if(sender!=pid_||!pid_||!p||p->dwData!=602||p->cbData<12||p->cbData>2048)return FALSE;
        unsigned f[3]{};std::memcpy(f,p->lpData,12);
        if(f[2]>96||p->cbData!=12+f[2]*2)return FALSE;
        if(f[0]==2){window_=reinterpret_cast<HWND>(w);Refresh();return TRUE;}
        if(reinterpret_cast<HWND>(w)!=window_)return FALSE;
        if(f[0]==4){if(f[1]<=revision_)acknowledged_=f[1];return TRUE;}
        if(f[0]!=1)return FALSE;
        bool allowed=false;
        for(const auto& page:pages_)for(const auto& row:page.rows)for(const auto& o:row.options)
            if(o.command&&o.command==f[1]&&o.enabled&&(row.kind==SettingRow::Kind::Text||!f[2]))allowed=true;
        if(!allowed||pending_.size()>=128)return FALSE;
        std::wstring text(f[2],L'\0');std::memcpy(text.data(),static_cast<const unsigned char*>(p->lpData)+12,f[2]*2);
        if(text.find(L'\0')!=std::wstring::npos)return FALSE;
        pending_.emplace_back(f[1],std::move(text));PostMessageW(bridge_,ApplyMessage,0,0);return TRUE;
    }
    return DefWindowProcW(window,message,w,l);
}
#ifdef PULSE_DEVELOPMENT
void SettingsWindow::SelectPageForTest(size_t page){Refresh();SettleForTest();if(!Call(window_,SelectPage,page))throw std::runtime_error("Settings page unavailable");SettleForTest();}
HWND SettingsWindow::ControlForTest(UINT command)const{return reinterpret_cast<HWND>(Call(window_,FindControl,command));}
void SettingsWindow::CommitForTest(){Call(window_,Commit);SettleForTest();}
void SettingsWindow::SettleForTest(){Pump(100);if(!window_||!IsWindow(window_)||acknowledged_!=revision_)throw std::runtime_error("Settings update did not settle");}
bool SettingsWindow::ScrollForTest(){return Call(window_,ScrollCheck)!=0;}
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

#pragma once
#ifdef PULSE_DEVELOPMENT
#include "development.h"
#include <cstdint>
#include <vector>

namespace pulse::development {
constexpr wchar_t PeerClass[] = L"Pulse.Development.InputPeer.v1";
constexpr UINT PeerCount = WM_APP + 90;
inline LRESULT CALLBACK PeerProcedure(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_COPYDATA) {
        SetWindowLongPtrW(window, GWLP_USERDATA, GetWindowLongPtrW(window, GWLP_USERDATA) + 1);
        return TRUE; // Synthetic tray protocol sink; never claims Explorer visual acceptance.
    }
    if (message == PeerCount) return GetWindowLongPtrW(window, GWLP_USERDATA);
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, w, l);
}
inline int RunUiPeer(HINSTANCE instance) {
    RequireIsolatedDesktop();
    WNDCLASSW cls{};
    cls.lpfnWndProc = PeerProcedure; cls.hInstance = instance;
    cls.lpszClassName = PeerClass; cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&cls)) return 3;
    cls.lpszClassName = L"Shell_TrayWnd";
    if (!RegisterClassW(&cls)) return 3;
    const auto title = std::to_wstring(GetCurrentProcessId());
    HWND shell = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls.lpszClassName,
        title.c_str(), WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    HWND target = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, PeerClass,
        title.c_str(), WS_POPUP, 50, 50, 600, 400, nullptr, nullptr, instance, nullptr);
    if (!shell || !target) return 3;
    ShowWindow(target, SW_SHOWNOACTIVATE);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    if (IsWindow(target)) DestroyWindow(target);
    if (IsWindow(shell)) DestroyWindow(shell);
    return 0;
}
struct UiPeer {
    PROCESS_INFORMATION process{};
    HWND target = nullptr, shell = nullptr;
    explicit UiPeer(bool start) {
        if (!start) return;
        RequireIsolatedDesktop();
        if (FindWindowW(L"Shell_TrayWnd", nullptr)) throw std::runtime_error("Unexpected shell on test desktop");
        wchar_t exe[32768]{};
        if (!GetModuleFileNameW(nullptr, exe, 32768)) throw std::runtime_error("Peer executable unavailable");
        std::wstring command = L"\"" + std::wstring(exe) + L"\" --dev-ui-peer";
        std::wstring desktop = L"winsta0\\" + Environment(L"PULSE_DEV_DESKTOP");
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.lpDesktop = desktop.data();
        // The parent has already been assigned to the protected Job. The child inherits it.
        if (!CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &startup, &process)) throw std::runtime_error("Cannot start isolated UI peer");
        CloseHandle(process.hThread); process.hThread = nullptr;
        const auto title = std::to_wstring(process.dwProcessId);
        for (int attempt = 0; attempt < 200; ++attempt) {
            target = FindWindowW(PeerClass, title.c_str());
            shell = FindWindowW(L"Shell_TrayWnd", title.c_str());
            if (target && shell) break;
            if (WaitForSingleObject(process.hProcess, 10) == WAIT_OBJECT_0) break;
        }
        DWORD targetPid = 0, shellPid = 0;
        GetWindowThreadProcessId(target, &targetPid); GetWindowThreadProcessId(shell, &shellPid);
        if (!target || !shell || targetPid != process.dwProcessId || shellPid != process.dwProcessId)
            throw std::runtime_error("Isolated UI peer identity not ready");
    }
    bool Close() {
        if (!process.hProcess) return true;
        if (target) PostMessageW(target, WM_CLOSE, 0, 0);
        const bool exited = WaitForSingleObject(process.hProcess, 3000) == WAIT_OBJECT_0;
        DWORD code = 1;
        if (exited) GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hProcess); process.hProcess = nullptr;
        return exited && code == 0;
    }
    ~UiPeer() { Close(); }
};
inline bool SaveBitmap(HDC source, int width, int height, const wchar_t* name) {
    RequireIsolatedDesktop();
    if (width <= 0 || height <= 0 || static_cast<uint64_t>(width) * height > 4 * 1024 * 1024 ||
        std::wcspbrk(name, L"\\/:")) return false;
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HDC memory = CreateCompatibleDC(source);
    HBITMAP bitmap = CreateDIBSection(source, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    bool ok = false;
    if (memory && bitmap && pixels) {
        HGDIOBJ previous = SelectObject(memory, bitmap);
        if (BitBlt(memory, 0, 0, width, height, source, 0, 0, SRCCOPY)) {
            GdiFlush();
            BITMAPFILEHEADER header{}; header.bfType = 0x4D42;
            header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
            header.bfSize = header.bfOffBits + width * height * 4;
            std::vector<unsigned char> bytes(header.bfSize);
            std::memcpy(bytes.data(), &header, sizeof(header));
            std::memcpy(bytes.data() + sizeof(header), &info.bmiHeader, sizeof(info.bmiHeader));
            std::memcpy(bytes.data() + header.bfOffBits, pixels, width * height * 4);
            const auto path = DataDirectory() + L"\\" + name;
            HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
                CloseHandle(file);
            }
        }
        SelectObject(memory, previous);
    }
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    return ok;
}
}
#endif

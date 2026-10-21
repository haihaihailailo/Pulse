#include "ui_window.h"
#include <exception>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    pulse::ui::EnableDpiAwareness();
    try {
        return pulse::ui::Run(instance, showCommand);
    } catch (const std::exception&) {
        MessageBoxW(nullptr, L"Pulse 启动失败，请检查系统图形组件与可用内存。", L"Pulse", MB_OK | MB_ICONERROR);
    } catch (...) {
        MessageBoxW(nullptr, L"Pulse 遇到无法恢复的错误。", L"Pulse", MB_OK | MB_ICONERROR);
    }
    return 1;
}

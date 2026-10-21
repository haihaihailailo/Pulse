#include "ui_window.h"
#include "i18n.h"
#include <exception>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand) {
    pulse::i18n::SetLanguage(pulse::i18n::Preference::Auto);
    pulse::ui::EnableDarkMode();
    pulse::ui::EnableDpiAwareness();
    try {
        return pulse::ui::Run(instance, showCommand, commandLine);
    } catch (const std::exception&) {
        MessageBoxW(nullptr, pulse::i18n::T(pulse::i18n::Key::StartupFailed).c_str(), pulse::i18n::T(pulse::i18n::Key::AppName).c_str(), MB_OK | MB_ICONERROR);
    } catch (...) {
        MessageBoxW(nullptr, pulse::i18n::T(pulse::i18n::Key::FatalError).c_str(), pulse::i18n::T(pulse::i18n::Key::AppName).c_str(), MB_OK | MB_ICONERROR);
    }
    return 1;
}

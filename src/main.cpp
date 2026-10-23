#include "ui_window.h"
#include "i18n.h"
#include "development.h"
#include "development_ui.h"
#include <exception>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand) {
#ifdef PULSE_DEVELOPMENT
    try { pulse::development::Initialize(); }
    catch (const std::exception& error) { std::fprintf(stderr, "PULSE_DEV_REFUSED: %s\n", error.what()); return 2; }
#endif
    pulse::i18n::SetLanguage(pulse::i18n::Preference::Auto);
    pulse::ui::EnableDarkMode();
    pulse::ui::EnableDpiAwareness();
    try {
#ifdef PULSE_DEVELOPMENT
        if (pulse::development::ParseSmoke(commandLine) == pulse::development::SmokeMode::UiPeer)
            return pulse::development::RunUiPeer(instance);
#endif
        return pulse::ui::Run(instance, showCommand, commandLine);
    } catch (const std::exception& error) {
#ifdef PULSE_DEVELOPMENT
        std::fprintf(stderr, "PULSE_DEV_FAILED: %s\n", error.what());
#else
        MessageBoxW(nullptr, pulse::i18n::T(pulse::i18n::Key::StartupFailed).c_str(), pulse::i18n::T(pulse::i18n::Key::AppName).c_str(), MB_OK | MB_ICONERROR);
#endif
    } catch (...) {
#ifdef PULSE_DEVELOPMENT
        std::fprintf(stderr, "PULSE_DEV_FAILED: unknown exception\n");
#else
        MessageBoxW(nullptr, pulse::i18n::T(pulse::i18n::Key::FatalError).c_str(), pulse::i18n::T(pulse::i18n::Key::AppName).c_str(), MB_OK | MB_ICONERROR);
#endif
    }
    return 1;
}

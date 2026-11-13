#pragma once
#include <windows.h>
namespace pulse::update {
inline constexpr wchar_t Version[] = L"custom-2026.11.14.1";
// Starts only on an explicit menu action. Development builds never launch it.
bool Start(HWND owner, bool english);
#ifdef PULSE_DEVELOPMENT
// Exercises the real resource/UTF-8/PowerShell launch boundary without network or updates.
bool TestHelperLaunch();
#endif
}

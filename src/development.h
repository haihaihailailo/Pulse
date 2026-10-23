#pragma once
// Opt-in build variant. Normal upstream builds do not use these paths or hooks.
#ifdef PULSE_DEVELOPMENT
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <cwchar>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace pulse::development {
inline std::wstring Environment(const wchar_t* key) {
    const DWORD size = GetEnvironmentVariableW(key, nullptr, 0);
    if (!size || size > 32768) return {};
    std::wstring value(size, L'\0');
    const DWORD count = GetEnvironmentVariableW(key, value.data(), size);
    if (!count || count >= size) throw std::runtime_error("Development environment changed");
    value.resize(count);
    return value;
}
inline std::wstring FullPath(const std::wstring& value) {
    if (value.size() < 3 || value[1] != L':' || (value[2] != L'\\' && value[2] != L'/'))
        throw std::runtime_error("Development paths must be absolute local paths");
    wchar_t path[32768]{};
    const DWORD size = GetFullPathNameW(value.c_str(), 32768, path, nullptr);
    if (!size || size >= 32768) throw std::runtime_error("Invalid development path");
    return std::wstring(path, size);
}
inline std::wstring Parent(const std::wstring& path) { return path.substr(0, path.find_last_of(L'\\')); }
inline void CheckDirectory(const std::wstring& directory) {
    for (std::wstring path = directory; path.size() > 3; path = Parent(path)) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Development directory missing or linked");
    }
}
inline const std::wstring& DataDirectory() {
    static const std::wstring directory = [] {
        wchar_t executable[32768]{};
        const DWORD size = GetModuleFileNameW(nullptr, executable, 32768);
        if (!size || size >= 32768) throw std::runtime_error("Development executable path unavailable");
        // <repo>/dev-work/builds/<build-id>/<executable>
        const auto build = Parent(FullPath(executable));
        const auto builds = Parent(build);
        if (_wcsicmp(builds.substr(builds.find_last_of(L'\\') + 1).c_str(), L"builds"))
            throw std::runtime_error("Use the protected development build location");
        const auto work = Parent(builds);
        if (_wcsicmp(work.substr(work.find_last_of(L'\\') + 1).c_str(), L"dev-work"))
            throw std::runtime_error("Development work root mismatch");
        const auto path = FullPath(Environment(L"PULSE_DEV_DATA_DIR"));
        const auto prefix = work + L"\\sessions\\";
        if (path.size() <= prefix.size() || _wcsnicmp(path.c_str(), prefix.c_str(), prefix.size()) ||
            _wcsicmp(path.substr(path.find_last_of(L'\\') + 1).c_str(), L"profile"))
            throw std::runtime_error("Development profile must be inside this checkout's dev-work/sessions");
        CheckDirectory(build);
        CheckDirectory(path);
        const auto marker = Parent(path) + L"\\pulse-dev-session.json";
        const DWORD attributes = GetFileAttributesW(marker.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            throw std::runtime_error("Use tools/Pulse-Dev.ps1 to register a development session");
        return path;
    }();
    return directory;
}
inline std::wstring DesktopName(HDESK desktop) {
    wchar_t name[256]{};
    DWORD needed = 0;
    if (!desktop || !GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &needed))
        throw std::runtime_error("Desktop identity unavailable");
    return name;
}
inline void RequireIsolatedDesktop() {
    const auto name = DesktopName(GetThreadDesktop(GetCurrentThreadId()));
    const auto expected = Environment(L"PULSE_DEV_DESKTOP");
    if (expected.rfind(L"PulseDev-", 0) != 0 || name != expected)
        throw std::runtime_error("Smoke checks require the registered private desktop");
    HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!input) throw std::runtime_error("Input desktop identity unavailable");
    std::wstring visible;
    try { visible = DesktopName(input); } catch (...) { CloseDesktop(input); throw; }
    CloseDesktop(input);
    if (visible == name) throw std::runtime_error("Refusing smoke checks on the visible desktop");
}
inline void Initialize() {
    (void)DataDirectory();
    const auto purpose = Environment(L"PULSE_DEV_SESSION_KIND");
    if (purpose == L"test" || purpose == L"debug") RequireIsolatedDesktop();
    else if (purpose != L"interactive") throw std::runtime_error("Unregistered development session kind");
}
enum class SmokeMode { None, First, Resume, UiPeer };
inline void WriteSmokeReceipt(const char* json, bool resumed) {
    const auto path = DataDirectory() + (resumed ? L"\\smoke-resume.json" : L"\\smoke-first.json");
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create development smoke receipt");
    const DWORD size = static_cast<DWORD>(std::strlen(json));
    DWORD written = 0;
    const bool saved = WriteFile(file, json, size, &written, nullptr) && written == size && FlushFileBuffers(file);
    const bool closed = CloseHandle(file) != FALSE;
    if (!saved || !closed) throw std::runtime_error("Development smoke receipt write failed");
}
inline SmokeMode ParseSmoke(const wchar_t* command) {
    // wWinMain receives raw text; launchers may quote arguments or append whitespace.
    // Supply a dummy executable so CommandLineToArgvW applies normal argument rules.
    const std::wstring line = L"Pulse-Dev " + std::wstring(command ? command : L"");
    int count = 0;
    wchar_t** args = CommandLineToArgvW(line.c_str(), &count);
    if (!args) throw std::runtime_error("Development arguments could not be parsed");
    const bool empty = count == 1;
    const bool first = count == 2 && std::wcscmp(args[1], L"--dev-smoke") == 0;
    const bool resume = count == 2 && std::wcscmp(args[1], L"--dev-smoke-resume") == 0;
    const bool peer = count == 2 && std::wcscmp(args[1], L"--dev-ui-peer") == 0;
    LocalFree(args);
    if (first) return SmokeMode::First;
    if (resume) return SmokeMode::Resume;
    if (peer) { RequireIsolatedDesktop(); return SmokeMode::UiPeer; }
    if (empty && Environment(L"PULSE_DEV_SESSION_KIND") == L"interactive") return SmokeMode::None;
    throw std::runtime_error("Expected one registered development smoke argument");
}
}
#endif

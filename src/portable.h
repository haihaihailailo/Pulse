#pragma once
// An explicit sibling marker opts a normal build into a separate local profile.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <stdexcept>

namespace pulse::portable {
inline std::wstring directory;
inline bool Enabled() { return !directory.empty(); }
inline void Initialize() {
#ifndef PULSE_DEVELOPMENT
    wchar_t executable[32768]{};
    const DWORD count = GetModuleFileNameW(nullptr, executable, 32768);
    if (!count || count >= 32768) throw std::runtime_error("Executable path unavailable");
    const std::wstring path(executable, count);
    const auto parent = path.substr(0, path.find_last_of(L"\\/"));
    const auto marker = parent + L"\\pulse-portable.ini";
    const DWORD attributes = GetFileAttributesW(marker.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return;
        throw std::runtime_error("Portable marker cannot be read");
    }
    if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("Portable marker must be a regular file");
    // Never redirect the portable profile through a junction into an installed profile.
    for (auto current = parent; current.size() > 3; current = current.substr(0, current.find_last_of(L"\\/"))) {
        const DWORD flags = GetFileAttributesW(current.c_str());
        if (flags == INVALID_FILE_ATTRIBUTES || (flags & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Portable directory must not be linked");
    }
    const auto data = parent + L"\\data";
    if (!CreateDirectoryW(data.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        throw std::runtime_error("Cannot create portable data directory");
    const DWORD flags = GetFileAttributesW(data.c_str());
    if (flags == INVALID_FILE_ATTRIBUTES || !(flags & FILE_ATTRIBUTE_DIRECTORY) || (flags & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("Invalid portable data directory");
    for (const wchar_t* name : {L"settings.ini", L"pulse_net.log", L"pulse_net.log.1"}) {
        const auto file = data + L"\\" + name;
        const DWORD fileFlags = GetFileAttributesW(file.c_str());
        if (fileFlags != INVALID_FILE_ATTRIBUTES && (fileFlags & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            throw std::runtime_error("Linked portable profile file refused");
    }
    directory = data;
#endif
}
}

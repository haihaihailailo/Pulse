#include "updater.h"
#include <objbase.h>
#include <sddl.h>
#include <string>
#include <vector>

namespace pulse::update {
namespace {
bool Launch(HWND owner, bool english, bool libraryOnly) {
    wchar_t exe[32768]{}, temp[32768]{}, system[32768]{}, id[40]{};
    const DWORD length = GetModuleFileNameW(nullptr, exe, 32768);
    const DWORD tempLength = GetTempPathW(32768, temp);
    if (!length || length >= 32768 || !tempLength || tempLength >= 32768 ||
        !GetSystemDirectoryW(system, 32768)) return false;
    // Never execute an updater from a linked temp path, including ancestors.
    std::wstring parent(temp);
    while (parent.size() > 3 && (parent.back() == L'\\' || parent.back() == L'/')) parent.pop_back();
    for (auto p = parent; p.size() > 3; p = p.substr(0, p.find_last_of(L"\\/"))) {
        const DWORD a = GetFileAttributesW(p.c_str());
        if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    }
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, id, 40)) return false;
    const std::wstring dir = parent + L"\\Pulse.Update-" + id;
    // A high-integrity Pulse must not execute a medium-writable helper script.
    // Private ACL and inherited mandatory label follow the current token.
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    std::vector<unsigned char> tokenUser(needed);
    bool privateDirectory = GetTokenInformation(token, TokenUser, tokenUser.data(), needed, &needed) != FALSE;
    TOKEN_ELEVATION elevation{}; DWORD size = sizeof(elevation);
    privateDirectory = privateDirectory && GetTokenInformation(token, TokenElevation, &elevation, size, &size);
    CloseHandle(token);
    LPWSTR sid = nullptr;
    privateDirectory = privateDirectory && ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(tokenUser.data())->User.Sid, &sid);
    if (!privateDirectory) return false;
    const std::wstring sddl = std::wstring(L"D:P(A;OICI;FA;;;") + sid +
        L")(A;OICI;FA;;;SY)S:(ML;OICI;NW;;;" + (elevation.TokenIsElevated ? L"HI)" : L"ME)");
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return false;
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    const BOOL createdDirectory = CreateDirectoryW(dir.c_str(), &security);
    LocalFree(descriptor);
    if (!createdDirectory) return false;
    const auto file = dir + L"\\update.ps1";
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(201), MAKEINTRESOURCEW(10));
    HGLOBAL loaded = resource ? LoadResource(nullptr, resource) : nullptr;
    const DWORD bytes = resource ? SizeofResource(nullptr, resource) : 0;
    const void* data = loaded ? LockResource(loaded) : nullptr;
    if (!data || !bytes || bytes > 65536) { RemoveDirectoryW(dir.c_str()); return false; }
    HANDLE out = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (out == INVALID_HANDLE_VALUE) { RemoveDirectoryW(dir.c_str()); return false; }
    const unsigned char bom[] = {0xef, 0xbb, 0xbf};
    DWORD written = 0;
    bool ok = WriteFile(out, bom, 3, &written, nullptr) && written == 3;
    ok = ok && WriteFile(out, data, bytes, &written, nullptr) && written == bytes;
    CloseHandle(out);
    FILETIME created{}, exited{}, kernel{}, user{};
    ok = ok && GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    ULARGE_INTEGER time{}; time.LowPart = created.dwLowDateTime; time.HighPart = created.dwHighDateTime;
    const std::wstring powershell = std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    std::wstring command = L"\"" + powershell + L"\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" +
        file + L"\" -Target \"" + exe + L"\" -CurrentVersion " + Version + L" -OwnerPid " +
        std::to_wstring(GetCurrentProcessId()) + L" -OwnerCreated " + std::to_wstring(time.QuadPart) +
        L" -OwnerWindow " + std::to_wstring(reinterpret_cast<UINT_PTR>(owner)) +
        L" -Language " + (english ? L"en" : L"zh");
    if (libraryOnly) command += L" -LibraryOnly";
    std::vector<wchar_t> buffer(command.begin(), command.end()); buffer.push_back(0);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    ok = ok && CreateProcessW(powershell.c_str(), buffer.data(), nullptr, nullptr, FALSE,
                              CREATE_NO_WINDOW, nullptr, dir.c_str(), &startup, &process);
    if (ok) {
        if (libraryOnly) {
            DWORD exitCode = 1;
            ok = WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0 &&
                GetExitCodeProcess(process.hProcess, &exitCode) && exitCode == 0;
        }
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
    }
    if (!ok || libraryOnly) { DeleteFileW(file.c_str()); RemoveDirectoryW(dir.c_str()); }
    return ok;
}
}
bool Start(HWND owner, bool english) {
#ifdef PULSE_DEVELOPMENT
    (void)owner; (void)english;
    return false;
#else
    return Launch(owner, english, false);
#endif
}
#ifdef PULSE_DEVELOPMENT
bool TestHelperLaunch() { return Launch(nullptr, false, true); }
#endif
}

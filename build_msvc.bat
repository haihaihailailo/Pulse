@echo off
setlocal EnableExtensions DisableDelayedExpansion
pushd "%~dp0"
if errorlevel 1 exit /b 1

where cl.exe >nul 2>&1
if errorlevel 1 (
    echo cl.exe was not found. Use an x64 MSVC developer command prompt.
    goto :failed
)
where rc.exe >nul 2>&1
if errorlevel 1 (
    echo rc.exe was not found. Install the Windows SDK and use an x64 developer command prompt.
    goto :failed
)

if not exist "build-msvc\pulse" mkdir "build-msvc\pulse"
if errorlevel 1 goto :failed
if not exist "build-msvc\selftest" mkdir "build-msvc\selftest"
if errorlevel 1 goto :failed

rem Compile from the repository root so app.rc can resolve app.manifest.
rc /nologo /c65001 /fo "build-msvc\app.res" app.rc
if errorlevel 1 goto :failed

rem Source definitions of UNICODE, _UNICODE and NOMINMAX are #ifndef guarded.
set "PULSE_CXXFLAGS=/nologo /std:c++17 /EHsc /O2 /utf-8 /MT /W3 /DUNICODE /D_UNICODE /DNOMINMAX"
rem Same system libraries as build.bat; GUI-only libraries are added below.
set "PULSE_BACKEND_LIBS=winhttp.lib powrprof.lib pdh.lib iphlpapi.lib ntdll.lib winmm.lib shell32.lib gdi32.lib user32.lib ole32.lib advapi32.lib uuid.lib ws2_32.lib setupapi.lib"

rem Match build.bat: every src\*.cpp except the console self-test.
set "PULSE_SOURCES="
for %%F in (src\*.cpp) do if /I not "%%~nxF"=="metrics_selftest.cpp" call :add_source "%%F"
rem Select the Unicode GUI CRT startup, which initializes the CRT then calls wWinMain.
rem app.rc already embeds app.manifest as resource 1; do not generate a second manifest.
cl %PULSE_CXXFLAGS% /Fobuild-msvc\pulse\ /Febuild-msvc\Pulse.exe %PULSE_SOURCES% build-msvc\app.res /link /MACHINE:X64 /INCREMENTAL:NO /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup /MANIFEST:NO d2d1.lib dwrite.lib dwmapi.lib comctl32.lib %PULSE_BACKEND_LIBS%
if errorlevel 1 goto :failed

rem src\metrics_selftest.cpp defines wmain; use the Unicode console CRT startup.
rem Match build.bat's four self-test sources and backend libraries exactly.
cl %PULSE_CXXFLAGS% /Fobuild-msvc\selftest\ /Febuild-msvc\metrics_selftest.exe src\metrics_selftest.cpp src\metrics.cpp src\netprobe.cpp src\fpsmon.cpp /link /MACHINE:X64 /INCREMENTAL:NO /SUBSYSTEM:CONSOLE /ENTRY:wmainCRTStartup /MANIFEST:EMBED %PULSE_BACKEND_LIBS%
if errorlevel 1 goto :failed

echo Built build-msvc\Pulse.exe and build-msvc\metrics_selftest.exe.
popd
exit /b 0

:add_source
set "PULSE_SOURCES=%PULSE_SOURCES% "%~1""
exit /b 0

:failed
echo MSVC build failed.
popd
exit /b 1

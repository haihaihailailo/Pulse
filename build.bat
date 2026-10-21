@echo off
setlocal EnableExtensions DisableDelayedExpansion
pushd "%~dp0"
if errorlevel 1 exit /b 1

rem MinGW-w64 g++ 13.2 x64. Do not include the console self-test in Pulse.exe.
windres -F pe-x86-64 -i app.rc -O coff -o app.res.o
if errorlevel 1 goto :failed

set "PULSE_SOURCES="
for %%F in (src\*.cpp) do if /I not "%%~nxF"=="metrics_selftest.cpp" call :add_source "%%F"
g++ -std=c++17 -O2 -s -municode -mwindows -static -pthread -finput-charset=UTF-8 -fexec-charset=UTF-8 -o Pulse.exe %PULSE_SOURCES% app.res.o -ld2d1 -ldwrite -ldwmapi -lwinhttp -lpowrprof -lpdh -liphlpapi -lntdll -lwinmm -lshell32 -lgdi32 -luser32 -lole32 -ladvapi32 -lcomctl32 -luuid -lws2_32 -lsetupapi
if errorlevel 1 goto :failed

rem Select the console CRT entry without constraining the parallel backend implementation.
set "SELFTEST_ENTRY="
findstr /R /C:"wmain[ (]" src\metrics_selftest.cpp >nul
if not errorlevel 1 set "SELFTEST_ENTRY=-municode"
g++ -std=c++17 -O2 -s -static -pthread %SELFTEST_ENTRY% -finput-charset=UTF-8 -fexec-charset=UTF-8 -o metrics_selftest.exe src/metrics_selftest.cpp src/metrics.cpp src/netprobe.cpp -lwinhttp -lpowrprof -lpdh -liphlpapi -lntdll -lwinmm -lshell32 -lgdi32 -luser32 -lole32 -ladvapi32 -luuid -lws2_32 -lsetupapi
if errorlevel 1 goto :failed
echo Built Pulse.exe and metrics_selftest.exe.
popd
exit /b 0

:add_source
set "PULSE_SOURCES=%PULSE_SOURCES% "%~1""
exit /b 0

:failed
echo Build failed.
popd
exit /b 1

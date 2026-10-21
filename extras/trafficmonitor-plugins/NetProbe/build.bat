@echo off
setlocal
pushd "%~dp0"
g++ -std=c++17 -O2 -shared -static -s -finput-charset=UTF-8 -fexec-charset=UTF-8 -o NetProbe.dll NetProbePlugin.cpp -lwinhttp -lwinmm -lgdi32 -luser32 -lshell32
set "NETPROBE_BUILD_EXIT=%ERRORLEVEL%"
popd
exit /b %NETPROBE_BUILD_EXIT%

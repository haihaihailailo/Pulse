@echo off
cd /d "%~dp0"
g++ -std=c++17 -O2 -shared -static -s -finput-charset=UTF-8 -fexec-charset=UTF-8 -o BatteryPlus.dll BatteryPlusPlugin.cpp -lpowrprof -lpdh -lgdi32 -luser32
if errorlevel 1 exit /b 1

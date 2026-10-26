@echo off
rem Builds and runs the 3D Auto Track test suite (TrackTest.cpp, which
rem compiles ..\ReferenceScaler.cpp in). Arguments go to TrackTest: [model.obj]
rem [model scale] [out folder]. Exit code 0 when every cell passes.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /O2 /EHsc /MT /std:c++20 /DNOMINMAX TrackTest.cpp /Fe:TrackTest.exe /Fo:%TEMP%\ /link /IMPLIB:%TEMP%\TrackTest.lib gdiplus.lib gdi32.lib user32.lib d3d11.lib dxgi.lib shlwapi.lib ole32.lib dwmapi.lib windowsapp.lib || exit /b 2
.\TrackTest.exe %*

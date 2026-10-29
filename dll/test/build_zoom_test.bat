@echo off
if "%~1"=="" exit /b 2
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /O2 /EHsc /MT /std:c++20 /DNOMINMAX ZoomTest.cpp /Fe:"%TEMP%\JRefZoomTest.exe" /Fo:"%TEMP%\JRefZoomTest.obj" /link /IMPLIB:"%TEMP%\JRefZoomTest.lib" gdiplus.lib gdi32.lib user32.lib d3d11.lib dxgi.lib shlwapi.lib ole32.lib dwmapi.lib windowsapp.lib || exit /b 2
"%TEMP%\JRefZoomTest.exe" "%~1"

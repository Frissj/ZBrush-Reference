@echo off
if "%~1"=="" exit /b 2
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /O2 /EHsc /MT /std:c++20 /DNOMINMAX AutoFitDepthTest.cpp /Fe:"%TEMP%\JRefAutoFitDepthTest.exe" /Fo:"%TEMP%\JRefAutoFitDepthTest.obj" /link /IMPLIB:"%TEMP%\JRefAutoFitDepthTest.lib" gdiplus.lib gdi32.lib user32.lib d3d11.lib dxgi.lib shlwapi.lib ole32.lib dwmapi.lib windowsapp.lib || exit /b 2
"%TEMP%\JRefAutoFitDepthTest.exe" "%~1" "%~2" "%~3" "%~4" "%~5" "%~6"

@echo off
if "%~1"=="" exit /b 2
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /O2 /EHsc /MT /std:c++20 /DNOMINMAX RotationTest.cpp /Fe:"%TEMP%\JRefRotationTest.exe" /Fo:"%TEMP%\JRefRotationTest.obj" /link /IMPLIB:"%TEMP%\JRefRotationTest.lib" gdiplus.lib gdi32.lib user32.lib d3d11.lib dxgi.lib shlwapi.lib ole32.lib dwmapi.lib windowsapp.lib || exit /b 2
"%TEMP%\JRefRotationTest.exe" "%~1"

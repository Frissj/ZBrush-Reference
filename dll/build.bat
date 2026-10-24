@echo off
rem Builds ..\JRef_Data\JRef64.dll (or the path given) with Visual Studio's
rem compiler (x64), and writes ..\JRef_Data\build.txt ("build N;", from
rem kDllBuild), which the installed copy compares with its own build at
rem startup to update itself.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if "%~1"=="" (set OUT=..\JRef_Data\JRef64.dll) else (set OUT=%~1)
rem ZBrush may have the DLL loaded: a loaded DLL can be renamed, then replaced.
if exist "%OUT%.old" del "%OUT%.old" 2>nul
if exist "%OUT%" move /y "%OUT%" "%OUT%.old" >nul 2>nul
cl /nologo /O2 /EHsc /MT /LD /std:c++20 /DNOMINMAX ReferenceScaler.cpp /Fe:"%OUT%" /Fo:%TEMP%\ /link /IMPLIB:%TEMP%\JRef64.lib gdiplus.lib gdi32.lib user32.lib d3d11.lib dxgi.lib shlwapi.lib ole32.lib dwmapi.lib windowsapp.lib || exit /b 1
if not "%~1"=="" exit /b 0
for /f "tokens=6 delims=; " %%a in ('findstr /c:"static const int kDllBuild" ReferenceScaler.cpp') do (echo build %%a;)> ..\JRef_Data\build.txt

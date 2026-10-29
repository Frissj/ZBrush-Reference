@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /O2 /EHsc /MT /std:c++20 /DNOMINMAX AutoFitTest.cpp /Fe:"%TEMP%\JRefAutoFitTest.exe" /Fo:"%TEMP%\JRefAutoFitTest.obj" || exit /b 2
"%TEMP%\JRefAutoFitTest.exe"

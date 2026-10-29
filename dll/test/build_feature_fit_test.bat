@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /O2 /EHsc /MT /std:c++20 /DNOMINMAX FeatureFitTest.cpp /Fe:"%TEMP%\JRefFeatureFitTest.exe" /Fo:"%TEMP%\JRefFeatureFitTest.obj" || exit /b 2
"%TEMP%\JRefFeatureFitTest.exe"

@echo off
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH ( echo Could not find MSVC & exit /b 1 )
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if not exist "%~dp0obj" mkdir "%~dp0obj"
cl /nologo /std:c++20 /EHsc /O2 /MT /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE ^
   "%~dp0icon-preview.cpp" /Fe:"%~dp0obj\icon-preview.exe" /Fo:"%~dp0obj\\" /link /SUBSYSTEM:CONSOLE

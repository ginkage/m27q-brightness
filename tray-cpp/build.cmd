@echo off
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Could not find vswhere.exe
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo Could not find an MSVC toolset. Install the Desktop development with C++ workload.
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo vcvars64.bat failed
    exit /b 1
)

if not exist "%~dp0obj" mkdir "%~dp0obj"

cl /nologo /std:c++20 /EHsc /W4 /O2 /GL /MT /permissive- ^
   /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE ^
   "%~dp0main.cpp" ^
   /Fe:"%~dp0brighttray.exe" /Fo:"%~dp0obj\\" ^
   /link /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF

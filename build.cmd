@echo off
setlocal
set CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe
if not exist "%CSC%" (
    echo Could not find csc.exe at "%CSC%"
    exit /b 1
)
"%CSC%" /nologo /target:exe /platform:anycpu /optimize+ /out:"%~dp0bright.exe" "%~dp0Program.cs"

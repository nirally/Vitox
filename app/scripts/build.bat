@echo off
rem Vitox M2 Probe - Release x64 build + signed sideload package (same identity/cert as M1).
setlocal
set NoDefaultCurrentDirectoryInExePath=
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set /p PFXPW=<"C:\Vitox\Vitox-M1\cert\pfx_password.txt"
msbuild "%~dp0..\VitoxM2Probe.vcxproj" /nologo /m:1 /v:minimal /p:Configuration=Release /p:Platform=x64 /p:PackageCertificatePassword=%PFXPW% %*
set RC=%ERRORLEVEL%
echo EXIT=%RC%
exit /b %RC%

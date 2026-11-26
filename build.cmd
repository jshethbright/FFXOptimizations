@echo off
setlocal
set "ROOT=%~dp0"
set "OUT=%ROOT%build"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x86
if errorlevel 1 exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /W4 /WX /EHsc /std:c++17 /O2 /MT /LD /Fo"%OUT%\FFXTexturePrefetch.obj" "%ROOT%FFXTexturePrefetch.cpp" /link /IMPLIB:"%OUT%\FFXTexturePrefetch.lib" /OUT:"%OUT%\FFXTexturePrefetch.dll"
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /EHsc /std:c++17 /O2 /MT /DFFX_PREFETCH_TEST /Fo"%OUT%\prefetch-test.obj" /Fe"%OUT%\prefetch-test.exe" "%ROOT%FFXTexturePrefetch.cpp"
exit /b %ERRORLEVEL%

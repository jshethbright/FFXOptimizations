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
set "SDK=%~1"
if not defined SDK set "SDK=%RESHADE_SDK_INCLUDE%"
if not defined SDK set "SDK=%ROOT%third_party\reshade\include"
if not exist "%SDK%\reshade.hpp" (
    echo Pass the ReShade SDK include directory or set RESHADE_SDK_INCLUDE.
    exit /b 1
)
cl /nologo /W4 /WX /EHsc /std:c++17 /O2 /MT /DFFX_GPU_TRACE_TEST /Fo"%OUT%\gpu-trace-test.obj" /Fe"%OUT%\gpu-trace-test.exe" "%ROOT%FFXGpuTextureTrace.cpp" /link d3d11.lib dxguid.lib
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /EHsc /std:c++17 /O2 /MT /LD /external:I"%SDK%" /external:W0 /Fo"%OUT%\FFXGpuTextureTrace.obj" "%ROOT%FFXGpuTextureTrace.cpp" /link dxguid.lib /IMPLIB:"%OUT%\FFXGpuTextureTrace.lib" /OUT:"%OUT%\FFXGpuTextureTrace.addon32"
exit /b %ERRORLEVEL%
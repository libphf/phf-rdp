@echo off
setlocal
cd /d %~dp0

where cl.exe >nul 2>nul
if not errorlevel 1 goto compile

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" goto findvs
echo [Error] cl.exe not in PATH and vswhere.exe not found.
exit /b 1

:findvs
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo [Error] Visual Studio C++ toolset not found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul

:compile
echo [Build] Compiling phf-rdp-server.exe (C Native with OpenMP and DirectX 11)...
cl.exe /nologo /O2 /openmp /MD /I include /I src ^
    src\main.c ^
    src\capture_win_dxgi.c ^
    src\phf.c ^
    src\huff.c ^
    src\mongoose.c ^
    src\tray_win.c ^
    /link d3d11.lib dxgi.lib dxguid.lib gdi32.lib user32.lib ws2_32.lib winmm.lib ole32.lib shell32.lib ^
    /out:phf-rdp-server.exe

if errorlevel 1 (
    echo [Build] Compilation failed!
    exit /b 1
)

echo [Build] Compilation succeeded! phf-rdp-server.exe created.

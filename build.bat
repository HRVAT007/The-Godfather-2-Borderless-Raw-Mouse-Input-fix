@echo off
setlocal
cd /d "%~dp0"

rem --- Locate a Visual Studio 2017+ x86 build environment via vswhere ---
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars32.bat"
)
if not defined VCVARS (
  echo.
  echo ERROR: Could not locate Visual Studio vcvars32.bat.
  echo Install Visual Studio 2022 with the "Desktop development with C++" workload,
  echo or edit this script to point at your vcvars32.bat.
  echo.
  exit /b 1
)
call "%VCVARS%" >nul

rem --- Build the ASI (32-bit DLL). ---------------------------------------------
rem The output is deliberately NOT named gf2fix.asi: Modern Fixes treats any ASI whose name
rem contains GF2 as a competing mod and yields its shared exe patches to it, which crashes the
rem game on boot on modern CPUs. Building it under the install name makes that impossible to get
rem wrong. The source keeps the gf2fix name, and so does its generated gf2fix.ini.
cl /nologo /O2 /MT /LD /W3 /DNDEBUG /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0601 ^
   /I minhook\include ^
   src\gf2fix.cpp minhook\src\buffer.c minhook\src\hook.c minhook\src\trampoline.c minhook\src\hde\hde32.c ^
   /Fe:rawmouse.asi /link user32.lib gdi32.lib
if %errorlevel% neq 0 (
  echo BUILD FAILED
  exit /b 1
)
del /q gf2fix.obj buffer.obj hook.obj trampoline.obj hde32.obj gf2fix.lib gf2fix.exp 2>nul
echo BUILD OK: rawmouse.asi
endlocal

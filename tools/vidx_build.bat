@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars32.bat" >nul
cl /nologo /O2 /MT /W3 vidx_test.cpp /Fe:vidx_test.exe /link user32.lib
if %errorlevel% neq 0 exit /b 1
vidx_test.exe

@echo off
setlocal EnableExtensions EnableDelayedExpansion
rem Builds build\winmm.dll (the mod) and build\offline_check.exe (a self-test against the game exe).
rem Requires Visual Studio 2022 or its Build Tools with the C++ workload.

set "VSDIR="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
rem Delayed expansion keeps the "(x86)" in the path from ending the for /f clause early.
for /f "usebackq delims=" %%I in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%I"
if not defined VSDIR (
  echo [error] Visual Studio C++ build tools not found.
  exit /b 1
)
rem vcvars64 probes for vswhere on PATH and complains harmlessly when it isn't there.
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1

cd /d "%~dp0"
if not exist build mkdir build

rem /utf-8: the sources are UTF-8 (the menu uses characters such as the degree sign).
set CFLAGS=/nologo /utf-8 /O2 /MT /EHa /std:c++20 /W4 /wd4100 /Zi /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE
set SOURCES=src\main.cpp src\bob.cpp src\canvas.cpp src\config.cpp src\fov.cpp src\game.cpp src\input.cpp src\log.cpp src\scan.cpp src\ue.cpp src\ui.cpp

echo Building winmm.dll ...
rc /nologo /fo build\version.res src\version.rc || exit /b 1
rem /PDBALTPATH: embed only "winmm.pdb" in the DLL, not the full path of this build folder.
cl %CFLAGS% /LD %SOURCES% build\version.res /Fo:build\ /Fd:build\ /Fe:build\winmm.dll /link /DEBUG /PDBALTPATH:%%_PDB%% /OPT:REF /OPT:ICF user32.lib || exit /b 1

echo Building offline_check.exe ...
cl %CFLAGS% tools\offline_check.cpp src\game.cpp src\log.cpp src\scan.cpp src\ue.cpp /Fo:build\ /Fd:build\ /Fe:build\offline_check.exe /link user32.lib || exit /b 1

echo Done: build\winmm.dll
exit /b 0

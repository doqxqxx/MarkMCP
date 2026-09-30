@echo off
rem Build script: compile plug1.dll and test host into test\build\
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VCVARS% set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"

call %VCVARS% >nul 2>&1
if errorlevel 1 (
    echo [build] failed to set up MSVC environment
    exit /b 1
)

set OUTDIR=%~dp0build
if not exist %OUTDIR% mkdir %OUTDIR%

rem ---- plug1.dll (matches vcxproj Release x64 settings, Unicode charset) ----
cl /nologo /W3 /EHsc /O2 /MT /DNDEBUG /D_WINDOWS /D_USRDLL /DPLUG1_EXPORTS /D_UNICODE /DUNICODE /utf-8 /LD ^
  /Fe:%OUTDIR%\plug1.dll /Fo:%OUTDIR%\ ^
  /Fd:%OUTDIR%\plug1.pdb ^
  "%~dp0..\plug1\dllmain.cpp" "%~dp0..\plug1\MarkMCP.cpp" "%~dp0..\plug1\McpServer.cpp" ^
  /link /DEFAULTLIB:user32.lib
if errorlevel 1 (
    echo [build] plug1.dll build failed
    exit /b 1
)

rem ---- test host ----
cl /nologo /W3 /EHsc /MT /utf-8 ^
  /Fe:%OUTDIR%\mcp_test_host.exe /Fo:%OUTDIR%\ ^
  "%~dp0mcp_test_host.cpp"
if errorlevel 1 (
    echo [build] mcp_test_host.exe build failed
    exit /b 1
)

echo [build] OK
endlocal

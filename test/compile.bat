@echo off
setlocal

:: Set temp directories to user-writable location
set TEMP=C:\Users\cgrah\AppData\Local\Temp
set TMP=C:\Users\cgrah\AppData\Local\Temp
set TMPDIR=C:\Users\cgrah\AppData\Local\Temp

:: Ensure MinGW is in PATH
set PATH=C:\msys64\mingw64\bin;%PATH%

:: Change to test directory
cd /d E:\SierraChart\ACS_Source\test

:: Check if test name provided
if "%~1"=="" goto :usage

:: Handle "all" mode - run all tests
if "%~1"=="all" goto :all

:: Handle syntax-only mode
if "%~2"=="syntax" goto :syntax

:: Handle clean mode
if "%~2"=="clean" (
    echo [CLEAN] Removing %1.exe ...
    del /f %1.exe 2>nul
)

:: Compile
echo [COMPILE] %1.cpp ...
C:\msys64\mingw64\bin\g++.exe -std=c++17 -I.. -o %1.exe %1.cpp 2>&1

if %ERRORLEVEL% NEQ 0 (
    echo [FAILED] Compilation failed
    exit /b 1
)

echo [SUCCESS] Built %1.exe

:: Run if second arg is "run" or "clean"
if "%~2"=="run" goto :run
if "%~2"=="clean" goto :run
goto :end

:syntax
echo [SYNTAX] Checking %1.cpp ...
C:\msys64\mingw64\bin\g++.exe -std=c++17 -I.. -fsyntax-only %1.cpp 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [FAILED] Syntax errors found
    exit /b 1
)
echo [SUCCESS] Syntax OK
goto :end

:run
echo.
echo [RUNNING] %1.exe
echo ========================================
E:\SierraChart\ACS_Source\test\%1.exe
goto :end

:all
echo ========================================
echo [ALL] Running all tests...
echo ========================================
set FAILED=0
for %%t in (test_ssot_invariants test_posture_rejection) do (
    echo.
    echo [COMPILE] %%t.cpp ...
    C:\msys64\mingw64\bin\g++.exe -std=c++17 -I.. -o %%t.exe %%t.cpp 2>&1
    if errorlevel 1 (
        echo [FAILED] %%t compile failed
        set FAILED=1
    ) else (
        echo [RUN] %%t.exe ...
        E:\SierraChart\ACS_Source\test\%%t.exe
        if errorlevel 1 set FAILED=1
    )
)
echo.
echo ========================================
if %FAILED%==1 (
    echo [RESULT] Some tests FAILED
    exit /b 1
) else (
    echo [RESULT] All tests PASSED
    exit /b 0
)

:usage
echo Usage: compile.bat test_name [mode]
echo.
echo Modes:
echo   (none)  - compile only
echo   run     - compile and run
echo   syntax  - syntax check only (no exe)
echo   clean   - remove exe then compile and run
echo   all     - compile and run ALL tests
echo.
echo Examples:
echo   compile.bat test_ssot_invariants
echo   compile.bat test_ssot_invariants run
echo   compile.bat test_ssot_invariants syntax
echo   compile.bat all
exit /b 1

:end
endlocal

@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d C:\Users\cgrah\Desktop\AMT
echo Compiling AuctionSensor_v1.cpp...
cl /nologo /c /EHsc /std:c++20 /I. /W3 /DWIN32 /D_WIN32 /D_WINDOWS AuctionSensor_v1.cpp
if %ERRORLEVEL% EQU 0 (
    echo Build SUCCEEDED
) else (
    echo Build FAILED
)
exit /b %ERRORLEVEL%

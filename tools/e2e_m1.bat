@echo off
rem The flat room end to end with simulated hands. It moves the real cursor and types: run it while nobody's
rem at the PC. It builds, opens the S3 test page in Chrome, puts it in a fresh layout (yours is put back after),
rem starts the room, runs e2e_m1 and writes out\e2e.log.
cd /d "%~dp0\.."
if not exist out mkdir out
set CMAKE="E:\VS\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
%CMAKE% --build --preset release -- /m /v:m > out\e2e_build.log 2>&1
if errorlevel 1 (echo BUILD FAILED > out\e2e.log & exit /b 1)
taskkill /f /im FullImmersion.exe > nul 2>&1
set LAYOUT=%APPDATA%\FullImmersion\layout.json
if not exist "%APPDATA%\FullImmersion" mkdir "%APPDATA%\FullImmersion"
if exist "%LAYOUT%" copy /y "%LAYOUT%" "%LAYOUT%.before-e2e" > nul
copy /y tools\e2e_layout.json "%LAYOUT%" > nul
start "" "C:\Program Files\Google\Chrome\Application\chrome.exe" --new-window file:///%CD:\=/%/spikes/s3_input/test_page.html
rem (timeout won't wait without a console, so ping does the waiting)
ping -n 4 127.0.0.1 > nul
start "" build\Release\FullImmersion.exe
ping -n 5 127.0.0.1 > nul
build\Release\e2e_m1.exe > out\e2e.log 2>&1
echo DONE %ERRORLEVEL% >> out\e2e.log
taskkill /f /im FullImmersion.exe > nul 2>&1
if exist "%LAYOUT%.before-e2e" (move /y "%LAYOUT%.before-e2e" "%LAYOUT%" > nul) else (del "%LAYOUT%")

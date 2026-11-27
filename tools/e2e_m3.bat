@echo off
rem M3 end to end: an agent arranges a workspace through the door (no mouse or keyboard is touched).
cd /d "%~dp0\.."
if not exist out mkdir out
set CMAKE="E:\VS\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
%CMAKE% --build --preset release -- /m /v:m > out\e2e_build.log 2>&1
if errorlevel 1 (echo BUILD FAILED > out\e2e_m3.log & exit /b 1)
taskkill /f /im FullImmersion.exe > nul 2>&1
set LAYOUT=%APPDATA%\FullImmersion\layout.json
if not exist "%APPDATA%\FullImmersion" mkdir "%APPDATA%\FullImmersion"
if exist "%LAYOUT%" copy /y "%LAYOUT%" "%LAYOUT%.before-e2e" > nul
copy /y tools\e2e_layout.json "%LAYOUT%" > nul
tasklist /fi "windowtitle eq FullImmersion input test*" | find "chrome" > nul || start "" "C:\Program Files\Google\Chrome\Application\chrome.exe" --new-window file:///%CD:\=/%/spikes/s3_input/test_page.html
ping -n 4 127.0.0.1 > nul
start "" build\Release\FullImmersion.exe
ping -n 5 127.0.0.1 > nul
powershell -NoProfile -ExecutionPolicy Bypass -File tools\e2e_m3.ps1 > out\e2e_m3.log 2>&1
set E1=%ERRORLEVEL%
node tools\test-fi-mcp.js >> out\e2e_m3.log 2>&1
set E2=%ERRORLEVEL%
echo DONE %E1% %E2% >> out\e2e_m3.log
build\Release\s1_capture.exe FullImmersion.exe 1 > nul 2>&1
copy /y out\s1_last.bmp out\e2e_m3_room.bmp > nul
taskkill /im FullImmersion.exe > nul 2>&1
ping -n 3 127.0.0.1 > nul
taskkill /f /im FullImmersion.exe > nul 2>&1
if exist "%LAYOUT%.before-e2e" (move /y "%LAYOUT%.before-e2e" "%LAYOUT%" > nul) else (del "%LAYOUT%")

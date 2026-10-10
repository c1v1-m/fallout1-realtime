@echo off
chcp 65001 >nul
rem Push the game to the Steam Deck and pull its traces back, over SSH (a key after "deck key").
rem
rem   deck            send the new fallout-ce.exe (what changes with almost every build)
rem   deck all        send the exe AND the content under data\ -- never the saves (data\SAVEGAME,
rem                   *.SAV), never characters (*.GCD), never fallout.cfg / f1_res.ini: every
rem                   device keeps its own settings and its own game
rem   deck traces     fetch the Deck's three newest trace logs into build\Release\deck_traces
rem   deck key        ONCE: make an SSH key for the Deck and install it there. He types the Deck
rem                   password that one time; every run after that asks for nothing.
rem
rem Double-click runs "deck". The Deck needs sshd on (sudo systemctl enable --now sshd) and the
rem game folder below. The archive goes through cmd's pipe on purpose: PowerShell would mangle
rem the binary stream.
rem
rem FEEDBACK (2026-10-10, "ať mám nějakou zpětnou vazbu"): after a push the exe on the Deck is
rem checked against this PC's build by SHA-256 and the result is printed in colour -- green OK with
rem the build time, or red CHYBA with the reason. A Deck that cannot be reached says so at once.
setlocal
set DECK=deck@192.168.0.8
set REMOTE=/home/deck/Downloads/fallout1-ce/build/Release
set LOCAL=%~dp0build\Release
set SSHOPT=-o StrictHostKeyChecking=accept-new -o ConnectTimeout=10
rem The Deck key, once he has made it (no password prompt at all from then on).
if exist "%USERPROFILE%\.ssh\id_ed25519_deck" set SSHOPT=%SSHOPT% -i "%USERPROFILE%\.ssh\id_ed25519_deck"

rem Colours: cmd understands ANSI escapes on Windows 10 and later.
for /f %%a in ('echo prompt $E^| cmd') do set "ESC=%%a"
set "GREEN=%ESC%[92m"
set "RED=%ESC%[91m"
set "YELLOW=%ESC%[93m"
set "RESET=%ESC%[0m"

if /i "%~1"=="key" goto key
if /i "%~1"=="traces" goto traces
if /i "%~1"=="all" goto all

echo Posílám fallout-ce.exe na Deck ...
tar -cf - -C "%LOCAL%" fallout-ce.exe | ssh %SSHOPT% %DECK% "tar -xf - -C %REMOTE%"
if errorlevel 1 goto failpush
call :verify
goto end

:all
echo Posílám fallout-ce.exe a data\ na Deck (bez savů, postav a nastavení) ...
tar -cf - -C "%LOCAL%" --exclude "data/SAVEGAME" --exclude "*.SAV" --exclude "*.sav" --exclude "*.GCD" --exclude "*.gcd" fallout-ce.exe data | ssh %SSHOPT% %DECK% "tar -xf - -C %REMOTE%"
if errorlevel 1 goto failpush
call :verify
echo   Data se nahrála spolu s exe (kontroluje se jen exe).
goto end

:traces
if not exist "%LOCAL%\deck_traces" mkdir "%LOCAL%\deck_traces"
rem Reach the Deck FIRST: in the pull below the exit code is the LOCAL tar's, which succeeds on an
rem empty stream -- so an unreachable Deck used to print a green OK over the old files.
ssh %SSHOPT% %DECK% "true" 2>nul
if errorlevel 1 goto failpush
ssh %SSHOPT% %DECK% "cd %REMOTE% && ls -t trace_*.log | head -3 | tar -cf - -T -" | tar -xf - -C "%LOCAL%\deck_traces"
if errorlevel 1 goto failpush
echo.
echo %GREEN%  OK - tři nejnovější trace z Decku jsou v build\Release\deck_traces, nejnovější:%RESET%
for /f "delims=" %%f in ('dir /b /o-d "%LOCAL%\deck_traces\trace_*.log" 2^>nul') do (
    echo      %%f
    goto tracesdone
)
:tracesdone
goto end

:key
if not exist "%USERPROFILE%\.ssh" mkdir "%USERPROFILE%\.ssh"
if not exist "%USERPROFILE%\.ssh\id_ed25519_deck" ssh-keygen -t ed25519 -N "" -C "fallout1-ce deck deploy" -f "%USERPROFILE%\.ssh\id_ed25519_deck"
echo.
echo Installing the key on the Deck -- type the Deck password one last time:
type "%USERPROFILE%\.ssh\id_ed25519_deck.pub" | ssh -o StrictHostKeyChecking=accept-new %DECK% "mkdir -p ~/.ssh && chmod 700 ~/.ssh && cat >> ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys && echo Key installed: no more passwords."
goto end

rem --- Compare the exe on the Deck with this PC's build ---------------------------------------
:verify
set "LHASH="
set "RHASH="
for /f "delims=" %%h in ('certutil -hashfile "%LOCAL%\fallout-ce.exe" SHA256 ^| findstr /v /c:":"') do set "LHASH=%%h"
if defined LHASH set "LHASH=%LHASH: =%"
rem Through a file, not inside for /f: there cmd splits the "=" in the -o options and ssh fails.
ssh %SSHOPT% %DECK% "sha256sum %REMOTE%/fallout-ce.exe" > "%TEMP%\deck_hash.txt" 2>nul
for /f "usebackq tokens=1" %%h in ("%TEMP%\deck_hash.txt") do set "RHASH=%%h"
del "%TEMP%\deck_hash.txt" 2>nul
for %%f in ("%LOCAL%\fallout-ce.exe") do set "BUILT=%%~tf"
echo.
if not defined RHASH goto verifynoremote
if /i not "%LHASH%"=="%RHASH%" goto verifybad
echo %GREEN%  ============================================================%RESET%
echo %GREEN%    OK - Deck má nový exe, kontrolní součet sedí.%RESET%
echo %GREEN%    build z: %BUILT%     nahráno: %date% %time:~0,8%%RESET%
echo %GREEN%    SHA-256: %RHASH:~0,16%...%RESET%
echo %GREEN%  ============================================================%RESET%
exit /b 0
:verifybad
echo %RED%  ============================================================%RESET%
echo %RED%    CHYBA - exe na Decku není ten, co je tady.%RESET%
echo %RED%    tady: %LHASH:~0,16%...   Deck: %RHASH:~0,16%...%RESET%
echo %RED%    Spusť deck.bat znovu; když to zůstane, je hra na Decku zapnutá?%RESET%
echo %RED%  ============================================================%RESET%
exit /b 1
:verifynoremote
echo %YELLOW%  Nahráno, ale kontrolu se nepodařilo udělat - Deck neodpověděl.%RESET%
exit /b 1

:failpush
echo.
echo %RED%  ============================================================%RESET%
echo %RED%    CHYBA - na Deck se nepodařilo připojit.%RESET%
echo %RED%    Je zapnutý, na stejné Wi-Fi a má adresu 192.168.0.8?%RESET%
echo %RED%  ============================================================%RESET%
goto end

:end
endlocal
pause

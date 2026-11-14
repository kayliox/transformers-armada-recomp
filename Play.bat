@echo off
rem Plays the game. In the game: F1 = the menu (settings, saves), F11 = full screen.
rem First start: settings.ini is made from settings.example.ini (overlay\settings.example.ini when there is one).
cd /d "%~dp0"
rem (the new folder layout, when the folder above is marked for it: settings.ini is the one up there)
if exist "..\layout.json" if exist "..\settings.ini" goto play
rem (in the mods' repository this folder is its port\: the install folder is one more up)
for %%I in ("%~dp0.") do if /i "%%~nxI"=="port" if exist "..\..\layout.json" if exist "..\..\settings.ini" goto play
if not exist settings.ini if exist overlay\settings.example.ini copy /y overlay\settings.example.ini settings.ini >nul
if not exist settings.ini if exist settings.example.ini copy /y settings.example.ini settings.ini >nul
:play
rem (TODO 371) Windows 11's Smart App Control can refuse the game, which is built on this PC and unsigned: one line when
rem it is on, and after a start it refused, what to do. The game runs in this window (it is a console program).
set "sac="
for /f "tokens=3" %%V in ('reg query "HKLM\SYSTEM\CurrentControlSet\Control\CI\Policy" /v VerifiedAndReputablePolicyState 2^>nul ^| find "REG_DWORD"') do set "sac=%%V"
if /i "%sac%"=="0x1" echo Note: Windows' Smart App Control is on. If it blocks the game, this window says what to do.
"out\build\transformers\RelWithDebInfo\TransformersRecomp.exe"
set "code=%errorlevel%"
if "%code%"=="-1058471934" goto blocked
if "%code%"=="-1073740760" goto blocked
if "%code%"=="-1073741515" goto missing
if "%code%"=="-1073741521" goto missing
if "%code%"=="-1073741502" goto missing
exit /b %code%
:missing
echo.
echo The game could not start: Windows did not load one of its files (exit code %code%).
echo If Windows Security's Protection history lists one of the game's files, restore it. Running the setup again puts missing files back.
if /i not "%sac%"=="0x1" goto end
:blocked
echo.
echo Windows' Smart App Control blocked the game, or a file it loads, from starting. It lets only programs Microsoft
echo already knows run, and this game is built on your PC, so its files are new to it. Nothing is damaged.
echo To see which files it blocked: Event Viewer ^> Applications and Services Logs ^> Microsoft ^> Windows ^> CodeIntegrity ^> Operational, event 3077.
echo To play, turn it off: Windows Security ^> App ^& browser control ^> Smart App Control settings ^> Off.
echo On some Windows 11 versions it cannot be turned back on without resetting Windows.
:end
pause
exit /b %code%

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
start "" "out\build\transformers\RelWithDebInfo\TransformersRecomp.exe"

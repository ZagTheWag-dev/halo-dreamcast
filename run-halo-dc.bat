@echo off
rem Starts the Dreamcast Halo disc in the Flycast emulator. The game's log
rem shows in Flycast's "Flycast Serial Output" window.
start "" "C:\Tools\flycast\flycast.exe" "%~dp0engine\build\dc\disc\halo.cdi"

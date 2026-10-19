@echo off
rem Builds the Dreamcast Halo campaign disc: engine\build\dc-campaign\disc\halo.cdi
rem (a30, "Halo", played as the N64 port's campaign cartridge plays it: the
rem level's squads, simplified AI, checkpoints; no scripts or cutscenes).
rem Run build-halo-dc.bat first: the campaign disc takes Halo's menus, the
rem title music and the intro from its file tree (engine\build\dc\fs).
setlocal
rem Your own Halo: Combat Evolved Xbox disc image (.iso / .xiso), the first argument:
rem   build-halo-dc-campaign.bat "D:\Games\Halo.iso"
set "XBOX_ISO=%~f1"
if "%~1"=="" (
  echo Usage: build-halo-dc-campaign.bat "path-to-your-Halo-Xbox-disc-image.iso"
  pause
  exit /b 1
)
set "INVADER_DEFINITIONS=%~dp0engine\dependencies\invader\src\tag\hek\definition"
cd /d "%~dp0engine"

echo [1/4] Compiling the campaign's game...
python tools\dc_build.py --campaign || goto :fail
ninja -f build\dc-campaign\build.ninja || goto :fail

echo [2/4] Taking a30 off the Xbox disc image...
if not exist build\xbox\dc-campaign\a30.map (
  python -B -c "import sys; from pathlib import Path; from tools.n64.xiso import XboxDisc; Path('build/xbox/dc-campaign').mkdir(parents=True, exist_ok=True); d = XboxDisc(sys.argv[1]); d.extract('maps/a30.map', 'build/xbox/dc-campaign/a30.map'); d.close()" "%XBOX_ISO%" || goto :fail
)

echo [3/4] Converting a30 for the Dreamcast...
python -B -m tools.n64.convert build\xbox\dc-campaign\a30.map -o build\dc-campaign\fs\maps\a30.map --dc --campaign --filesystem build\dc-campaign\fs || goto :fail
echo       and Halo's menus, the title music and the intro (from build-halo-dc.bat's)...
for %%d in (maps hud snd levels music intro) do if not exist build\dc-campaign\fs\%%d mkdir build\dc-campaign\fs\%%d
copy /y build\dc\fs\maps\ui.map build\dc-campaign\fs\maps\ > nul || goto :fail
copy /y build\dc\fs\hud\ui.dch build\dc-campaign\fs\hud\ > nul || goto :fail
copy /y build\dc\fs\snd\ui.dcs build\dc-campaign\fs\snd\ > nul || goto :fail
copy /y build\dc\fs\levels\ui.dc? build\dc-campaign\fs\levels\ > nul || goto :fail
copy /y build\dc\fs\music\title.dca build\dc-campaign\fs\music\ > nul || goto :fail
copy /y build\dc\fs\intro\intro.dc? build\dc-campaign\fs\intro\ > nul || goto :fail

echo [4/4] Making the disc image...
C:\DreamSDK\usr\bin\bash.exe "%~dp0engine\tools\dc_disc.sh" dc-campaign || goto :fail

echo.
echo Built engine\build\dc-campaign\disc\halo.cdi - the campaign disc.
pause
exit /b 0
:fail
echo BUILD FAILED
pause
exit /b 1

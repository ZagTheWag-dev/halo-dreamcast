@echo off
rem Builds the Dreamcast Halo disc: engine\build\dc\disc\halo.cdi
rem (the program, plus Blood Gulch, the menu's pictures and title music and
rem the boot intro, converted from your own Xbox disc image; ffmpeg reads the
rem intro: C:\Tools\ffmpeg)
setlocal
rem Your own Halo: Combat Evolved Xbox disc image (.iso / .xiso), the first argument:
rem   build-halo-dc.bat "D:\Games\Halo.iso"
set "XBOX_ISO=%~f1"
if "%~1"=="" (
  echo Usage: build-halo-dc.bat "path-to-your-Halo-Xbox-disc-image.iso"
  pause
  exit /b 1
)
set "INVADER_DEFINITIONS=%~dp0engine\dependencies\invader\src\tag\hek\definition"
cd /d "%~dp0engine"

echo [1/5] Compiling the game...
python tools\dc_build.py || goto :fail
ninja -f build\dc\build.ninja || goto :fail

echo [2/5] Taking Blood Gulch and the menu's map off the Xbox disc image...
if not exist build\xbox\dc-input\bloodgulch.map (
  python -B -c "import sys; from pathlib import Path; from tools.n64.xiso import XboxDisc; Path('build/xbox/dc-input').mkdir(parents=True, exist_ok=True); d = XboxDisc(sys.argv[1]); d.extract('maps/bloodgulch.map', 'build/xbox/dc-input/bloodgulch.map'); d.close()" "%XBOX_ISO%" || goto :fail
)
rem (ui.map in a folder of its own: the converter reads what is beside a map)
if not exist build\xbox\dc-ui\ui.map (
  python -B -c "import sys; from pathlib import Path; from tools.n64.xiso import XboxDisc; Path('build/xbox/dc-ui').mkdir(parents=True, exist_ok=True); d = XboxDisc(sys.argv[1]); d.extract('maps/ui.map', 'build/xbox/dc-ui/ui.map'); d.close()" "%XBOX_ISO%" || goto :fail
)

rem (the boot intro: Bink video, read by ffmpeg)
if not exist build\xbox\bink\intro.bik (
  python -B -c "import sys; from pathlib import Path; from tools.n64.xiso import XboxDisc; Path('build/xbox/bink').mkdir(parents=True, exist_ok=True); d = XboxDisc(sys.argv[1]); d.extract('bink/intro.bik', 'build/xbox/bink/intro.bik'); d.close()" "%XBOX_ISO%" || goto :fail
)

echo [3/5] Converting Blood Gulch for the Dreamcast...
python -B -m tools.n64.convert build\xbox\dc-input\bloodgulch.map -o build\dc\fs\maps\bloodgulch.map --dc --filesystem build\dc\fs || goto :fail
echo       and Halo's menus: their widgets, pictures, fonts and sounds...
python -B -m tools.n64.convert build\xbox\dc-ui\ui.map -o build\dc\fs\maps\ui.map --dc --ui --filesystem build\dc\fs || goto :fail
echo       and the title music...
python -B -m tools.n64.menu_dc build\xbox\dc-ui\ui.map --filesystem build\dc\fs --music-only || goto :fail
echo       and the ring and space behind the menu...
python -B -m tools.n64.ui_dc build\xbox\dc-ui\ui.map --filesystem build\dc\fs || goto :fail

echo       and the boot intro...
python -B -m tools.n64.intro_dc build\xbox\bink\intro.bik --filesystem build\dc\fs || goto :fail

echo [4/5] Checking the converted map...
python tools\dc_check_map.py build\dc\fs\maps\bloodgulch.map || goto :fail

echo [5/5] Making the disc image...
C:\DreamSDK\usr\bin\bash.exe "%~dp0engine\tools\dc_disc.sh" || goto :fail

echo.
echo Built engine\build\dc\disc\halo.cdi - double-click run-halo-dc.bat to play it.
pause
exit /b 0
:fail
echo BUILD FAILED
pause
exit /b 1

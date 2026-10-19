# Halo: Combat Evolved on the Sega Dreamcast

Halo: Combat Evolved's own game code running on a Sega Dreamcast: Halo's real Xbox menus, Blood Gulch multiplayer with bots, and the campaign level "Halo" (a30).
It runs at a steady 30 fps.

![Halo's main menu on the Dreamcast](screenshots/main-menu.jpg)

| | |
|---|---|
| ![Blood Gulch](screenshots/blood-gulch.jpg) | ![A kill on Blood Gulch](screenshots/blood-gulch-kill.jpg) |
| Blood Gulch: Halo's sky, haze and terrain detail | Halo's HUD, kill messages, bots |
| ![The assault rifle](screenshots/assault-rifle.jpg) | ![The red base](screenshots/blood-gulch-base.jpg) |
| The assault rifle's ammo counter on the gun | Slayer against bots |
| ![The campaign level "Halo"](screenshots/campaign-halo.jpg) | ![The river in "Halo"](screenshots/campaign-river.jpg) |
| The campaign: "Halo" (a30) | Its river and waterfall |

Screenshots taken in the Flycast emulator.

> **You need your own copy of the Xbox game.** Nothing from the retail game is in this repository.
> The build converts the maps from your own disc image.
> A disc image you build contains converted game data: keep it to yourself.

## About this project

This is early development.
Everything here was built in three days: development started on October 17, 2026, and this first public release went up on October 19, 2026.

- **Day 1:**
  - the Dreamcast toolchain
  - Halo's game code compiling and booting on KallistiOS
  - Blood Gulch loading from a disc
  - then the renderer, controls, HUD, 30 fps pacing, effects, sound and a title menu
- **Day 2:**
  - Halo's boot intro and Halo's real Xbox menus
  - the first runs on a real Dreamcast, and the fixes they called for
- **Day 3:**
  - the weapons' displays (the assault rifle's counter, the plasma gauges)
  - Halo's sky and haze, terrain and cliff detail, water, glowing lights
  - the campaign level "Halo"

I meant to keep working on it privately for longer.
Then I saw a gameplay video of another attempt to bring Halo to the Dreamcast.
That project is closed source, and I think this one is already further along.
So I decided to release mine early and in the open, so anyone can build it, learn from it and improve it.

# *Vibe code is shared code! Leave your sources open!*

### On real hardware

This isn't just an emulator demo: it was run on a real Sega Dreamcast from a **GDEMU** (an SD card in place of the disc drive).
To play the disc you build on yours:

1. Copy `halo.cdi` to the GDEMU's SD card, in a numbered folder of its own (for example `42\disc.cdi`).
2. Add it to your menu (GDMENU).
3. Boot it like any other game.

Real hardware is stricter than an emulator.
The first runs on the console found a frozen picture that Flycast never showed: the PowerVR's vertex buffer was overflowing. That was fixed the same day.
The Dreamcast's processor also faults on unaligned memory access, which the port now emulates the way the Xbox's x86 would have behaved.
The builds since then are developed in Flycast first, and still need fresh testing on a console.

## What works

- **Halo's own menus**, the Xbox's shell running on the Dreamcast:
  - the boot intro, the main menu over the ring
  - profiles with the virtual keyboard
  - the game type editor
  - split-screen join, map and game type select, the pregame lobby
- **Blood Gulch multiplayer:**
  - up to four players on the Dreamcast's pads, plus bots
  - Slayer, CTF and Halo's other game types
  - vehicles, every weapon
  - Halo's HUD and sounds on the Dreamcast's sound chip
  - the pause menu with scores
- **The campaign level "Halo" (a30)**, started from Halo's CAMPAIGN menu with a difficulty choice:
  - its squads placed as you go, simplified Covenant and Marine AI, checkpoints
  - not yet: Halo's scripts, cutscenes and the level's second half
- **The look:**
  - Halo's lightmaps and its sky model with haze
  - terrain and cliff detail maps
  - weapon displays: the assault rifle's counter and compass, the plasma weapons' heat gauges
  - self-illuminated lights, water
  - soft shadows, particles and explosions
- **30 fps**, paced by the display. Draw and game time are measured on screen and over the serial port.

Developed in Flycast and run on a real Dreamcast from a GDEMU (see "On real hardware" above).

## Building

On Windows:

1. **DreamSDK R4** at `C:\DreamSDK`, with KallistiOS 2.2.2 built in `C:\DreamSDK\opt\toolchains\dc\kos`.
   Build KallistiOS's VQ texture encoder once:
   `C:\DreamSDK\usr\bin\bash.exe engine\tools\dc_build_pvrtex.sh`
2. **Python 3** with Pillow, and **ninja** on the PATH.
3. **Invader's tag definitions** (read at conversion time):
   `git clone --depth 1 https://github.com/SnowyMouse/invader engine\dependencies\invader`
4. **ffmpeg** (for the boot intro) at `C:\Tools\ffmpeg\<version>\bin\ffmpeg.exe`, or set `FFMPEG`.
5. Build with your own Halo: Combat Evolved Xbox disc image:

       build-halo-dc.bat "D:\Games\Halo.iso"
       build-halo-dc-campaign.bat "D:\Games\Halo.iso"

   The first makes `engine\build\dc\disc\halo.cdi` (multiplayer).
   The second makes `engine\build\dc-campaign\disc\halo.cdi` (the campaign; build the multiplayer disc first).
6. Play it in Flycast (`run-halo-dc.bat` expects `C:\Tools\flycast`), or put the `.cdi` on a GDEMU or other optical drive emulator's SD card.

The Dreamcast port's own notes are `engine\port\dc\README.md`.

## How it works

- The game code is the decompiled source of Halo: Combat Evolved (the Xbox build 2342), by way of the [Nintendo 64 port "halo-64"](https://github.com/JackTulli/halo-64).
  That port cut the game down to fit a small console and defined a platform interface, `n64.h`.
- This project implements that interface for the Dreamcast on KallistiOS (`engine/port/dc`):
  - a PowerVR renderer for the level, models, sky, HUD, particles and shadows
  - the AICA for sound
  - the Dreamcast pads, the disc and frame pacing
- Halo's own Xbox menus run on top, through a local stand-in for the Xbox's network lobby.
- The maps are converted from the Xbox's cache files by Python tools (`engine/tools/n64`, with `--dc`).

## Credits

- The Halo decompilation projects this code descends from: punpckhdq/halo, bnunu/halo-1, OpenCE, and halo-ce-universal.
- [halo-64](https://github.com/JackTulli/halo-64), the Nintendo 64 port this is built on (CC0).
- [KallistiOS](https://github.com/KallistiOS/KallistiOS) and [DreamSDK](https://dreamsdk.org).
- [Invader](https://github.com/SnowyMouse/invader)'s tag definitions (GPL-3; read at build time, not included).
- [Flycast](https://github.com/flyinghead/flycast), used for testing.

## Legal

This is a fan project for preservation and education. It isn't affiliated with or endorsed by Microsoft, Bungie, Halo Studios or Sega.
Halo is a trademark of Microsoft.
No game assets are included or distributed: you need your own copy of the game.
The port's own code follows the [CC0 dedication](engine/LICENSE.md) of the project it builds on.

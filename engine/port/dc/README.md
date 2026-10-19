# Dreamcast port

The N64 port's game side (`port/n64/game`, the `HALO_N64` trims in `source/`,
`port/n64/src/xdk_*.c`) on a KallistiOS platform layer (`port/dc/src`).
`HALO_N64` stays defined; `HALO_DC` marks where the Dreamcast differs.

## Build

Needs DreamSDK R4 at `C:\DreamSDK` with KallistiOS v2.2.2 built in
`C:\DreamSDK\opt\toolchains\dc\kos`, Python (with Pillow), ninja, and the
Invader tag definitions in `dependencies/invader`.
The models' VQ textures need KallistiOS's encoder built once:
`C:/DreamSDK/usr/bin/bash.exe <repo>/engine/tools/dc_build_pvrtex.sh`.

`build-halo-dc.bat` (the repo's root) does all of it:

    python tools/dc_build.py && ninja -f build/dc/build.ninja    # build/dc/halo.elf
    python -m tools.n64.convert build/xbox/dc-input/bloodgulch.map         -o build/dc/fs/maps/bloodgulch.map --dc                   # INVADER_DEFINITIONS set
    python -m tools.n64.convert build/xbox/dc-ui/ui.map -o build/dc/fs/maps/ui.map --dc --ui --filesystem build/dc/fs   # Halo's menus
    python -m tools.n64.menu_dc build/xbox/dc-ui/ui.map --filesystem build/dc/fs --music-only   # the title music
    python tools/dc_check_map.py build/dc/fs/maps/bloodgulch.map
    C:/DreamSDK/usr/bin/bash.exe <repo>/engine/tools/dc_disc.sh  # build/dc/disc/halo.cdi

Convert from a folder holding the map alone: with `a30.map` beside it the
converter merges Firefight's Covenant (5.9 MB instead of 3.97 MB).

Run `build/dc/disc/halo.cdi` in Flycast (`run-halo-dc.bat`). The game's log
is in Flycast's "Flycast Serial Output" window (`emu.cfg`:
`Debug.SerialConsoleEnabled = yes`) as well as on the screen.

## Maps

`tools/n64/convert.py --dc`: the N64's H64M image, little-endian
(`repack.ORDER`), with the animations in the image where the Xbox reads
them (no `anim/` streams). With `--filesystem` it also writes the level,
the models, the HUD, the effects and the sounds (below; without it the
sound tags are left out, as the N64 does without a file tree); the HUD's sprite
and glyph numbers + 1 go into the map's BitmapData 'pixel data offset' and
FontCharacter 'pixels offset' (little-endian; 0 for the rest, and without
`--filesystem`). `tools/dc_check_map.py` checks the header, the 'tags'/'sbsp' signatures and
every relocation (the SH-4 traps on an unaligned access).

## Platform layer

- `dc_main.c`: start-up (the game on its own 256 KB stack, the scripted test
  controllers on), the log (framebuffer console + serial), time, the heap.
  The serial log is queued and written out by a thread of its own only while
  the game sleeps (a few characters at a time: KallistiOS's 10 ms tick runs
  any ready thread, whatever its priority), so lines no longer stall frames
  (the statistics every 5 s took ~100 ms). `n64_wait_microseconds` (Halo's
  `Sleep`) is precise: thd_sleep for whole 10 ms ticks, the rest spun.
- `dc_pace.c`: the frame pacing (below).
- `dc_files.c`: `n64_file_*` over `/cd` (whole-sector DMA, a 64 KB bounce
  buffer otherwise); the testing settings, compiled in (none as shipped:
  the game boots to the menu; `debug_map` etc. go straight into a game).
- `dc_menu.c`: the map and game-type tables, and the menus' loop (below;
  `port/dc/game/dc_shell.c` is Halo's menus' side).
- `dc_platform.c`: answers for the placeholders the game reads back.
- `dc_fx.c`: the particles, explosions and shot trails (below).
- `dc_sound.c`: the game's sounds on the AICA (below).
- `dc_globals.c`: the N64 layer's variables the game reads.
- `dc_stubs_generated.c`: weak do-nothing versions of the rest of n64.h.
  After a link fails on undefined `n64_*` names, run
  `python tools/dc_stubs.py <the link's log>` and build again.

## Renderer

- `tools/n64/level_dc.py` (`convert.py --dc --filesystem build/dc/fs`):
  `levels/<map>.dcl`, the N64's level geometry, light and PVS groups as
  float strip runs, with PowerVR textures (twiddled 16-bit mipmap chains;
  natural terrain tinted by its detail maps).
- `dc_render.c`: PowerVR start-up, the frame, each view's camera (one ftrv
  and divide to screen x, y, 1/w), frustum tests, near-plane clipping,
  KallistiOS's linear table fog. `dc_level.c`: the level, by PVS group and
  run, a header per material.
- `dc_model.c`: the objects' models and the first-person weapon. Each
  vertex of a batch is transformed once (by its node's matrix), divided
  and tinted into a scratch of tile-accelerator vertices; a strip index is
  a 32-byte copy of one. Only a strip that crosses the near plane goes
  through the clip (`dc_clip_strip`, which sends a crossing strip's runs
  of triangles wholly in front as strips and clips only the crossing
  ones): before, an object near the eye and the first-person weapon went
  through it whole, a divide a strip index, the crossing strips triangle
  by triangle.
- `dc_shadow.c`: the units' and vehicles' shadows (below).
- Testing setting `debug_no_fog` (dc_files.c `dc_settings`).

## Frame pacing

The game paces itself as the Xbox did: `rasterizer_globals.framerate_throttle`
on (`port/n64/game/n64_render_game.c`, HALO_DC), the game's count of display
refreshes moved on by a KallistiOS vertical blank handler (`dc_pace.c`), and
`main.c`'s `main_update_time` / `main_rasterizer_throttle` give each frame the
refresh two after the last one's (30 fps), one tick of game time, and hold it
until the refresh before (asleep on a genwait the interrupt wakes, the game
switched to at once). The frame is then sent, the PowerVR renders it and
KallistiOS flips to it on the next refresh: every frame on the screen for
exactly two refreshes. A refresh counts as 1/60 s of game time (1/50 on PAL),
so 150 ticks take 5005 ms on NTSC's 59.94 Hz (5016 in Flycast's ~59.8).

Before, Sleep (to 10 ms) and pvr_wait_ready beat against each other: 20 and
30 fps by turns; even a precise Sleep gives a few frames in 120 shown for one
or three refreshes. Testing setting `dc_sleep_pacing`: that way again.

The `dcf:` line every 120 frames: frames a second measured, how many
refreshes each frame stayed on the screen (1, 2, 3, 4, 5+: all should be 2),
and where a frame's time went, average/worst in us: `game` (input, the ticks:
frame end to the next start), `draw` (level, objects, HUD on the CPU; the
opaque list straight to the TA), `send` (the PT/TR lists and HUD), `ready`
(pvr_wait_ready), `idle` (the wait for the refresh) and `render` (the
PowerVR's, as KallistiOS times it). Blood Gulch with three bots, autotest, in
Flycast: 29.89 fps (Flycast's refresh / 2), 2:120 every window, game ~2.2 ms,
draw 3-7 ms, send < 0.2 ms, idle 24-28 ms of the 33. `dcr:` lines: the level
and objects (fps measured over the 120 frames too). The game's `tick:` line
every 150 ticks and the `player N:` lines stay; the tick's breakdowns
(phases, object types, stages: five long lines every 5 s down the serial
port) only with testing setting `debug_tick_phases`.

60 fps with interpolated frames is not done: the native ports'
render_interpolation.c is not in this tree, and it would need every object's
node matrices kept twice.

## Shadows

Halo's dynamic object shadows (the Xbox's, from each object's lighting; the
N64 had none) as a soft dark oval on the ground under each unit and
vehicle. The game side (`port/n64/game/n64_render_game.c`, HALO_DC
`frame_object_shadow`) gives each unit or vehicle on its own (a seated
unit is its vehicle's) that was drawn this frame within 45 units, and the
viewer's own unit (seen looking down), a ray straight down from its
bounding sphere's centre through the structure and the vehicles (not
itself), once a frame (`collision_test_vector`); a vehicle two more, at
its front and back, for the slope along it. n64.h's
`n64_render_shadow` gets the ground's point and plane, the object's
forward, the oval's half-length and half-width (the bounding sphere's
size: round for a Spartan, longer than wide for a vehicle, along its
forward) and its darkness (0.6 for a unit, 0.75 for a vehicle; less as it
rises off the ground, gone 2.5 units up, a jump's shadow spreading and
fading; a camouflaged unit's with its camouflage).

`dc_shadow.c` lays it on that plane, 0.04 units off it: two triangles on
the translucent list, a 64x64 radial falloff (dark in its middle third,
smoothstep to nothing) in the HUD's 8-bit coverage palette (bank 0:
entry v is (v, v, v, v)) times a black vertex colour, blended by alpha,
depth-tested (GEQUAL) and never writing depth, so the PowerVR's sort lays
it over the ground and under whoever stands on it. No PowerVR fog (it
would add the fog's colour): it fades by its distance into the fog, as the
particles do. Crossing the near plane (the viewer's own) it is clipped.
4 KB of video memory, made as the PowerVR comes up (`dc_shadow_reserve`,
after the HUD's palettes). Ground that bends under the oval is not
followed (where it rises the oval goes under it); the Warthog's front and
back rays keep its oval along a slope.

Cost in Flycast, a game with seven bots: 1-1.5 rays a frame (16-50 us
with the vehicles' extra rays; before the rays were only for drawn objects,
10 a frame, 170-240 us), 1-20 us a frame to draw. The `dcr: objects` line
has the shadows drawn a frame and their time. Checked with screenshots in
the chase view (testing setting `debug_chase`): a Spartan's under its feet,
lighter and wider in the air while jumping, a bot's, the Warthog's from
its front to its back (seen from `debug_spin`): `docs/shadows-chase-grid.png`,
`docs/shadow-*.png` (the red one: the Warthog's oval tinted red for a test,
to show where it lies).

## Performance

Measured in Flycast with seven bots, autotest and `debug_explosions` (the
`dcf:`/`dcr:` lines over 52 s, averages of the 13 windows; Flycast's
SH-4 timing is approximate, real hardware's cache misses are not in it):

| | before | after |
|---|---|---|
| frames on screen 2 refreshes | 1560 of 1560 | 1560 of 1560 |
| draw (CPU) average / worst | 4.63 / 10.2 ms | 4.46 / 9.1 ms (shadows included) |
| models average / worst window | 2.35 / 4.2 ms | 2.08 / 3.8 ms |
| first-person weapon (1500 triangles) | 1.75 ms | 1.49 ms |
| level | 0.98 ms | 0.93 ms |
| game (ticks) average / worst | 3.0 / 8.3 ms | 2.9 / 8.3 ms |
| PowerVR render average / worst | 7.5 / 10.0 ms | 7.5 / 10.0 ms |
| idle (waiting for the refresh) | 25.7 ms | 26.0 ms |

Where the first-person weapon's time went: about 0.47 ms transforming its
1629 vertices, 0.33 ms sending its in-front batches, 0.73 ms its one batch
that crosses the near plane (the arms reach back past the eye: 44 of its
strips cross). The rest of the frame has room: 26 of each 33 ms are idle,
and the PowerVR renders in 7.5-10 ms, beside the CPU. The `dcr: objects`
line now also gives the first-person weapon's triangles and time.


- `tools/n64/hud_dc.py` (`convert.py --dc --filesystem`): `hud/<map>.dch`,
  the HUD's images (hud.py's set and numbering, 73 on Blood Gulch) at their
  own size, premultiplied, twiddled: 8-bit palettised (a coverage ramp, or
  alpha and intensity in four bits each) or ARGB4444 for a coloured one;
  the meters at half size in RAM (fill order, shape); every font of the map
  in one 4-bit atlas (896 glyphs, 512x512). `tools/dc_disc.sh` copies it.
- `dc_hud.c`: n64.h's HUD. The game's own HUD code (interface/hud*.c through
  `port/n64/game/n64_hud_game.c`) gives its quads, glyphs and blips in its
  640x480 units, one a pixel; they are staged as translucent-list quads and
  sent after the world's translucent polygons, 1/w from 131072 up (past the
  first-person models), a step a quad, so the autosort keeps their order.
  Halo's blend, colour x texture + framebuffer x (1 - alpha): MODULATEALPHA,
  ONE / INVSRCALPHA. Meters are coloured on the CPU (n64_hud.c's rule) into
  12 small textures, loaded between the frame's lists. Text is Halo's own
  layout: the HALO_DC `rasterizer_draw_string` runs draw_string.c with a
  glyph callback (`n64_hud_glyph`, with rasterizer_text.c's drop shadow);
  messages with buttons ("%action") get the Dreamcast's button names
  (`n64_hud_game_icon_string`, from hud_messaging.c), and so do Halo's
  state messages' button icons (hud_messaging.c, HALO_DC: the pad's
  button as text where Halo drew the Xbox's picture), the weapon pick-up
  prompt ("HOLD D-PAD LEFT TO PICK UP") and "Hold START for score"
  (text_group.c's built-in string): `n64_input_button_name` (dc_input.c)
  names an Xbox button as the pad has it, for the player's scheme and the
  fire swap (what xdk_input.c and read_port make of each). The pause screens
  (`n64_hud_pause`, `n64_hud_pause_controls`) are n64_hud.c's, in the HUD's
  font (large_ui).
- Video memory: the HUD takes 571 KB (sprites 443, glyph atlas 128) plus 96
  KB of meter slots; about 1.1 MB stays free after level, models and HUD
  (the `dch:` load line says how much). What is kept for good (the
  palettes, the blip, the meter slots, the effects' puff and ribbon) is
  made as the PowerVR comes up (`dc_hud_reserve`, `n64_particles_reserve`
  from `n64_render_initialize`), at the bottom of video memory, so the
  maps' and the menu's textures come and go above it without a hole (it
  used to be the first map's load, after the menu's level); the same free
  space each time round title, game, title (4726 KB after the menu's
  pictures, 1166 KB in a game). The palettes are ARGB8888 (one format
  for all of them): 8-bit banks 0-1 and 4-bit bank 32 are the HUD's.
- Halo's multitexture overlays (hud_draw.c: the sniper's angle ticks), which
  the N64 drew as their first map alone (the sniper's mask: a white bar
  each side of the screen, zoomed or not), draw their second map
  (`n64_hud_game.c` `dc_multitexture_quad`, HALO_DC): the ticks at their
  scale and offset (scrolled by the aim's pitch, wrapped), faded by the
  maps' fades (the zoom's: none unzoomed); the mask's soft ends are left
  out. The scope's black mask round the zoomed view (the weapon HUD's
  screen effect) is not drawn.
- One view only so far: split screen would need a clip per view.
- Testing settings: `debug_no_hud`, `debug_hud_inset=<pixels>`,
  `dc_pause_test` (port A presses START, A, B, START at 25, 33, 41 and 49 s:
  the pause screens without hands), `dc_no_pause`.

## Effects

- `tools/n64/effects_dc.py` (`convert.py --dc --filesystem`): `fx/<map>.dcf`,
  effects.py's file little-endian ('DCFX'): each effect event's emitters
  (296 effects, 1877 emitters on Blood Gulch), the 128x64 I4 atlas of the
  particles' pictures and their colours, the explosions the N64 draws its
  own way; and the projectiles' looks, which the N64 never drew: Halo shows
  plasma bolts, bullets and the tank's shell by their lights and contrails
  (dropped tags), so each projectile tag gets a glow (its light's colour,
  no model only) and a trail (its contrail's first point state: colour,
  width, added or blended). 10 on Blood Gulch; the sniper rifle and the
  flamethrower have their own. `tools/dc_disc.sh` copies it. 158 KB, read
  whole into RAM.
- `dc_fx.c`: n64_particles.c on the PowerVR. The effect code
  (source/effects/effects.c, HALO_N64) reads the emitters in place
  (`n64_fx_event`) and makes particles (`n64_particle_spawn`, a pool of 256,
  600 a second with bursts of 128: twice the N64's); grenades', rockets',
  the tank's and the needler's explosions are the N64's flash, fireball and
  smoke (`n64_explosion`); the sniper's vapour ribbon and the flame trail
  are the N64's; `n64_fx_projectile` (projectiles.c, HALO_DC, each tick of
  each projectile's flight) adds a trail segment (at most 3 units, 0.11 s)
  and a glow with a white middle (one tick and a half) by the projectile's
  look. Drawn from `n64_render_view_particles` (after the first-person
  weapon) as quads on the translucent list, staged with the world's,
  facing the camera (one wholly outside the view's rectangle, or a trail's
  box, not sent: the view's, not the screen's), 1/w their depth (first-person ones, a muzzle flash on
  your own gun, at the first-person models' 1024 x 1/w); the PowerVR
  sorts them (autosort). Additive particles and glows add (SRCALPHA, ONE),
  the rest blend by alpha; none takes the PowerVR's fog (it would add its
  colour to a glow): each fades with distance into it, as on the N64. A
  fireball's puff mixes two colours by its texel as the N64's combiner did:
  the edge's colour is the vertex's offset colour (oargb, added after the
  texture), the middle's less it the vertex colour. As on the N64 every
  particle is the soft blob of cell 0 in its own colour (testing setting
  `dc_fx_cells`: their own pictures), the flame the flame's cell.
- Video memory: 32 KB for the atlas (white and grey ARGB4444), 8 KB for
  the 64x64 puff, 0.5 KB for the ribbon; about 1.1 MB stays free (the `dcx:`
  load line). No palettes.
- The `dcx:` line every 120 frames: particles live, made and refused since
  the last line, drawn and quads a frame, explosions, trails, glows and
  ribbons live, the drawing's time a frame (20-80 us in fire fights).
- Testing settings: `debug_explosions` (an explosion of each kind in turn
  6 units ahead, a little above the eye), `debug_weapon=<weapon tag>` (the
  first weapon: the plasma rifle for bolts, the rocket launcher...),
  `dc_fx_cells`.
- Not done: decals (bullet holes, scorch marks: Halo's pool is off on the
  N64 too; the N64's own decal path is the paintball modifier's), Halo's
  own contrails (the trails are a tick's segments, not ribbons that
  linger: a rocket's smoke trail is its exhaust effect's particles),
  lights (a bolt lights nothing around it), particle systems (`pctl`: the
  rocket's exhaust system).

## Sound

- `tools/n64/sounds_dc.py` (`convert.py --dc --filesystem`): sounds.py's
  records for the tag image in `repack.ORDER` (what
  `port/n64/game/n64_sound_game.c` reads, as on the N64: the N64's output
  is byte-identical), and `snd/<map>.dcs`, the files they play, in the
  AICA's own 4-bit Yamaha ADPCM: each permutation decoded from Xbox ADPCM,
  made mono, 22050 Hz, encoded following the decoder's state exactly (no
  audioconv64). An AICA channel plays at most 65535 samples (its loop end
  is 16-bit): the 28 longer files of Blood Gulch (to 5.3 s) are resampled
  to a rate that fits (12-21 kHz; the channel plays each at its own). Each
  file sits on a sector of its own (2048 bytes) for the disc's reads, in
  the preload order: every pitch range's first take, then the rest, by
  class (announcer, weapons, detonations, impacts, vehicles, footsteps,
  dialogue...); each has an alternate (the next take of its pitch range).
  858 files on Blood Gulch, 775 s, 8.9 MB. Cached by content in
  `build/dc/sound-cache` (the first conversion takes about 30 s).
  `tools/dc_disc.sh` copies the bank.
- `dc_sound.c`: n64.h's voices (n64_sound.c on the N64) on AICA channels
  0-31 through KallistiOS's own driver (`snd_init`, AICA_CMD_CHAN start,
  stop and update commands as snd_sfx sends them): ADPCM straight from
  sound RAM at the file's rate x the pitch (0.25-2), once or looping, the
  volume KallistiOS's linear 0-255 and the pan cutting the far side up to
  -9 dB; a voice is free, or the cheapest of no higher priority is stolen
  (lower priority, quieter, older: the N64's mixer's rule). Whether a
  voice still plays is timed here (its samples at its rate, the rate's
  changes included, + 30 ms). Paused games' voices play on unheard.
- Sound RAM is 2 MB, 1.8 after the driver, the bank 9 MB: at the map's
  load the bank's first files fill it to 160 KB short (127 files, 1.7 MB,
  1.2 s; a file too big for what is left is skipped, not the end of it); the rest are read from the disc when first asked for by a
  thread of its own (whole sectors, DMA; plain G2 writes into sound RAM,
  not the store queues the PowerVR lists use) into room the least
  recently played files give up. Meanwhile a sound plays as another take
  of the same sound that is in sound RAM, or, with none, waits for the
  disc (up to 300 ms for a sound played once, then dropped; as long as it
  takes for a loop). In Flycast a load takes 10-35 ms; a real drive's
  seeks are slower, so more sounds will play as another take there.
- The `dcs:` lines every 120 frames: voices playing, looping, waiting;
  started, stolen, refused; played as another take, waited for (worst
  wait), dropped; on how many voices the AICA was seen moving (the
  driver's copy of the channel's position, 60 ms into each play); the
  disc's loads (KB, time), files given up, what is in sound RAM.
- Testing settings: `dc_sound_test` (the game's sounds refused: from 3 s
  after the load five files alone at full volume in the middle, 2 s
  apart; the last looping for 6 s, pitch 0.5-2 and pan swept; one read
  from the disc, then again; then the game's), `debug_sound` (each sound
  started logged, the voices every 150 ticks: n64_sound_game.c),
  `debug_no_sound`.
- Checked by listening's stand-in: `tools/dc_flycast_test.ps1 -audio
  <file.raw>` sends Flycast's sound to a file (SDL's "disk" driver, 16-bit
  stereo 44100 Hz; without `-audio` a test run is silent, Flycast's null
  backend) and `tools/dc_audio.py <file.raw> [--png] [--wav]` measures it.
  With `dc_sound_test` the files at their own rate came out of Flycast's
  AICA as the converter decodes them (correlation 1.000, 56-74 dB apart,
  0.0 dB gain at full volume); the loop held for its 6 s at the swept
  pitch and pan and stopped.
- The title music: the menu's (above). No
  environment sounds (ambience, devices, music: left out by sounds.py, as
  on the N64), no reverb.

## Menu

The game boots to Halo's own menus: the Xbox's shell
(`source/interface/ui_widget*.c`, compiled for the Dreamcast, with
`player_ui.c`, `virtual_keyboard.c` and `saved games/player_profile.c`,
`playlist_profile.c`, `saved_game_files.c`) over ui.map's widgets,
pictures, fonts and sounds, run by `dc_menu.c` (n64.h's `n64_menu_run`,
from main.c's `main_menu_load` through n64_game.c) and
`port/dc/game/dc_shell.c`, over the scene behind Halo's main menu (ui.map's
space sky and the ring, Halo's title camera path: `tools/n64/ui_dc.py`).

- The screens are the Xbox's: the main menu (CAMPAIGN, MULTIPLAYER,
  SETTINGS; GAME DEMOS hidden, as on an Xbox without its demos; the build
  number), MULTIPLAYER (COOPERATIVE PLAY, SPLIT SCREEN, SYSTEM LINK PLAY
  dimmed: no network, EDIT GAMETYPES), split screen's press-A-to-join and
  profile panels, SELECT MAP, SELECT GAMETYPE, the pregame lobby (ENLISTED
  PLAYERS, its countdown, X delay, A start sooner, D-pad teams), the
  profiles (create, rename with the virtual keyboard, controls, colour),
  the game types' editor, Halo's error dialogs and button icons.
- What the Dreamcast has not, Halo's menus show and refuse with their error
  sound: maps other than Blood Gulch (the list starts on it), a campaign
  level (its menus browse; `dc_campaign_refused`).
- Split screen is a network game on the Xbox (the box a server and a client
  of itself); `dc_shell.c` answers the shell's networking calls with a local
  lobby (the players who joined, their profiles' names and colours or the
  server's random ones, teams, the map and game variant chosen, the
  countdown as `network_server_manager.c` keeps it: 11 s, 5 s less a player
  joining, 31 s at most). One player may count down: the Dreamcast adds
  bots to make four. When it runs out the game starts: the players on their
  pads, teams and colours, the variant as it is (`n64_game_set_shell_variant`:
  a default one or one edited under EDIT GAMETYPES), the lobby names in the
  game.
- After a game, as on the Xbox: Halo's postgame scores in the game, then the
  map select screen with the same players (`network_game_reset_to_pregame_ui`);
  the pause menu's MAIN MENU goes to the main menu.
- The saved games: the Xbox's writable drives (u:, z:, t:) in RAM
  (`port/dc/xdk/xdk_dc_files.c`, through `xdk_kernel.c`'s files, and the XAPI's
  save games), so Halo's default profiles and game types are made at start
  and what the player makes lasts until the Dreamcast is switched off.
- The pads: while the menus are up (`dc_input_menu`) `xdk_input.c` gives
  Halo the Dreamcast pad's own buttons as the Xbox pad's (A B X Y, START,
  the D-pad, the stick), which Halo's input and event manager turn into the
  widgets' events.
- `tools/n64/convert.py --dc --ui` (ui.map, taken off the Xbox disc into
  `build/xbox/dc-ui`): `maps/ui.map` (the widgets, their collections, the
  strings, fonts, the multiplayer maps' and the virtual keyboard's tags
  kept), `hud/ui.dch` and `snd/ui.dcs` (the clicks). The pictures: 220
  images, 26 MB at 16 bits a texel, 4.1 MB on the disc (`UiHudExporter`: the
  large ones VQ compressed with pvrtex, the shell's gradient a ramp of
  colours drawn as Gouraud strips), streamed (`dc_hud.c`: each into video
  memory when first drawn, the least lately drawn given back when there is
  no room, copied by the CPU since a frame's direct rendering has the store
  queues). The widgets' bitmaps blend straight (`n64_hud_game.c`
  `dc_widget_quad` premultiplies their colour; the shimmer of "nifty render
  fx" is left out).
- Title music (`dc_sound.c` `n64_music_start`, `n64_music_stop`): its start
  (the monks) once, then its three loops in a random order, stereo at 44100
  Hz, streamed from the disc. The file is the AICA's 4-bit ADPCM in blocks
  of 4096 samples a channel; a thread reads it (32 KB at a time, 44 KB a
  second), decodes it on the SH-4 (each version from the decoder's resting
  state, which the AICA's own decoder could not do across versions) into
  two rings of 16-bit samples in sound RAM (8 parts of 4096 samples, 0.74
  s) that channels 62 and 63 loop over, hard left and right, volume 204
  of 255 (the N64's 0.8); every 20 ms it refills the parts played. With no
  map's sounds loaded but the menus' clicks (which the bank keeps off the
  rings). 10 MB on the disc (233 s); `tools/n64/menu_dc.py --music-only`.
- Loading: a LOADING band stays on the screen while the map loads; the
  menus' tags, pictures, sounds and music are given back first.
- Testing settings: `debug_menu=<script>` (a press every 20 frames on port
  A, `u d l r a b x y s` and `.` for none, going on through each return to
  the menus), with `debug_game_ticks=<n>` to end each game early. Checked
  that way in Flycast: the main menu, split screen to a Slayer game on Blood
  Gulch with three bots and back to the map select screen, a profile made
  and named with the virtual keyboard, the game types' editor, the campaign
  refused.

## Status

The game boots to Halo's title over a flight across Blood Gulch, with the
title music; the setup chooses the game type, the score, the bots and their
skill; Blood Gulch loads from the CD and the game runs (one player on the
pads, the bots), then its results and the menu again. The level is drawn
textured from player 1's eye with fog and Halo's baked light, with the
objects (the units and vehicles with soft shadows on the ground), the
first-person weapon and Halo's HUD (shields, health, ammunition and grenade
counters, reticle, motion sensor, messages naming the Dreamcast's buttons,
the respawn countdown, the sniper's zoomed angle ticks); START held opens
the pause menu (scoreboard, controls); the effects' particles (muzzle
flashes, impacts, sparks, smoke), explosions, plasma bolts' glows and
trails, tracers, the sniper's ribbon; the game's sounds (weapons,
footsteps, impacts, explosions, vehicles, the announcer) on the AICA. A
steady 30 fps, paced by the display. Blood Gulch is the one map (the
owner's choice).

Checked at the end in Flycast (`docs/final-*.png`), from a clean
`build/dc` (`build/xbox` kept: `build-halo-dc.bat` end to end in 52 s,
the sound conversion's 30 s included):

- As shipped, two minutes on the title: 29.89 fps with every frame on the
  screen for two refreshes in every window after the first, the music
  `0 late`, no FATAL.
- Five games through the menu (autotest, `debug_game_ticks`,
  `debug_menu`): 9185 KB of heap left at each map load after the first
  through the menu (9290 at the first, which skips the menu) and 9152 KB
  free after each game; video memory the same each time (1162 KB left in
  a game, 2503 KB in the menu). Only the loads' windows are off 2:120
  (one frame of 5+ refreshes each). Halo's own silent
  `memory check failed at scenario_load` line at each second load is
  there from before the menu, with or without sound.

Not done, or not right yet:

- Split screen: no per-view clip (a view's first-person weapon draws over
  the other); Halo's menus let up to four join, tested with one. Autotest alone starts
  the N64's two-player game; with `debug_map`, `debug_players 1`,
  `debug_bots 3` it is the one player and three bots.
- Not drawn: the sky's gradient, decals (bullet holes, scorch marks),
  Halo's lingering contrails and particle systems, the sniper's black
  scope mask; shadows are ovals, not silhouettes, straight down.
- Not tested: real Dreamcast hardware and pads (disc seek times for the
  sounds and the music, the SH-4's and PowerVR's real timing, the VMU,
  the Jump Pack), PAL at 50 Hz, game types other than slayer and CTF,
  seven bots for long.
- Leaving a game: about 4 s of an empty light-blue screen while the menu's
  level and pictures load.
- Controls page: still the N64's five scheme names.

## Boot, menu, real hardware

- Boot intro: `tools/n64/intro_dc.py` (ffmpeg reads the Xbox disc's
  bink/intro.bik) -> intro/intro.dcv (15 VQ pictures a second) and
  intro/intro.dca; `dc_intro.c` plays it before the menu (START/A skips).
- Menus: Halo's own, the Xbox's shell (above) over the ring and space
  (`tools/n64/ui_dc.py` -> levels/ui.dcm, levels/ui.dcs; Halo's title camera
  path).
- `dc_exceptions.c`: unaligned loads and stores emulated. Flycast never
  raises these, a real Dreamcast does; each place is logged once ("dce:").


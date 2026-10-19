#!/usr/bin/env bash
# The Dreamcast disc (build/dc/disc/halo.cdi): build/dc/halo.elf as the
# scrambled 1ST_READ.BIN, the boot sectors (IP.BIN), and the converted files
# (build/dc/fs: maps/<map>.map from tools/n64/convert.py --dc) at the root.
# Run in DreamSDK's shell (it puts the disc tools on PATH):
#
#   C:/DreamSDK/usr/bin/bash.exe <full path of tools/dc_disc.sh>
set -e

# the tools (also when DreamSDK's profile has not set them up)
export PATH="/opt/toolchains/dc/sh-elf/bin:/opt/toolchains/dc/kos/utils/makeip:/mingw64/bin:/usr/bin:$PATH"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# (an argument: the build folder under build/, "dc-campaign" the campaign
# disc: build/dc-campaign/disc/halo.cdi)
BUILD="$ROOT/build/${1:-dc}"
DISC="$BUILD/disc"
SCRAMBLE=/opt/dreamsdk/tools/checker/tools/scramble.exe

rm -rf "$DISC/root"
mkdir -p "$DISC/root"

# the program as a raw binary (linked at 0x8c010000), scrambled: the BIOS
# unscrambles a CD-ROM's boot file
sh-elf-objcopy -R .stack -O binary "$BUILD/halo.elf" "$BUILD/halo.bin"
"$SCRAMBLE" "$BUILD/halo.bin" "$DISC/root/1ST_READ.BIN"

makeip -f -g "HALO COMBAT EVOLVED DC" -c "HALO-DC" -n "T-00000" -e "V1.000" \
	-b 1ST_READ.BIN "$DISC/IP.BIN" > /dev/null

# the game's files: the converted maps, levels, HUDs, effects and sounds (no converter temporaries)
mkdir -p "$DISC/root/maps"
cp "$BUILD"/fs/maps/*.map "$DISC/root/maps/"
if ls "$BUILD"/fs/levels/*.dc[lms] > /dev/null 2>&1; then
	mkdir -p "$DISC/root/levels"
	cp "$BUILD"/fs/levels/*.dc[lms] "$DISC/root/levels/"
	# (a campaign level's encounters: n64_campaign.c)
	cp "$BUILD"/fs/levels/*.enc "$DISC/root/levels/" 2> /dev/null || true
fi
# the HUD's images and fonts (tools/n64/hud_dc.py; ui.dch, the menu's:
# tools/n64/menu_dc.py)
if ls "$BUILD"/fs/hud/*.dch > /dev/null 2>&1; then
	mkdir -p "$DISC/root/hud"
	cp "$BUILD"/fs/hud/*.dch "$DISC/root/hud/"
fi
# the effects' particles and the projectiles' looks (tools/n64/effects_dc.py)
if ls "$BUILD"/fs/fx/*.dcf > /dev/null 2>&1; then
	mkdir -p "$DISC/root/fx"
	cp "$BUILD"/fs/fx/*.dcf "$DISC/root/fx/"
fi
# the sounds, the AICA's ADPCM (tools/n64/sounds_dc.py)
if ls "$BUILD"/fs/snd/*.dcs > /dev/null 2>&1; then
	mkdir -p "$DISC/root/snd"
	cp "$BUILD"/fs/snd/*.dcs "$DISC/root/snd/"
fi
# the title music (tools/n64/menu_dc.py; the menu's pictures and fonts are
# hud/ui.dch, above)
if [ -f "$BUILD/fs/music/title.dca" ]; then
	mkdir -p "$DISC/root/music"
	cp "$BUILD/fs/music/title.dca" "$DISC/root/music/"
fi

# the boot intro (tools/n64/intro_dc.py)
if [ -f "$BUILD/fs/intro/intro.dcv" ]; then
	mkdir -p "$DISC/root/intro"
	cp "$BUILD"/fs/intro/intro.dc[va] "$DISC/root/intro/"
fi

# an audio/data disc (the first session's audio track is what lets the
# image boot as a CD-ROM), Rock Ridge and Joliet names
mkisofs -quiet -C 0,11702 -V HALO_DC -G "$DISC/IP.BIN" -sysid "SEGA SEGAKATANA" \
	-duplicates-once -l -r -J -o "$DISC/halo.iso" "$DISC/root"
rm -f "$DISC/halo.cdi"
cdi4dc "$DISC/halo.iso" "$DISC/halo.cdi" > /dev/null
rm -f "$DISC/halo.iso"
ls -l "$DISC/halo.cdi" "$DISC/root/1ST_READ.BIN" "$DISC/root/maps"

/*
DC_FILES.C

The n64.h files over KallistiOS's file system (port/n64/src/n64_files.c on
the N64): read-only, "rom:/x" read from DC_FILE_ROOT "/x" (the CD's root,
where tools/dc_disc.sh puts the converted maps). And the testing settings
(n64_debug_flag, n64_debug_text): the N64 reads each from a file on the
cartridge; the Dreamcast's are compiled in (dc_settings below), so that an
ELF booted with no disc runs the same test.

Reads go straight to the CD (one DMA of whole sectors) when the position is
on a sector and the buffer is 32-byte aligned; otherwise through a 64 KB
bounce buffer, which keeps them whole sectors too: KallistiOS reads every
other request a sector at a time.
*/

#include <kos.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "n64.h"

#ifndef DC_FILE_ROOT
#define DC_FILE_ROOT "/cd"
#endif

#define MAXIMUM_FILES 8
#define SECTOR_SIZE 2048
#define BOUNCE_SIZE (64 * 1024)

static struct
{
	int open;
	file_t fd;
	long size;
	long position;
} files[MAXIMUM_FILES];

static uint8_t *bounce;
static int bounce_owner = -1;
static long bounce_start, bounce_length;

static int dc_path(const char *path, char *out, size_t size)
{
	if (!strncmp(path, "rom:/", 5))
		path += 5;
	while (*path == '/')
		path++;
	return snprintf(out, size, DC_FILE_ROOT "/%s", path) < (int)size;
}

static int valid(long handle)
{
	return handle >= 0 && handle < MAXIMUM_FILES && files[handle].open;
}

long n64_file_open(const char *path)
{
	char name[256];
	size_t size;
	file_t fd;
	int i;

	if (!dc_path(path, name, sizeof(name)))
		return -1;
	for (i = 0; i < MAXIMUM_FILES && files[i].open; i++)
		;
	if (i == MAXIMUM_FILES)
		return -1;
	fd = fs_open(name, O_RDONLY);
	if (fd == FILEHND_INVALID)
		return -1;
	size = fs_total(fd);
	if (size == (size_t)-1)
	{
		fs_close(fd);
		return -1;
	}
	files[i].open = 1;
	files[i].fd = fd;
	files[i].size = (long)size;
	files[i].position = 0;
	return i;
}

long n64_file_size(long handle)
{
	return valid(handle) ? files[handle].size : -1;
}

/* (from the start; only noted: the next read seeks). Past the end is
allowed, as fseek allows it on the N64: the next read gives 0 bytes */
long n64_file_seek(long handle, long offset)
{
	if (!valid(handle) || offset < 0)
		return -1;
	files[handle].position = offset;
	return offset;
}

/* as fread: the bytes read, short only at the end of the file or on a
read error */
long n64_file_read(long handle, void *buffer, long size)
{
	uint8_t *out = buffer;
	long done = 0;

	if (!valid(handle) || size <= 0 || files[handle].position >= files[handle].size)
		return 0;
	if (size > files[handle].size - files[handle].position)
		size = files[handle].size - files[handle].position;
	if (!bounce && !(bounce = memalign(32, BOUNCE_SIZE)))
		return 0;
	while (done < size)
	{
		long position = files[handle].position, count;
		ssize_t result;

		if (!(position & (SECTOR_SIZE - 1)) && !((uintptr_t)out & 31) && size - done >= SECTOR_SIZE)
		{
			/* whole sectors straight into the buffer */
			count = (size - done) & ~(long)(SECTOR_SIZE - 1);
			if (fs_seek(files[handle].fd, position, SEEK_SET) != position)
				break;
			result = fs_read(files[handle].fd, out, count);
			if (result <= 0)
				break;
			count = result;
		}
		else
		{
			if (bounce_owner != handle || position < bounce_start || position >= bounce_start + bounce_length)
			{
				long start = position & ~(long)(SECTOR_SIZE - 1);

				/* (nothing held while the buffer fills: a failed read leaves
				it describing nothing) */
				bounce_owner = -1;
				if (fs_seek(files[handle].fd, start, SEEK_SET) != start)
					break;
				result = fs_read(files[handle].fd, bounce, BOUNCE_SIZE);
				if (result <= 0)
					break;
				bounce_owner = handle;
				bounce_start = start;
				bounce_length = result;
			}
			count = bounce_start + bounce_length - position;
			if (count > size - done)
				count = size - done;
			memcpy(out, bounce + (position - bounce_start), count);
		}
		out += count;
		done += count;
		files[handle].position += count;
	}
	return done;
}

void n64_file_close(long handle)
{
	if (!valid(handle))
		return;
	fs_close(files[handle].fd);
	files[handle].open = 0;
	/* (handle numbers are used again) */
	if (bounce_owner == handle)
		bounce_owner = -1;
}

/* ---------- testing settings

The N64's port.json "flags" (an empty file: a flag; a file holding text: a
setting). None as shipped: the game boots to the menu (dc_menu.c), whose
choice starts each game. */

static const struct
{
	const char *name, *value;
} dc_settings[] = {
	/* {"debug_map", "bloodgulch"}, */	/* straight into a game, no menu (with debug_players, debug_bots) */
	/* {"debug_players", "1"}, */
	/* {"debug_bots", "3"}, */
	/* {"autotest", ""}, */	/* the scripted controllers instead of the pads (straight into a game: alone, the N64's two players in split screen; with debug_map, debug_players 1, debug_bots 3, the one player and three bots earlier tests used) */
	/* {"debug_menu", "..s..s"}, */	/* the menu's presses, one every 20 frames (dc_menu.c) */
	/* {"debug_game_ticks", "1800"}, */	/* each game ended after so many ticks: its results, the menu */
	/* {"debug_weapon", "weapons\\plasma rifle\\plasma rifle"}, */	/* the players' first weapon */
	/* {"debug_explosions", ""}, */	/* an explosion of each kind in turn ahead of the view (dc_fx.c) */
	/* {"dc_fx_cells", ""}, */	/* particles in their own pictures, not the N64's soft blob */
	/* {"dc_no_pause", ""}, */	/* START held opens no pause menu */
	/* {"dc_pause_test", ""}, */	/* port A's START, A, B, START at 25, 33, 41, 49 s (dc_input.c) */
	/* {"dc_scheme", "north"}, */	/* the stick moves, A B X Y look */
	/* {"dc_sleep_pacing", ""}, */	/* frames paced by Sleep, not the display's refresh (dc_pace.c) */
	/* {"dc_sound_test", ""}, */	/* the bank's files alone, a loop swept, a disc load; then the game's (dc_sound.c) */
	/* {"debug_tick_phases", ""}, */	/* the game tick's breakdowns after each tick: line (n64_game.c) */
	/* {"debug_sound", ""}, */	/* each sound started logged (300), the voices every 150 ticks (n64_sound_game.c) */
	/* {"debug_no_sound", ""}, */	/* no sounds loaded or played */
};
#define DC_SETTING_COUNT ((int)(sizeof(dc_settings) / sizeof(dc_settings[0])))

static const char *dc_setting(const char *name)
{
	int i;

	for (i = 0; i < DC_SETTING_COUNT; i++)
	{
		if (!strcmp(dc_settings[i].name, name))
			return dc_settings[i].value;
	}
	return NULL;
}

int n64_debug_flag(const char *name)
{
	return dc_setting(name) != NULL;
}

int n64_debug_text(const char *name, char *buffer, int size)
{
	const char *value = dc_setting(name);

	if (!value || size <= 0)
		return 0;
	snprintf(buffer, size, "%s", value);
	return 1;
}

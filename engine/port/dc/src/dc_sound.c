/*
DC_SOUND.C

The game's sounds on the AICA: n64.h's sound API (port/n64/src/n64_sound.c
on the N64), for port/n64/game/n64_sound_game.c, which plays Halo's sound
tags (tools/n64/sounds.py's records, as on the N64) by asking for a file of
the map's list on a voice: its volume, pan, pitch, priority and whether it
loops, then each frame the volume, pan and pitch again as the listener and
the sound move.

Each voice is one of the AICA's 64 channels (0-31 here), driven through
KallistiOS's own sound driver on the ARM (snd_init's stream.drv: a command
queue in sound RAM it reads every 10 ms or so; AICA_CMD_CHAN start, stop and
update, as snd_sfx does): the channel plays the file's 4-bit Yamaha ADPCM
straight from sound RAM at the file's rate times the pitch, looping from its
start or once, its volume and pan the AICA's (KallistiOS's 0-255 linear,
made the AICA's logarithmic level; a pan from the middle cuts the far side
in 3 dB steps: -9 dB at the far left or right, the N64's "the far side keeps
some of it").

The files: rom:/snd/<map>.dcs (tools/n64/sounds_dc.py), 858 on Blood
Gulch, 9 MB, where sound RAM is 2 MB (1.8 after the driver). The bank's
first files (each sound's first take, the announcer, weapons, impacts,
vehicles, footsteps... first) are loaded at the map's load, as many as fill
sound RAM less a margin; the rest come from the disc when asked for, read
by a thread of its own (below the game's priority; its CD reads are DMA
and wait on a semaphore, so the game runs meanwhile) into the room the
least recently played files leave. A file not in sound RAM yet plays as
another take of the same sound that is (the bank's alternates: each file's
next permutation in its pitch range, a ring) while it loads; with none, the
voice waits for the disc, up to DC_LATE_LIMIT for a sound played once (then
it is dropped), as long as it takes for a loop.

The game asks whether a voice still plays (n64_voice_playing): the time the
file takes at the voice's rate is kept here, the rate's changes included,
plus a margin for the driver's latency. While the game is paused (the pause
menu) the voices play on unheard, as on the N64.

If anything of it is missing (no bank on the disc, sound RAM too small, the
driver not answering) the game plays silent: n64_sound_load_map says no.
The 'dcs:' line every 120 frames (dc_render.c): voices, what was started,
stolen, refused, played in another take's place, waited for or dropped,
the disc's loads and their time, and how many voices the AICA was seen to
move (the driver's copy of each channel's position, read once 60 ms into
a play: the hardware playing, not only this file's bookkeeping).
*/

#include <kos.h>
#include <dc/sound/sound.h>
#include <dc/sound/aica_comm.h>
#include <dc/spu.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "dc_render.h"

#ifndef DC_FILE_ROOT
#define DC_FILE_ROOT "/cd"
#endif

#define DC_VOICES 32
/* a voice's highest pitch (the N64's) and lowest */
#define DC_MAXIMUM_PITCH 2.0f
#define DC_MINIMUM_PITCH 0.25f
/* a voice counts as playing this much past its file's end (the driver's
command latency) */
#define DC_END_MARGIN_US 30000
/* how long a sound played once may wait for the disc */
#define DC_LATE_LIMIT_US 300000
/* sound RAM kept free of the map's first files for the ones asked for
later */
#define DC_PRELOAD_MARGIN (160 * 1024)
#define DC_LOAD_QUEUE 64
#define DC_SECTOR 2048
/* the loader's buffer: the largest file (65534 samples, 32 KB) and the
sector it starts in; the preload's window */
#define DC_BUFFER_SIZE (64 * 1024)
/* at most this many files given up to make room for one */
#define DC_MAXIMUM_EVICTIONS 24
/* the KallistiOS pan of the far left/right: 3 dB steps of 8 (aica.c
calc_aica_pan) */
#define DC_PAN_RANGE 28.0f

/* ---------- snd/<map>.dcs (sounds_dc.py), little-endian */

struct dcs_header
{
	char magic[4];
	uint32_t version;
	uint16_t file_count, first_takes;
	uint32_t table_offset, data_offset, data_size;
	uint32_t pad[2];
};

struct dcs_file
{
	uint32_t offset;
	uint32_t samples;
	uint16_t rate;
	uint16_t alternate;
	uint8_t sound_class, rank;
	uint16_t pad;
};

typedef char dcs_sizes_assert[sizeof(struct dcs_header) == 32 && sizeof(struct dcs_file) == 16 ? 1 : -1];

enum
{
	FILE_ABSENT,
	FILE_QUEUED, /* given room, waiting for the loader */
	FILE_READY
};

enum
{
	VOICE_FREE,
	VOICE_WAITING, /* for its file to load */
	VOICE_PLAYING
};

/* a file of the bank, as this keeps it */
struct dc_sound_file
{
	uint32_t address; /* in sound RAM (0: none) */
	uint32_t bytes;
	uint32_t last_used; /* the play count when it last played */
	uint16_t users; /* voices playing or waiting for it */
	volatile uint8_t state;
	uint8_t pad;
};

/* a stretch of the cache in sound RAM, in address order */
struct dc_block
{
	uint32_t start, size;
	int file; /* -1: free */
};

struct dc_voice
{
	int state;
	int file;
	int loop;
	int priority;
	unsigned short generation;
	/* as last set (the game's) and as last sent (KallistiOS's units) */
	float volume, pan, pitch;
	int sent_volume, sent_pan, sent_frequency;
	/* the file's samples played by since_us at frequency (Hz); started
	at, the AICA's position looked at, waiting since */
	float played;
	uint64_t since_us, start_us, waiting_us;
	int checked;
};

static struct
{
	int ready; /* the driver up, the arena and the loader made */
	int failed;
	uint32_t arena, arena_size;
	/* the map's bank */
	int loaded;
	/* the bank's files kept off the music's rings (the arena's first
	DC_MUSIC_SPACE): the menus' bank (ui: Halo's clicks under its title
	music) */
	int music_room;
	file_t fd;
	struct dcs_header header;
	struct dcs_file *table;
	struct dc_sound_file *files;
	struct dc_block *blocks;
	int block_count, block_capacity;
	struct dc_voice voices[DC_VOICES];
	int paused;
	uint32_t plays;
	/* testing setting "dc_sound_test" (sound_test) */
	int test, test_step, test_file;
	long test_voice;
	uint64_t test_next_us, test_loop_us;
	/* the loader: files queued (game), taken (loader) */
	uint16_t queue[DC_LOAD_QUEUE];
	volatile unsigned queue_head, queue_tail;
	volatile int loader_busy;
	semaphore_t loader_wake;
	kthread_t *loader;
	uint8_t *buffer;
	/* statistics: since boot (the n64.h ones) and since the last line */
	unsigned long started, stolen, refused;
	unsigned long line_started, line_stolen, line_refused, substituted, waited, dropped, evicted;
	unsigned long aica_moving, aica_still;
	uint64_t waited_worst_us;
	volatile unsigned long loads, load_bytes;
	volatile uint32_t load_worst_us, load_total_us;
} snd;

/* ---------- the driver's commands (KallistiOS snd_sfxmgr.c's) */

static void aica_channel(int channel, uint32_t command, const struct dc_voice *voice)
{
	AICA_CMDSTR_CHANNEL(packet, cmd, chan);

	memset(packet, 0, sizeof(packet));
	cmd->cmd = AICA_CMD_CHAN;
	cmd->timestamp = 0;
	cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
	cmd->cmd_id = channel;
	chan->cmd = command;
	chan->freq = 44100;
	if (voice)
	{
		const struct dc_sound_file *file = &snd.files[voice->file];
		uint32_t samples = snd.table[voice->file].samples;

		chan->base = file->address;
		chan->type = AICA_SM_ADPCM;
		chan->length = samples;
		chan->loop = voice->loop;
		chan->loopstart = 0;
		chan->loopend = samples;
		chan->freq = voice->sent_frequency;
		chan->vol = voice->sent_volume;
		chan->pan = voice->sent_pan;
	}
	snd_sh4_to_aica(packet, cmd->size);
}

/* the game's volume, pan and pitch in the driver's units */
static int voice_volume_units(const struct dc_voice *voice)
{
	float volume = snd.paused ? 0.0f : voice->volume;
	int units = (int)(volume * 255.0f + 0.5f);

	return units < 0 ? 0 : units > 255 ? 255 : units;
}

static int voice_pan_units(const struct dc_voice *voice)
{
	float pan = voice->pan < -1.0f ? -1.0f : voice->pan > 1.0f ? 1.0f : voice->pan;

	return 128 + (int)(pan * DC_PAN_RANGE);
}

static int voice_frequency(const struct dc_voice *voice)
{
	float pitch = voice->pitch;

	if (pitch > DC_MAXIMUM_PITCH)
		pitch = DC_MAXIMUM_PITCH;
	if (pitch < DC_MINIMUM_PITCH)
		pitch = DC_MINIMUM_PITCH;
	return (int)(snd.table[voice->file].rate * pitch + 0.5f);
}

/* ---------- the cache in sound RAM */

/* the music's two rings (MUSIC_RING's 16-bit samples, each), at the
arena's start */
#define DC_MUSIC_SPACE (2 * 4096 * 8 * 2)

static void blocks_reset(void)
{
	uint32_t skip = snd.music_room ? DC_MUSIC_SPACE : 0;

	snd.block_count = 1;
	snd.blocks[0].start = snd.arena + skip;
	snd.blocks[0].size = snd.arena_size - skip;
	snd.blocks[0].file = -1;
}

static void block_free(int file)
{
	int i;

	for (i = 0; i < snd.block_count && snd.blocks[i].file != file; i++)
		;
	if (i == snd.block_count)
		return;
	snd.blocks[i].file = -1;
	/* (with the free stretches either side) */
	if (i + 1 < snd.block_count && snd.blocks[i + 1].file < 0)
	{
		snd.blocks[i].size += snd.blocks[i + 1].size;
		memmove(&snd.blocks[i + 1], &snd.blocks[i + 2], (snd.block_count - i - 2) * sizeof(struct dc_block));
		snd.block_count--;
	}
	if (i > 0 && snd.blocks[i - 1].file < 0)
	{
		snd.blocks[i - 1].size += snd.blocks[i].size;
		memmove(&snd.blocks[i], &snd.blocks[i + 1], (snd.block_count - i - 1) * sizeof(struct dc_block));
		snd.block_count--;
	}
	snd.files[file].address = 0;
	snd.files[file].state = FILE_ABSENT;
}

/* the first free stretch it fits, else 0 */
static uint32_t block_take(int file, uint32_t bytes)
{
	int i;

	for (i = 0; i < snd.block_count; i++)
	{
		struct dc_block *block = &snd.blocks[i];

		if (block->file >= 0 || block->size < bytes)
			continue;
		if (block->size > bytes)
		{
			if (snd.block_count == snd.block_capacity)
				return 0;
			memmove(&snd.blocks[i + 1], &snd.blocks[i], (snd.block_count - i) * sizeof(struct dc_block));
			snd.block_count++;
			snd.blocks[i + 1].start = block->start + bytes;
			snd.blocks[i + 1].size = block->size - bytes;
			block->size = bytes;
		}
		block->file = file;
		return block->start;
	}
	return 0;
}

/* room for a file: free room, else the least recently played files no
voice uses given up (of those never played, the last of the bank's order
first) */
static uint32_t cache_room(int file, uint32_t bytes)
{
	int evictions;

	for (evictions = 0; evictions <= DC_MAXIMUM_EVICTIONS; evictions++)
	{
		uint32_t address = block_take(file, bytes);
		int victim = -1, i;

		if (address)
			return address;
		for (i = 0; i < snd.header.file_count; i++)
		{
			const struct dc_sound_file *candidate = &snd.files[i];

			if (candidate->state != FILE_READY || candidate->users)
				continue;
			if (victim < 0 || candidate->last_used < snd.files[victim].last_used ||
				(candidate->last_used == snd.files[victim].last_used &&
					snd.table[i].offset > snd.table[victim].offset))
				victim = i;
		}
		if (victim < 0)
			return 0;
		block_free(victim);
		snd.evicted++;
	}
	return 0;
}

/* a file asked for: given room and queued for the loader (FALSE: no room,
or the queue full) */
static int cache_request(int file)
{
	struct dc_sound_file *entry = &snd.files[file];

	if (entry->state != FILE_ABSENT)
		return 1;
	if (snd.queue_head - snd.queue_tail >= DC_LOAD_QUEUE)
		return 0;
	entry->address = cache_room(file, entry->bytes);
	if (!entry->address)
		return 0;
	entry->state = FILE_QUEUED;
	snd.queue[snd.queue_head % DC_LOAD_QUEUE] = (uint16_t)file;
	/* (the entry written before the loader can see it) */
	__asm__ volatile("" ::: "memory");
	snd.queue_head++;
	sem_signal(&snd.loader_wake);
	return 1;
}

/* the loader's thread: each queued file read from the disc (its sectors,
DMA into the 32-byte aligned buffer) and copied to its room in sound RAM
(plain G2 writes, not the store queues: the game's PowerVR lists use
those) */
static void *dc_sound_loader(void *argument)
{
	(void)argument;
	for (;;)
	{
		sem_wait(&snd.loader_wake);
		while (snd.queue_tail != snd.queue_head)
		{
			int file;
			const struct dcs_file *entry;
			uint32_t start, skip, count;
			uint64_t began = timer_us_gettime64();

			snd.loader_busy = 1;
			file = snd.queue[snd.queue_tail % DC_LOAD_QUEUE];
			entry = &snd.table[file];
			start = entry->offset & ~(DC_SECTOR - 1);
			skip = entry->offset - start;
			count = (skip + snd.files[file].bytes + DC_SECTOR - 1) & ~(DC_SECTOR - 1);
			if (count > DC_BUFFER_SIZE)
				count = DC_BUFFER_SIZE;
			if (fs_seek(snd.fd, start, SEEK_SET) == (off_t)start && fs_read(snd.fd, snd.buffer, count) >= (ssize_t)(skip + snd.files[file].bytes))
			{
				spu_memload(snd.files[file].address, snd.buffer + skip, snd.files[file].bytes);
				snd.load_bytes += snd.files[file].bytes;
			}
			else
			{
				/* (unreadable: silence, rather than whatever sound RAM held) */
				spu_memset(snd.files[file].address, 0, snd.files[file].bytes);
			}
			snd.files[file].state = FILE_READY;
			{
				uint32_t took = (uint32_t)(timer_us_gettime64() - began);

				snd.loads++;
				snd.load_total_us += took;
				if (took > snd.load_worst_us)
					snd.load_worst_us = took;
			}
			snd.queue_tail++;
			snd.loader_busy = 0;
		}
	}
	return NULL;
}

/* the loader done with everything queued (the map's unload) */
static void loader_flush(void)
{
	while (snd.loader && (snd.queue_tail != snd.queue_head || snd.loader_busy))
		thd_sleep(5);
}

/* ---------- voices */

static void voice_release(struct dc_voice *voice)
{
	if (voice->state != VOICE_FREE && snd.files[voice->file].users)
		snd.files[voice->file].users--;
	voice->state = VOICE_FREE;
}

static void voice_stop(int slot)
{
	struct dc_voice *voice = &snd.voices[slot];

	if (voice->state == VOICE_PLAYING)
		aica_channel(slot, AICA_CH_CMD_STOP, NULL);
	voice_release(voice);
}

/* the sample its channel is at by now (as kept here) */
static float voice_played(const struct dc_voice *voice, uint64_t now)
{
	return voice->played + (float)(now - voice->since_us) * (float)voice->sent_frequency * 1e-6f;
}

static void voice_key_on(int slot, uint64_t now)
{
	struct dc_voice *voice = &snd.voices[slot];

	voice->sent_volume = voice_volume_units(voice);
	voice->sent_pan = voice_pan_units(voice);
	voice->sent_frequency = voice_frequency(voice);
	voice->played = 0.0f;
	voice->since_us = voice->start_us = now;
	voice->checked = 0;
	voice->state = VOICE_PLAYING;
	snd.files[voice->file].last_used = ++snd.plays;
	aica_channel(slot, AICA_CH_CMD_START, voice);
}

/* a voice's state now: a sound played once over at its end, a wait for
the disc over (started, or dropped past the limit) */
static void voice_update(int slot, uint64_t now)
{
	struct dc_voice *voice = &snd.voices[slot];

	if (voice->state == VOICE_PLAYING && !voice->loop)
	{
		/* (its end: the samples left at its rate since the rate's last
		change, and the driver's latency) */
		float left = (float)snd.table[voice->file].samples - voice->played;

		if (now >= voice->since_us + (uint64_t)(left * 1e6f / (float)voice->sent_frequency) + DC_END_MARGIN_US)
			voice_release(voice);
	}
	else if (voice->state == VOICE_WAITING)
	{
		if (snd.files[voice->file].state == FILE_READY)
		{
			uint64_t waited = now - voice->waiting_us;

			if (waited > snd.waited_worst_us)
				snd.waited_worst_us = waited;
			snd.waited++;
			voice_key_on(slot, now);
		}
		else if (!voice->loop && now - voice->waiting_us > DC_LATE_LIMIT_US)
		{
			snd.dropped++;
			voice_release(voice);
		}
	}
}

static int voice_slot(long voice)
{
	int slot = (int)(voice & 0xFF);

	if (voice < 0 || slot >= DC_VOICES || snd.voices[slot].generation != (unsigned short)(voice >> 8) ||
		snd.voices[slot].state == VOICE_FREE)
		return -1;
	return slot;
}

/* a free voice, else the cheapest of no higher priority (libdragon's
mixer_ch_alloc, as the N64 has it: lower priority, quieter, older) */
static int voice_choose(int priority)
{
	int best = -1, i;

	for (i = 0; i < DC_VOICES; i++)
	{
		const struct dc_voice *voice = &snd.voices[i];

		if (voice->state == VOICE_FREE)
			return i;
		if (voice->priority > priority)
			continue;
		if (best < 0 || voice->priority < snd.voices[best].priority ||
			(voice->priority == snd.voices[best].priority &&
				(voice->volume < snd.voices[best].volume ||
					(voice->volume == snd.voices[best].volume && voice->start_us < snd.voices[best].start_us))))
			best = i;
	}
	return best;
}

static long voice_play(int file, float volume, float pan, float pitch, int priority, int loop);

/* ---------- start-up and the map's bank */

void n64_sound_initialize(void)
{
	kthread_attr_t attributes;

	if (snd.ready || snd.failed)
		return;
	snd.failed = 1;
	if (snd_init() < 0)
	{
		n64_log("dcs: no sound driver");
		return;
	}
	/* (the whole of sound RAM the driver leaves, for good) */
	snd.arena_size = snd_mem_available() & ~31u;
	snd.arena = snd.arena_size ? snd_mem_malloc(snd.arena_size) : 0;
	snd.buffer = memalign(32, DC_BUFFER_SIZE);
	if (!snd.arena || !snd.buffer)
	{
		n64_log("dcs: no sound RAM");
		return;
	}
	sem_init(&snd.loader_wake, 0);
	memset(&attributes, 0, sizeof(attributes));
	attributes.stack_size = 8 * 1024;
	attributes.prio = PRIO_DEFAULT + 1;
	attributes.label = "sound loader";
	snd.loader = thd_create_ex(&attributes, dc_sound_loader, NULL);
	if (!snd.loader)
	{
		n64_log("dcs: no loader thread");
		return;
	}
	snd.fd = -1;
	snd.failed = 0;
	snd.ready = 1;
}

void n64_sound_reserve(void)
{
	n64_sound_initialize();
}

/* ---------- the title music (the menu's: music/title.dca, tools/n64/menu_dc.py)

Halo's title music, stereo at 44100 Hz: its start ("in", the monks) once,
then its three loops, one at a time, a random one each time but never the
same twice running (n64_sound.c's order). An AICA channel plays at most
65535 samples, the music 233 s: it is streamed. The file is the AICA's
4-bit ADPCM, each channel in blocks of MUSIC_BLOCK samples; a thread of its
own reads it from the disc (MUSIC_CHUNK blocks at a time) and decodes it on
the SH-4 (sounds_dc.py's decoder, each version from the decoder's resting
state: one version runs straight into the next, which the AICA's own
decoder, carrying its state across, could not do) into two rings of 16-bit
samples in sound RAM, which channels MUSIC_LEFT and MUSIC_RIGHT loop over,
panned hard left and right. A ring is MUSIC_QUARTERS parts of a block
each; every 20 ms the thread fills every part the channels have left
behind (the driver keeps each channel's position), up to the one playing:
the music is most of a ring (0.65 s) ahead, a disc read's worth.

Only while no map is loaded (the menu): the rings are the start of the
sound RAM the map's files use (n64_sound_load_map stops the music first).

The boot intro's sound (dc_intro.c) goes the same way from a file read into
RAM beforehand (dc_music_play_memory), its one version played once, then
silence: the disc meanwhile reads only the intro's pictures. */

#define MUSIC_LEFT 62
#define MUSIC_RIGHT 63
#define MUSIC_BLOCK 4096
#define MUSIC_QUARTERS 8
#define MUSIC_RING (MUSIC_BLOCK * MUSIC_QUARTERS)
#define MUSIC_CHUNK 8
#define MUSIC_VERSIONS 8
/* KallistiOS's 0-255: the N64's 0.8 */
#define MUSIC_VOLUME 204

struct dca_header
{
	char magic[4];
	uint32_t version, rate;
	uint16_t count, first_loop;
	uint32_t block_bytes, data_offset;
	uint32_t pad[2];
};

struct dca_version
{
	uint32_t offset, blocks, samples, pad;
};

typedef char dca_sizes_assert[sizeof(struct dca_header) == 32 && sizeof(struct dca_version) == 16 ? 1 : -1];

static struct
{
	int on;
	volatile int stop;
	kthread_t *thread;
	file_t fd;
	struct dca_header header;
	struct dca_version versions[MUSIC_VERSIONS];
	uint32_t ring[2];
	/* the version playing, its next block (the chunk's first, and how many
	are in RAM), the place in the block, the samples left in the version */
	int version, last_loop;
	uint32_t block, chunk_first, chunk_count, place, left;
	uint8_t *chunk;
	int16_t *pcm;
	/* each channel's decoder */
	int signal[2], step[2];
	uint32_t random;
	unsigned long parts, reads, late, versions_started;
	/* the file in RAM instead of on the disc (dc_music_play_memory), and
	played once: finished, silence after it */
	const uint8_t *memory;
	unsigned long memory_size;
	int once;
	volatile int finished;
} music = {.fd = -1};

/* the AICA's ADPCM (sounds_dc.py aica_adpcm_decode): count samples of one
channel's nibbles from nibble first, the decoder's state carried */
static void music_decode(const uint8_t *data, uint32_t first, int count, int16_t *out, int *signal, int *step)
{
	static const int scale[8] = {230, 230, 230, 230, 307, 409, 512, 614};
	int s = *signal, k = *step, i;

	for (i = 0; i < count; i++, first++)
	{
		int nibble = (data[first >> 1] >> ((first & 1) << 2)) & 15, code = nibble & 7;
		int diff = ((1 + (code << 1)) * k) >> 3;

		if (diff > 32767)
			diff = 32767;
		s = nibble & 8 ? (s - diff < -32768 ? -32768 : s - diff) : (s + diff > 32767 ? 32767 : s + diff);
		k = (scale[code] * k) >> 8;
		k = k < 127 ? 127 : k > 24576 ? 24576 : k;
		out[i] = (int16_t)s;
	}
	*signal = s;
	*step = k;
}

/* the next version: the start first, then a random loop, not the last
one again */
static void music_next_version(void)
{
	int loops = music.header.count - music.header.first_loop, pick;

	if (music.version < 0)
		pick = 0;
	else if (loops <= 1)
		pick = music.header.first_loop;
	else
	{
		do
		{
			music.random = music.random * 1664525u + 1013904223u;
			pick = music.header.first_loop + (int)((music.random >> 16) % (uint32_t)loops);
		} while (pick == music.last_loop);
	}
	if (pick >= music.header.first_loop)
		music.last_loop = pick;
	music.version = pick;
	music.block = music.place = 0;
	music.chunk_count = 0;
	music.left = music.versions[pick].samples;
	music.signal[0] = music.signal[1] = 0;
	music.step[0] = music.step[1] = 127;
	music.versions_started++;
}

/* the current block in RAM (a chunk read from the disc when it is not):
NULL when unreadable */
static const uint8_t *music_block(void)
{
	const struct dca_version *v = &music.versions[music.version];

	if (music.memory)
	{
		unsigned long at = music.header.data_offset + v->offset + music.block * MUSIC_BLOCK;

		if (music.block >= v->blocks || at + MUSIC_BLOCK > music.memory_size)
			return NULL;
		return music.memory + at;
	}
	if (music.block < music.chunk_first || music.block >= music.chunk_first + music.chunk_count)
	{
		uint32_t count = v->blocks - music.block < MUSIC_CHUNK ? v->blocks - music.block : MUSIC_CHUNK;
		uint32_t at = music.header.data_offset + v->offset + music.block * MUSIC_BLOCK;

		music.chunk_count = 0;
		if (!count || fs_seek(music.fd, at, SEEK_SET) != (off_t)at ||
			fs_read(music.fd, music.chunk, count * MUSIC_BLOCK) != (ssize_t)(count * MUSIC_BLOCK))
			return NULL;
		music.chunk_first = music.block;
		music.chunk_count = count;
		music.reads++;
	}
	return music.chunk + (music.block - music.chunk_first) * MUSIC_BLOCK;
}

/* one part of the rings: MUSIC_BLOCK samples a channel, decoded and
loaded (silence where the disc failed) */
static void music_fill(int part)
{
	int16_t *left = music.pcm, *right = music.pcm + MUSIC_BLOCK;
	int done = 0;

	while (done < MUSIC_BLOCK)
	{
		const uint8_t *block;
		int count;

		if (!music.left)
		{
			if (music.once && music.version >= 0)
			{
				music.finished = 1;
				memset(left + done, 0, (MUSIC_BLOCK - done) * 2);
				memset(right + done, 0, (MUSIC_BLOCK - done) * 2);
				break;
			}
			music_next_version();
		}
		if (!(block = music_block()))
		{
			memset(left + done, 0, (MUSIC_BLOCK - done) * 2);
			memset(right + done, 0, (MUSIC_BLOCK - done) * 2);
			break;
		}
		count = MUSIC_BLOCK - done;
		if ((uint32_t)count > MUSIC_BLOCK - music.place)
			count = (int)(MUSIC_BLOCK - music.place);
		if ((uint32_t)count > music.left)
			count = (int)music.left;
		music_decode(block, music.place, count, left + done, &music.signal[0], &music.step[0]);
		music_decode(block + MUSIC_BLOCK / 2, music.place, count, right + done, &music.signal[1], &music.step[1]);
		done += count;
		music.place += count;
		music.left -= count;
		if (music.place == MUSIC_BLOCK)
		{
			music.block++;
			music.place = 0;
		}
	}
	spu_memload(music.ring[0] + part * MUSIC_BLOCK * 2, left, MUSIC_BLOCK * 2);
	spu_memload(music.ring[1] + part * MUSIC_BLOCK * 2, right, MUSIC_BLOCK * 2);
	music.parts++;
}

/* a channel looping over its ring (ring 0: stopped) */
static void music_channel(int channel, uint32_t ring, int pan)
{
	AICA_CMDSTR_CHANNEL(packet, cmd, chan);

	memset(packet, 0, sizeof(packet));
	cmd->cmd = AICA_CMD_CHAN;
	cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
	cmd->cmd_id = channel;
	chan->cmd = ring ? AICA_CH_CMD_START : AICA_CH_CMD_STOP;
	chan->base = ring;
	chan->type = AICA_SM_16BIT;
	chan->length = MUSIC_RING;
	chan->loop = 1;
	chan->loopstart = 0;
	chan->loopend = MUSIC_RING - 1;
	chan->freq = music.header.rate;
	chan->vol = MUSIC_VOLUME;
	chan->pan = pan;
	snd_sh4_to_aica(packet, cmd->size);
}

static void *music_main(void *argument)
{
	/* the next part to fill; the part playing at the last look; how many
	parts past it hold music not played yet */
	int part, playing, last = 0, ahead = MUSIC_QUARTERS - 2;

	(void)argument;
	for (part = 0; part < MUSIC_QUARTERS - 1; part++)
		music_fill(part);
	/* (both channels started in one go of the driver's queue) */
	snd_sh4_to_aica_stop();
	music_channel(MUSIC_LEFT, music.ring[0], 0);
	music_channel(MUSIC_RIGHT, music.ring[1], 255);
	snd_sh4_to_aica_start();
	while (!music.stop)
	{
		thd_sleep(20);
		playing = snd_get_pos(MUSIC_LEFT) / MUSIC_BLOCK;
		if (playing >= MUSIC_QUARTERS)
			continue;
		/* (the channel went past the parts filled ahead of it: it played
		one again, late) */
		ahead -= (playing - last + MUSIC_QUARTERS) % MUSIC_QUARTERS;
		if (ahead < 0)
			music.late++;
		last = playing;
		while (part != playing && !music.stop)
		{
			music_fill(part);
			part = (part + 1) % MUSIC_QUARTERS;
		}
		if (part == playing)
			ahead = MUSIC_QUARTERS - 1;
	}
	music_channel(MUSIC_LEFT, 0, 0);
	music_channel(MUSIC_RIGHT, 0, 0);
	return NULL;
}

/* the music's thread and channels, its header and versions read (both
music_start's) */
static int music_begin(const char *what)
{
	char line[120];
	kthread_attr_t attributes;

	if (!music.pcm)
		music.pcm = memalign(32, MUSIC_BLOCK * 2 * sizeof(int16_t));
	if (!music.pcm)
		return 0;
	/* (the start of the map files' sound RAM, free while no map is loaded) */
	music.ring[0] = snd.arena;
	music.ring[1] = snd.arena + MUSIC_RING * 2;
	music.version = music.last_loop = -1;
	music.left = 0;
	music.chunk_count = 0;
	music.finished = 0;
	music.parts = music.reads = music.late = music.versions_started = 0;
	music.random ^= (uint32_t)timer_us_gettime64();
	music.stop = 0;
	memset(&attributes, 0, sizeof(attributes));
	attributes.stack_size = 8 * 1024;
	attributes.prio = PRIO_DEFAULT;
	attributes.label = what;
	music.thread = thd_create_ex(&attributes, music_main, NULL);
	if (!music.thread)
		return 0;
	music.on = 1;
	snprintf(line, sizeof(line), "dca: %s, %u versions at %lu Hz", what, music.header.count,
		(unsigned long)music.header.rate);
	n64_log(line);
	return 1;
}

/* a DCA file in RAM (the boot intro's sound), played once from its start;
0 if it could not be (no sound, a map loaded, not a DCA file) */
int dc_music_play_memory(const void *data, unsigned long size)
{
	const uint8_t *bytes = data;

	n64_sound_initialize();
	if (music.on || !snd.ready || snd.loaded || snd.arena_size < 2 * MUSIC_RING * 2 ||
		n64_debug_flag("debug_no_sound") || size < sizeof(music.header))
		return 0;
	memcpy(&music.header, bytes, sizeof(music.header));
	if (memcmp(music.header.magic, "DCMU", 4) || music.header.version != 1 || !music.header.count ||
		music.header.count > MUSIC_VERSIONS || music.header.first_loop >= music.header.count ||
		music.header.block_bytes != MUSIC_BLOCK ||
		sizeof(music.header) + music.header.count * sizeof(struct dca_version) > size)
		return 0;
	memcpy(music.versions, bytes + sizeof(music.header), music.header.count * sizeof(struct dca_version));
	music.memory = bytes;
	music.memory_size = size;
	music.once = 1;
	if (!music_begin("the intro's sound"))
	{
		music.memory = NULL;
		music.once = 0;
		return 0;
	}
	return 1;
}

/* whether music played once has run out */
int dc_music_finished(void)
{
	return !music.on || music.finished;
}

/* the title music from its start (nothing if it already plays) */
void n64_music_start(void)
{
	char line[120];
	kthread_attr_t attributes;

	n64_sound_initialize();
	if (music.on || !snd.ready || (snd.loaded && !snd.music_room) || snd.arena_size < 2 * MUSIC_RING * 2 ||
		n64_debug_flag("debug_no_sound"))
		return;
	music.fd = fs_open(DC_FILE_ROOT "/music/title.dca", O_RDONLY);
	if (music.fd < 0)
	{
		n64_log("dca: no title music");
		return;
	}
	if (fs_read(music.fd, &music.header, sizeof(music.header)) != sizeof(music.header) ||
		memcmp(music.header.magic, "DCMU", 4) || music.header.version != 1 || !music.header.count ||
		music.header.count > MUSIC_VERSIONS || music.header.first_loop >= music.header.count ||
		music.header.block_bytes != MUSIC_BLOCK ||
		fs_read(music.fd, music.versions, music.header.count * sizeof(struct dca_version)) !=
			(ssize_t)(music.header.count * sizeof(struct dca_version)))
	{
		n64_log("dca: the title music is not this build's");
		fs_close(music.fd);
		music.fd = -1;
		return;
	}
	if (!music.chunk)
		music.chunk = memalign(32, MUSIC_CHUNK * MUSIC_BLOCK);
	if (!music.pcm)
		music.pcm = memalign(32, MUSIC_BLOCK * 2 * sizeof(int16_t));
	if (!music.chunk || !music.pcm)
	{
		fs_close(music.fd);
		music.fd = -1;
		return;
	}
	/* (the start of the map files' sound RAM, free while no map is loaded) */
	music.ring[0] = snd.arena;
	music.ring[1] = snd.arena + MUSIC_RING * 2;
	music.version = music.last_loop = -1;
	music.left = 0;
	music.chunk_count = 0;
	music.parts = music.reads = music.late = music.versions_started = 0;
	/* (the loops' order: the menu comes up at much the same time each boot) */
	music.random ^= (uint32_t)timer_us_gettime64();
	music.stop = 0;
	memset(&attributes, 0, sizeof(attributes));
	attributes.stack_size = 8 * 1024;
	attributes.prio = PRIO_DEFAULT;
	attributes.label = "title music";
	music.thread = thd_create_ex(&attributes, music_main, NULL);
	if (!music.thread)
	{
		fs_close(music.fd);
		music.fd = -1;
		return;
	}
	music.on = 1;
	snprintf(line, sizeof(line), "dca: title music, %u versions at %lu Hz", music.header.count,
		(unsigned long)music.header.rate);
	n64_log(line);
}

void n64_music_stop(void)
{
	char line[120];

	if (!music.on)
		return;
	music.stop = 1;
	thd_join(music.thread, NULL);
	music.thread = NULL;
	if (music.fd >= 0)
		fs_close(music.fd);
	music.fd = -1;
	music.on = 0;
	music.memory = NULL;
	music.once = 0;
	snprintf(line, sizeof(line), "dca: music stopped: %lu parts, %lu versions, %lu disc reads, %lu late",
		music.parts, music.versions_started, music.reads, music.late);
	n64_log(line);
}

/* the music's line (the menu's statistics, dc_menu.c) */
void dc_music_statistics_line(void)
{
	char line[120];

	if (!music.on)
		return;
	snprintf(line, sizeof(line), "dca: music version %d, %lu parts, %lu disc reads, %lu late, position %u",
		music.version, music.parts, music.reads, music.late, (unsigned)snd_get_pos(MUSIC_LEFT));
	n64_log(line);
}

/* the bank's first files into sound RAM, in its order (the disc's), a 64 KB
window of sectors at a time */
static void preload(void)
{
	uint32_t window_start = 0, window_size = 0, budget, used = 0;
	int count = 0, f;
	int *order;

	budget = snd.arena_size > DC_PRELOAD_MARGIN ? snd.arena_size - DC_PRELOAD_MARGIN : 0;
	if (snd.music_room)
		budget = budget > DC_MUSIC_SPACE ? budget - DC_MUSIC_SPACE : 0;
	order = malloc(snd.header.file_count * sizeof(int));
	if (!order)
		return;
	/* (by offset: the bank's order) */
	for (f = 0; f < snd.header.file_count; f++)
	{
		int at = f;

		while (at > 0 && snd.table[order[at - 1]].offset > snd.table[f].offset)
		{
			order[at] = order[at - 1];
			at--;
		}
		order[at] = f;
	}
	for (f = 0; f < snd.header.file_count; f++)
	{
		int file = order[f];
		struct dc_sound_file *entry = &snd.files[file];
		uint32_t offset = snd.table[file].offset;

		/* (a file too big for what is left, or for the window, is skipped:
		the smaller ones after it may still fit) */
		if (used + entry->bytes > budget || entry->bytes > DC_BUFFER_SIZE - DC_SECTOR)
			continue;
		if (offset < window_start || offset + entry->bytes > window_start + window_size)
		{
			window_start = offset & ~(DC_SECTOR - 1);
			if (fs_seek(snd.fd, window_start, SEEK_SET) != (off_t)window_start)
				break;
			window_size = fs_read(snd.fd, snd.buffer, DC_BUFFER_SIZE);
			if ((int)window_size <= 0)
				break;
			if (offset + entry->bytes > window_start + window_size)
				continue;
		}
		entry->address = block_take(file, entry->bytes);
		if (!entry->address)
			break;
		spu_memload(entry->address, snd.buffer + (offset - window_start), entry->bytes);
		entry->state = FILE_READY;
		used += entry->bytes;
		count++;
	}
	free(order);
	{
		char line[160];

		snprintf(line, sizeof(line), "dcs: %d of %u files in sound RAM at the start, %lu KB of %lu KB",
			count, snd.header.file_count, (unsigned long)used / 1024, (unsigned long)snd.arena_size / 1024);
		n64_log(line);
	}
}

int n64_sound_load_map(const char *map_name)
{
	char path[96], line[160];
	uint64_t began;
	int f;

	n64_sound_initialize();
	/* (the music's rings are where the map's files go; but the menus'
	clicks go round the title music) */
	if (strcmp(map_name, "ui"))
		n64_music_stop();
	n64_sound_unload_map();
	snd.music_room = !strcmp(map_name, "ui");
	/* (testing setting "debug_no_sound": none, to measure what sound
	costs) */
	if (!snd.ready || n64_debug_flag("debug_no_sound"))
		return 0;
	began = timer_us_gettime64();
	snprintf(path, sizeof(path), DC_FILE_ROOT "/snd/%s.dcs", map_name);
	snd.fd = fs_open(path, O_RDONLY);
	if (snd.fd < 0)
	{
		snprintf(line, sizeof(line), "dcs: no sounds for %s", map_name);
		n64_log(line);
		return 0;
	}
	if (fs_read(snd.fd, &snd.header, sizeof(snd.header)) != sizeof(snd.header) ||
		memcmp(snd.header.magic, "DCSB", 4) || snd.header.version != 1 || !snd.header.file_count)
	{
		n64_log("dcs: not a sound bank");
		n64_sound_unload_map();
		return 0;
	}
	snd.table = malloc(snd.header.file_count * sizeof(struct dcs_file));
	snd.files = calloc(snd.header.file_count, sizeof(struct dc_sound_file));
	snd.block_capacity = snd.header.file_count * 2 + 4;
	snd.blocks = malloc(snd.block_capacity * sizeof(struct dc_block));
	if (!snd.table || !snd.files || !snd.blocks || fs_seek(snd.fd, snd.header.table_offset, SEEK_SET) != (off_t)snd.header.table_offset ||
		fs_read(snd.fd, snd.table, snd.header.file_count * sizeof(struct dcs_file)) !=
			(ssize_t)(snd.header.file_count * sizeof(struct dcs_file)))
	{
		n64_log("dcs: the sound bank's table unread");
		n64_sound_unload_map();
		return 0;
	}
	for (f = 0; f < snd.header.file_count; f++)
	{
		snd.files[f].bytes = ((snd.table[f].samples + 1) / 2 + 31) & ~31u;
		if (snd.table[f].alternate >= snd.header.file_count)
			snd.table[f].alternate = (uint16_t)f;
	}
	blocks_reset();
	memset(snd.voices, 0, sizeof(snd.voices));
	snd.plays = 0;
	snd.test = n64_debug_flag("dc_sound_test");
	snd.test_step = snd.test_file = 0;
	snd.test_next_us = timer_us_gettime64() + 3000000;
	preload();
	snd.loaded = 1;
	snprintf(line, sizeof(line), "dcs: %u sounds for %s (%lu KB on the disc), loaded in %lu ms", snd.header.file_count,
		map_name, (unsigned long)snd.header.data_size / 1024, (unsigned long)((timer_us_gettime64() - began) / 1000));
	n64_log(line);
	return 1;
}

void n64_sound_unload_map(void)
{
	int i;

	n64_voice_stop_all();
	snd.loaded = 0;
	snd.paused = 0;
	loader_flush();
	for (i = 0; i < DC_VOICES; i++)
		snd.voices[i].state = VOICE_FREE;
	if (snd.fd >= 0 && snd.ready)
		fs_close(snd.fd);
	snd.fd = -1;
	free(snd.table);
	free(snd.files);
	free(snd.blocks);
	snd.table = NULL;
	snd.files = NULL;
	snd.blocks = NULL;
}

/* ---------- the voices (n64.h) */

/* Testing setting "dc_sound_test" (the game's own sounds refused meanwhile),
to compare Flycast's output with the converter's decoding (tools/dc_audio.py
--compare): from 3 s after the load, every 2 s the next of the files in
sound RAM, five of them, alone, at full volume in the middle, as recorded;
then the last of them looping for 6 s, its pitch (0.5-2) and pan (left to
right) swept, and stopped; then a file not in sound RAM (a wait for the
disc), and again 2 s later (in sound RAM by then); then the game's own
sounds. Each step logged ("dcs: test ...") */
static void sound_test(uint64_t now)
{
	char line[128];

	if (snd.test_step == 6 && now < snd.test_next_us)
	{
		float t = (float)(now - snd.test_loop_us) * 1e-6f;
		float pitch = 1.25f + 0.75f * fsin(t * 1.0471976f);

		n64_voice_set(snd.test_voice, 1.0f, fsin(t * 2.0943951f), pitch);
		return;
	}
	if (now < snd.test_next_us)
		return;
	snd.test_next_us = now + 2000000;
	if (snd.test_step < 5)
	{
		while (snd.test_file < snd.header.file_count && snd.files[snd.test_file].state != FILE_READY)
			snd.test_file++;
		if (snd.test_file >= snd.header.file_count)
			return;
		snprintf(line, sizeof(line), "dcs: test file %d, %lu samples at %u Hz, voice %lx", snd.test_file,
			(unsigned long)snd.table[snd.test_file].samples, snd.table[snd.test_file].rate,
			(unsigned long)voice_play(snd.test_file, 1.0f, 0.0f, 1.0f, 255, 0));
		snd.test_file++;
	}
	else if (snd.test_step == 5)
	{
		snd.test_voice = voice_play(snd.test_file - 1, 1.0f, 0.0f, 1.0f, 255, 1);
		snd.test_loop_us = now;
		snd.test_next_us = now + 6000000;
		snprintf(line, sizeof(line), "dcs: test loop of file %d, voice %lx: pitch 0.5-2 over 6 s, pan left-right over 3 s",
			snd.test_file - 1, (unsigned long)snd.test_voice);
	}
	else if (snd.test_step == 6)
	{
		n64_voice_stop(snd.test_voice);
		for (snd.test_file = 0; snd.test_file < snd.header.file_count && snd.files[snd.test_file].state != FILE_ABSENT;
			snd.test_file++)
			;
		snprintf(line, sizeof(line), "dcs: test loop stopped (%s); file %d from the disc, voice %lx",
			n64_voice_playing(snd.test_voice) ? "still playing" : "ended", snd.test_file,
			(unsigned long)voice_play(snd.test_file, 1.0f, 0.0f, 1.0f, 255, 0));
	}
	else if (snd.test_step == 7)
	{
		snprintf(line, sizeof(line), "dcs: test file %d again (%s), voice %lx", snd.test_file,
			snd.files[snd.test_file].state == FILE_READY ? "in sound RAM" : "not loaded",
			(unsigned long)voice_play(snd.test_file, 1.0f, 0.0f, 1.0f, 255, 0));
	}
	else
	{
		snprintf(line, sizeof(line), "dcs: test over, the game's sounds from here");
		snd.test = 0;
	}
	snd.test_step++;
	n64_log(line);
}

void n64_sound_poll(void)
{
	uint64_t now;
	int i;

	if (!snd.loaded)
		return;
	now = timer_us_gettime64();
	if (snd.test)
		sound_test(now);
	for (i = 0; i < DC_VOICES; i++)
	{
		struct dc_voice *voice = &snd.voices[i];

		voice_update(i, now);
		/* the AICA's own position (the driver's copy), once, 60 ms in */
		if (voice->state == VOICE_PLAYING && !voice->checked && now - voice->start_us > 60000)
		{
			voice->checked = 1;
			if (snd_get_pos(i) > 0)
				snd.aica_moving++;
			else
				snd.aica_still++;
		}
	}
}

static long voice_play(int file, float volume, float pan, float pitch, int priority, int loop)
{
	struct dc_voice *voice;
	uint64_t now;
	int slot, play = file;

	if (!snd.loaded || file < 0 || file >= snd.header.file_count)
		return -1;
	now = timer_us_gettime64();
	if (priority < 0)
		priority = 0;
	if (priority > 255)
		priority = 255;
	slot = voice_choose(priority);
	if (slot < 0)
	{
		snd.refused++;
		return -1;
	}
	/* the file, else another take of it in sound RAM while it loads, else
	a wait for the disc */
	if (snd.files[file].state != FILE_READY)
	{
		int alternate = snd.table[file].alternate, steps;

		cache_request(file);
		for (steps = 0; steps < 64 && alternate != file && snd.files[alternate].state != FILE_READY; steps++)
			alternate = snd.table[alternate].alternate;
		if (alternate != file && snd.files[alternate].state == FILE_READY)
		{
			play = alternate;
			snd.substituted++;
		}
		else if (snd.files[file].state == FILE_ABSENT)
		{
			snd.refused++;
			return -1;
		}
	}
	voice = &snd.voices[slot];
	if (voice->state != VOICE_FREE)
	{
		snd.stolen++;
		voice_stop(slot);
	}
	voice->file = play;
	voice->loop = loop != 0;
	voice->priority = priority;
	voice->volume = volume;
	voice->pan = pan;
	voice->pitch = pitch;
	voice->generation++;
	snd.files[play].users++;
	if (snd.files[play].state == FILE_READY)
	{
		voice_key_on(slot, now);
	}
	else
	{
		voice->state = VOICE_WAITING;
		voice->waiting_us = voice->start_us = now;
	}
	snd.started++;
	return slot | ((long)voice->generation << 8);
}

long n64_voice_play(int file, float volume, float pan, float pitch, int priority, int loop)
{
	/* (the game's own refused while the files are tested alone) */
	if (snd.test)
		return -1;
	return voice_play(file, volume, pan, pitch, priority, loop);
}

void n64_voice_set(long handle, float volume, float pan, float pitch)
{
	int slot = voice_slot(handle);
	struct dc_voice *voice;
	uint32_t changes = 0;

	if (slot < 0)
		return;
	voice = &snd.voices[slot];
	voice->volume = volume;
	voice->pan = pan;
	voice->pitch = pitch;
	if (voice->state != VOICE_PLAYING)
		return;
	/* (only a change worth hearing: each is a command to the driver) */
	{
		int units = voice_volume_units(voice);

		if (units != voice->sent_volume && (units == 0 || units - voice->sent_volume > 2 || voice->sent_volume - units > 2))
		{
			voice->sent_volume = units;
			changes |= AICA_CH_UPDATE_SET_VOL;
		}
		units = voice_pan_units(voice);
		if (units != voice->sent_pan)
		{
			voice->sent_pan = units;
			changes |= AICA_CH_UPDATE_SET_PAN;
		}
		units = voice_frequency(voice);
		if (units != voice->sent_frequency && (units - voice->sent_frequency) * 300 / voice->sent_frequency != 0)
		{
			uint64_t now = timer_us_gettime64();

			/* (the samples played so far at the old rate kept) */
			voice->played = voice_played(voice, now);
			voice->since_us = now;
			voice->sent_frequency = units;
			changes |= AICA_CH_UPDATE_SET_FREQ;
		}
	}
	if (changes)
		aica_channel(slot, AICA_CH_CMD_UPDATE | changes, voice);
}

void n64_voice_stop(long handle)
{
	int slot = voice_slot(handle);

	if (slot >= 0)
		voice_stop(slot);
}

int n64_voice_playing(long handle)
{
	int slot = voice_slot(handle);

	if (slot < 0)
		return 0;
	voice_update(slot, timer_us_gettime64());
	return snd.voices[slot].state != VOICE_FREE;
}

void n64_voice_stop_all(void)
{
	int i;

	if (!snd.ready || !snd.files)
		return;
	for (i = 0; i < DC_VOICES; i++)
		voice_stop(i);
}

/* the pause menu's: the voices play on, unheard; a volume set meanwhile is
kept for after */
void n64_sound_pause(int paused)
{
	int i;

	paused = paused != 0;
	if (!snd.loaded || paused == snd.paused)
		return;
	snd.paused = paused;
	for (i = 0; i < DC_VOICES; i++)
	{
		struct dc_voice *voice = &snd.voices[i];

		if (voice->state != VOICE_PLAYING)
			continue;
		voice->sent_volume = voice_volume_units(voice);
		aica_channel(i, AICA_CH_CMD_UPDATE | AICA_CH_UPDATE_SET_VOL, voice);
	}
}

void n64_sound_statistics(unsigned long *started, unsigned long *stolen, unsigned long *refused)
{
	*started = snd.started;
	*stolen = snd.stolen;
	*refused = snd.refused;
}

/* the "dcs:" line (dc_render.c, every 120 frames) */
void dc_sound_statistics_line(void)
{
	char line[240];
	int playing = 0, loops = 0, waiting = 0, resident = 0, i;
	unsigned long used = 0, loads = snd.loads;

	if (!snd.loaded)
		return;
	for (i = 0; i < DC_VOICES; i++)
	{
		playing += snd.voices[i].state == VOICE_PLAYING;
		loops += snd.voices[i].state == VOICE_PLAYING && snd.voices[i].loop;
		waiting += snd.voices[i].state == VOICE_WAITING;
	}
	for (i = 0; i < snd.header.file_count; i++)
	{
		if (snd.files[i].state != FILE_ABSENT)
		{
			resident++;
			used += snd.files[i].bytes;
		}
	}
	/* (three lines: Flycast's serial console cuts a line at 120 characters) */
	snprintf(line, sizeof(line), "dcs: voices %d playing (%d loops), %d waiting; %lu started, %lu stolen, %lu refused",
		playing, loops, waiting, snd.started - snd.line_started, snd.stolen - snd.line_stolen,
		snd.refused - snd.line_refused);
	n64_log(line);
	snprintf(line, sizeof(line), "dcs: %lu as another take, %lu waited (worst %lu ms), %lu dropped; the AICA moving on %lu of %lu",
		snd.substituted, snd.waited, (unsigned long)(snd.waited_worst_us / 1000), snd.dropped, snd.aica_moving,
		snd.aica_moving + snd.aica_still);
	n64_log(line);
	snprintf(line, sizeof(line), "dcs: disc loads %lu (%lu KB, %lu ms average, %lu worst), %lu given up; %d files, %lu KB",
		loads, snd.load_bytes / 1024, loads ? (unsigned long)(snd.load_total_us / loads / 1000) : 0,
		(unsigned long)(snd.load_worst_us / 1000), snd.evicted, resident, used / 1024);
	n64_log(line);
	snd.line_started = snd.started;
	snd.line_stolen = snd.stolen;
	snd.line_refused = snd.refused;
	snd.substituted = snd.waited = snd.dropped = snd.evicted = 0;
	snd.aica_moving = snd.aica_still = 0;
	snd.waited_worst_us = 0;
	snd.loads = snd.load_bytes = 0;
	snd.load_total_us = snd.load_worst_us = 0;
}

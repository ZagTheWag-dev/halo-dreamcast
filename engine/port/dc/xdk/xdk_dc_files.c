/*
XDK_DC_FILES.C

The Xbox's writable drives in RAM, for Halo's own menus' saved games
(source/saved games/saved_game_files.c, player_profile.c,
playlist_profile.c): u:\ (the save games: player profiles, game types),
z:\ (the utility drive: the default profiles and game types Halo writes at
start, its map of the save games) and t:\. The files and directories live
in RAM until the Dreamcast is switched off: a profile made, a game type
edited, last for the session.

port/n64/src/xdk_kernel.c hands paths on these drives here (CreateFileA,
GetFileAttributesA, FindFirstFileA and the rest, HALO_DC) and keeps the
handles; this file keeps the entries, and the XAPI's save games: each a
directory u:\udata\4d530004\<12 hex digits> with its name (XCreateSaveGame's),
as the Xbox's are.
*/

#include "platform.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_ENTRIES 256
#define RAM_PATH 160
#define RAM_FINDS 8
#define SAVE_NAME 128
/* the title's save games' directory (Halo's title ID) */
#define SAVE_ROOT "u:\\udata\\4d530004"
/* what GetDiskFreeSpaceExA reports (saved_game_files.c wants 40 MB free) */
#define DISK_FREE (64ULL * 1024 * 1024)
#define DISK_TOTAL (128ULL * 1024 * 1024)

/* (Win32's, which the XDK headers here leave out) */
#ifndef ERROR_FILE_EXISTS
#define ERROR_FILE_EXISTS 80L
#endif
#ifndef ERROR_INVALID_NAME
#define ERROR_INVALID_NAME 123L
#endif

struct ram_entry
{
	char path[RAM_PATH]; /* lower case, backslashes, no trailing one */
	unsigned char used, directory, save_game;
	unsigned long attributes;
	unsigned char *data;
	unsigned long size, capacity;
	unsigned short save_name[SAVE_NAME];
};

struct ram_find
{
	int used, saves;
	char directory[RAM_PATH];
	long next;
};

static struct ram_entry ram[RAM_ENTRIES];
static struct ram_find finds[RAM_FINDS];
static unsigned long next_save_number = 1;

int dc_ram_owns(const char *path)
{
	int drive;

	if (!path || !path[0] || path[1] != ':')
		return 0;
	drive = tolower((unsigned char)path[0]);
	return drive == 'u' || drive == 'z' || drive == 't';
}

/* "Z:\Saved\\x\" -> "z:\saved\x" */
static void normalize(const char *in, char *out)
{
	size_t length = 0;

	for (; *in && length < RAM_PATH - 1; in++)
	{
		char c = *in == '/' ? '\\' : (char)tolower((unsigned char)*in);

		if (c == '\\' && length && out[length - 1] == '\\')
			continue;
		out[length++] = c;
	}
	while (length > 2 && out[length - 1] == '\\')
		length--;
	out[length] = 0;
}

static long find_entry(const char *normalized)
{
	long i;

	for (i = 0; i < RAM_ENTRIES; i++)
	{
		if (ram[i].used && !strcmp(ram[i].path, normalized))
			return i;
	}
	return -1;
}

/* the drive's root ("z:"), or a directory made */
static int is_directory(const char *normalized)
{
	long i;

	if (strlen(normalized) == 2)
		return 1;
	i = find_entry(normalized);
	return i >= 0 && ram[i].directory;
}

/* the directory a path is in ("z:\saved\x" -> "z:\saved") */
static void parent_of(const char *normalized, char *parent)
{
	const char *slash = strrchr(normalized, '\\');
	size_t length = slash ? (size_t)(slash - normalized) : 0;

	memcpy(parent, normalized, length);
	parent[length] = 0;
}

/* an entry made (its directories too, as needed); -1 if none is free */
static long make_entry(const char *normalized, int directory)
{
	char parent[RAM_PATH];
	long i;

	parent_of(normalized, parent);
	if (strlen(parent) > 2 && !is_directory(parent) && make_entry(parent, 1) < 0)
		return -1;
	for (i = 0; i < RAM_ENTRIES; i++)
	{
		if (!ram[i].used)
		{
			memset(&ram[i], 0, sizeof(ram[i]));
			strcpy(ram[i].path, normalized);
			ram[i].used = 1;
			ram[i].directory = (unsigned char)directory;
			ram[i].attributes = directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
			return i;
		}
	}
	platform_log("ram drive: no room for %s", normalized);
	return -1;
}

static void free_entry(long i)
{
	free(ram[i].data);
	memset(&ram[i], 0, sizeof(ram[i]));
}

/* whether anything is in a directory */
static int has_children(const char *normalized)
{
	size_t length = strlen(normalized);
	long i;

	for (i = 0; i < RAM_ENTRIES; i++)
	{
		if (ram[i].used && !strncmp(ram[i].path, normalized, length) && ram[i].path[length] == '\\')
			return 1;
	}
	return 0;
}

/* ---------- xdk_kernel.c's files */

/* an entry opened (CreateFileA's dispositions); -1 and the error if not */
long dc_ram_open(const char *path, unsigned long access, unsigned long disposition, unsigned long *error)
{
	char normalized[RAM_PATH];
	long i;

	normalize(path, normalized);
	i = find_entry(normalized);
	*error = ERROR_SUCCESS;
	if (i >= 0 && ram[i].directory)
	{
		*error = ERROR_ACCESS_DENIED;
		return -1;
	}
	switch (disposition)
	{
	case CREATE_NEW:
		if (i >= 0)
		{
			*error = ERROR_FILE_EXISTS;
			return -1;
		}
		break;
	case OPEN_EXISTING:
	case TRUNCATE_EXISTING:
		if (i < 0)
		{
			*error = ERROR_FILE_NOT_FOUND;
			return -1;
		}
		break;
	}
	if (i < 0)
	{
		i = make_entry(normalized, 0);
		if (i < 0)
		{
			*error = ERROR_DISK_FULL;
			return -1;
		}
	}
	else if (disposition == CREATE_ALWAYS || disposition == TRUNCATE_EXISTING)
	{
		if (ram[i].attributes & FILE_ATTRIBUTE_READONLY)
		{
			*error = ERROR_ACCESS_DENIED;
			return -1;
		}
		ram[i].size = 0;
	}
	(void)access;
	return i;
}

unsigned long dc_ram_size(long i)
{
	return i >= 0 && i < RAM_ENTRIES && ram[i].used ? ram[i].size : 0;
}

long dc_ram_read(long i, unsigned long position, void *buffer, unsigned long size)
{
	if (i < 0 || i >= RAM_ENTRIES || !ram[i].used || position >= ram[i].size)
		return 0;
	if (size > ram[i].size - position)
		size = ram[i].size - position;
	memcpy(buffer, ram[i].data + position, size);
	return (long)size;
}

int dc_ram_set_size(long i, unsigned long size)
{
	if (i < 0 || i >= RAM_ENTRIES || !ram[i].used)
		return 0;
	if (size > ram[i].capacity)
	{
		unsigned long capacity = (size + 511) & ~511UL;
		unsigned char *data = realloc(ram[i].data, capacity);

		if (!data)
			return 0;
		memset(data + ram[i].capacity, 0, capacity - ram[i].capacity);
		ram[i].data = data;
		ram[i].capacity = capacity;
	}
	if (size > ram[i].size)
		memset(ram[i].data + ram[i].size, 0, size - ram[i].size);
	ram[i].size = size;
	return 1;
}

long dc_ram_write(long i, unsigned long position, const void *buffer, unsigned long size)
{
	if (position + size > dc_ram_size(i) && !dc_ram_set_size(i, position + size))
		return -1;
	memcpy(ram[i].data + position, buffer, size);
	return (long)size;
}

unsigned long dc_ram_attributes(const char *path)
{
	char normalized[RAM_PATH];
	long i;

	normalize(path, normalized);
	if (strlen(normalized) == 2)
		return FILE_ATTRIBUTE_DIRECTORY;
	i = find_entry(normalized);
	return i >= 0 ? ram[i].attributes : 0xFFFFFFFF;
}

/* a directory's entries matching "<directory>\*" or "<directory>\*.*" (any
pattern is taken as every entry); the find's number, or -1 */
static void fill_find_data(const struct ram_entry *e, struct _WIN32_FIND_DATAA *data)
{
	const char *slash = strrchr(e->path, '\\');

	memset(data, 0, sizeof(*data));
	data->dwFileAttributes = e->attributes;
	data->nFileSizeLow = e->size;
	strncpy(data->cFileName, slash ? slash + 1 : e->path, sizeof(data->cFileName) - 1);
}

static long next_child(struct ram_find *f, const struct ram_entry **out)
{
	size_t length = strlen(f->directory);

	for (; f->next < RAM_ENTRIES; f->next++)
	{
		const struct ram_entry *e = &ram[f->next];

		if (e->used && !strncmp(e->path, f->directory, length) && e->path[length] == '\\' &&
			!strchr(e->path + length + 1, '\\') && (!f->saves || e->save_game))
		{
			*out = e;
			return f->next++;
		}
	}
	return -1;
}

long dc_ram_find_first(const char *pattern, struct _WIN32_FIND_DATAA *data)
{
	char normalized[RAM_PATH], directory[RAM_PATH];
	const struct ram_entry *e;
	long slot;

	normalize(pattern, normalized);
	parent_of(normalized, directory);
	for (slot = 0; slot < RAM_FINDS && finds[slot].used; slot++)
		;
	if (slot == RAM_FINDS || !is_directory(directory))
		return -1;
	memset(&finds[slot], 0, sizeof(finds[slot]));
	finds[slot].used = 1;
	strcpy(finds[slot].directory, directory);
	if (next_child(&finds[slot], &e) < 0)
	{
		finds[slot].used = 0;
		return -1;
	}
	fill_find_data(e, data);
	return slot;
}

int dc_ram_find_next(long slot, struct _WIN32_FIND_DATAA *data)
{
	const struct ram_entry *e;

	if (slot < 0 || slot >= RAM_FINDS || !finds[slot].used || next_child(&finds[slot], &e) < 0)
		return 0;
	fill_find_data(e, data);
	return 1;
}

void dc_ram_find_close(long slot)
{
	if (slot >= 0 && slot < RAM_FINDS)
		finds[slot].used = 0;
}

/* the XAPI's save games, one by one (XFindFirstSaveGame) */
static void fill_save_data(const struct ram_entry *e, struct _XGAME_FIND_DATA *data)
{
	memset(data, 0, sizeof(*data));
	fill_find_data(e, &data->wfd);
	snprintf(data->szSaveGameDirectory, sizeof(data->szSaveGameDirectory), "%s\\", e->path);
	memcpy(data->szSaveGameName, e->save_name, sizeof(data->szSaveGameName));
}

long dc_ram_save_find_first(struct _XGAME_FIND_DATA *data)
{
	const struct ram_entry *e;
	long slot;

	for (slot = 0; slot < RAM_FINDS && finds[slot].used; slot++)
		;
	if (slot == RAM_FINDS)
		return -1;
	memset(&finds[slot], 0, sizeof(finds[slot]));
	finds[slot].used = 1;
	finds[slot].saves = 1;
	strcpy(finds[slot].directory, SAVE_ROOT);
	if (next_child(&finds[slot], &e) < 0)
	{
		finds[slot].used = 0;
		return -1;
	}
	fill_save_data(e, data);
	return slot;
}

int dc_ram_save_find_next(long slot, struct _XGAME_FIND_DATA *data)
{
	const struct ram_entry *e;

	if (slot < 0 || slot >= RAM_FINDS || !finds[slot].used || next_child(&finds[slot], &e) < 0)
		return 0;
	fill_save_data(e, data);
	return 1;
}

/* ---------- the Win32 calls on paths */

int __stdcall CreateDirectoryA(const char *path, void *security)
{
	char normalized[RAM_PATH];

	(void)security;
	if (!dc_ram_owns(path))
	{
		SetLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	normalize(path, normalized);
	if (find_entry(normalized) >= 0 || strlen(normalized) == 2)
	{
		SetLastError(ERROR_ALREADY_EXISTS);
		return FALSE;
	}
	if (make_entry(normalized, 1) < 0)
	{
		SetLastError(ERROR_DISK_FULL);
		return FALSE;
	}
	return TRUE;
}

int __stdcall RemoveDirectoryA(const char *path)
{
	char normalized[RAM_PATH];
	long i;

	if (!dc_ram_owns(path))
		return FALSE;
	normalize(path, normalized);
	i = find_entry(normalized);
	if (i < 0 || !ram[i].directory)
	{
		SetLastError(ERROR_PATH_NOT_FOUND);
		return FALSE;
	}
	if (has_children(normalized))
	{
		SetLastError(ERROR_DIR_NOT_EMPTY);
		return FALSE;
	}
	free_entry(i);
	return TRUE;
}

int __stdcall DeleteFileA(const char *path)
{
	char normalized[RAM_PATH];
	long i;

	if (!dc_ram_owns(path))
		return FALSE;
	normalize(path, normalized);
	i = find_entry(normalized);
	if (i < 0 || ram[i].directory)
	{
		SetLastError(ERROR_FILE_NOT_FOUND);
		return FALSE;
	}
	free_entry(i);
	return TRUE;
}

int __stdcall CopyFileA(const char *existing, const char *destination, int fail_if_exists)
{
	char from[RAM_PATH], to[RAM_PATH];
	long source, target;

	if (!dc_ram_owns(existing) || !dc_ram_owns(destination))
		return FALSE;
	normalize(existing, from);
	normalize(destination, to);
	source = find_entry(from);
	if (source < 0 || ram[source].directory)
	{
		SetLastError(ERROR_FILE_NOT_FOUND);
		return FALSE;
	}
	target = find_entry(to);
	if (target >= 0 && fail_if_exists)
	{
		SetLastError(ERROR_FILE_EXISTS);
		return FALSE;
	}
	if (target < 0 && (target = make_entry(to, 0)) < 0)
		return FALSE;
	if (!dc_ram_set_size(target, ram[source].size))
		return FALSE;
	memcpy(ram[target].data, ram[source].data, ram[source].size);
	return TRUE;
}

int __stdcall MoveFileA(const char *existing, const char *destination)
{
	char from[RAM_PATH], to[RAM_PATH];
	long source;

	if (!dc_ram_owns(existing) || !dc_ram_owns(destination))
		return FALSE;
	normalize(existing, from);
	normalize(destination, to);
	source = find_entry(from);
	if (source < 0 || find_entry(to) >= 0)
		return FALSE;
	strcpy(ram[source].path, to);
	return TRUE;
}

int __stdcall SetFileAttributesA(const char *path, unsigned long attributes)
{
	char normalized[RAM_PATH];
	long i;

	if (!dc_ram_owns(path))
		return FALSE;
	normalize(path, normalized);
	i = find_entry(normalized);
	if (i < 0)
		return FALSE;
	ram[i].attributes = (ram[i].directory ? FILE_ATTRIBUTE_DIRECTORY : 0) |
		(attributes & ~FILE_ATTRIBUTE_DIRECTORY);
	if (!ram[i].attributes)
		ram[i].attributes = FILE_ATTRIBUTE_NORMAL;
	return TRUE;
}

int __stdcall GetFileAttributesExA(const char *path, enum _GET_FILEEX_INFO_LEVELS level, void *information)
{
	struct _WIN32_FILE_ATTRIBUTE_DATA *data = information;
	char normalized[RAM_PATH];
	long i;

	(void)level;
	memset(data, 0, sizeof(*data));
	if (!dc_ram_owns(path))
	{
		/* (the disc's files: their size) */
		void *file = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);

		if (file == INVALID_HANDLE_VALUE)
			return FALSE;
		data->dwFileAttributes = FILE_ATTRIBUTE_READONLY;
		data->nFileSizeLow = GetFileSize(file, NULL);
		CloseHandle(file);
		return TRUE;
	}
	normalize(path, normalized);
	i = find_entry(normalized);
	if (i < 0)
	{
		if (strlen(normalized) == 2)
		{
			data->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
			return TRUE;
		}
		SetLastError(ERROR_FILE_NOT_FOUND);
		return FALSE;
	}
	data->dwFileAttributes = ram[i].attributes;
	data->nFileSizeLow = ram[i].size;
	return TRUE;
}

int __stdcall GetDiskFreeSpaceExA(const char *path, union _ULARGE_INTEGER *available, union _ULARGE_INTEGER *total,
	union _ULARGE_INTEGER *free_bytes)
{
	(void)path;
	if (available)
		available->QuadPart = DISK_FREE;
	if (total)
		total->QuadPart = DISK_TOTAL;
	if (free_bytes)
		free_bytes->QuadPart = DISK_FREE;
	return TRUE;
}

/* ---------- the XAPI */

unsigned long __stdcall XGetLanguage(void)
{
	/* (XC_LANGUAGE_ENGLISH) */
	return 1;
}

static long save_by_name(const unsigned short *name)
{
	size_t length = strlen(SAVE_ROOT);
	long i;

	for (i = 0; i < RAM_ENTRIES; i++)
	{
		if (ram[i].used && ram[i].save_game && !strncmp(ram[i].path, SAVE_ROOT, length) && name)
		{
			int k;

			for (k = 0; k < SAVE_NAME && ram[i].save_name[k] == name[k] && name[k]; k++)
				;
			if (k < SAVE_NAME && ram[i].save_name[k] == name[k])
				return i;
		}
	}
	return -1;
}

unsigned long __stdcall XCreateSaveGame(const char *root, const unsigned short *name, unsigned long disposition,
	unsigned long flags, char *path, unsigned int size)
{
	char directory[RAM_PATH];
	long i;

	(void)root;
	(void)flags;
	if (!name || !name[0])
		return ERROR_INVALID_NAME;
	i = save_by_name(name);
	if (i >= 0)
	{
		if (disposition == CREATE_NEW)
			return ERROR_ALREADY_EXISTS;
	}
	else
	{
		int k;

		if (disposition == OPEN_EXISTING)
			return ERROR_PATH_NOT_FOUND;
		snprintf(directory, sizeof(directory), "%s\\%012lx", SAVE_ROOT, next_save_number++);
		i = make_entry(directory, 1);
		if (i < 0)
			return ERROR_DISK_FULL;
		ram[i].save_game = 1;
		for (k = 0; k < SAVE_NAME - 1 && name[k]; k++)
			ram[i].save_name[k] = name[k];
	}
	if (path && size)
		snprintf(path, size, "%s\\", ram[i].path);
	return ERROR_SUCCESS;
}

unsigned long __stdcall XDeleteSaveGame(const char *root, const unsigned short *name)
{
	size_t length;
	long i, k;

	(void)root;
	i = save_by_name(name);
	if (i < 0)
		return ERROR_PATH_NOT_FOUND;
	length = strlen(ram[i].path);
	for (k = 0; k < RAM_ENTRIES; k++)
	{
		if (k != i && ram[k].used && !strncmp(ram[k].path, ram[i].path, length) && ram[k].path[length] == '\\')
			free_entry(k);
	}
	free_entry(i);
	return ERROR_SUCCESS;
}

/* a checksum in the signature's place (FNV-1a, five lanes): the same data
signs the same, which is all saved_game_files.c asks */
struct dc_signature
{
	unsigned long lanes[5];
	int used;
};

static struct dc_signature signatures[4];

void *__stdcall XCalculateSignatureBegin(unsigned long flags)
{
	int i, k;

	(void)flags;
	for (i = 0; i < 4 && signatures[i].used; i++)
		;
	if (i == 4)
		i = 0;
	signatures[i].used = 1;
	for (k = 0; k < 5; k++)
		signatures[i].lanes[k] = 2166136261UL + (unsigned long)k * 16777619UL;
	return &signatures[i];
}

unsigned long __stdcall XCalculateSignatureUpdate(void *context, const unsigned char *data, unsigned long size)
{
	struct dc_signature *s = context;
	unsigned long i;

	if (!s)
		return ERROR_INVALID_PARAMETER;
	for (i = 0; i < size; i++)
	{
		unsigned long *lane = &s->lanes[i % 5];

		*lane = (*lane ^ data[i]) * 16777619UL;
	}
	return ERROR_SUCCESS;
}

unsigned long __stdcall XCalculateSignatureEnd(void *context, struct _XCALCSIG_SIGNATURE *signature)
{
	struct dc_signature *s = context;
	int k;

	if (!s)
		return ERROR_INVALID_PARAMETER;
	for (k = 0; k < 5; k++)
	{
		signature->Signature[k * 4 + 0] = (unsigned char)s->lanes[k];
		signature->Signature[k * 4 + 1] = (unsigned char)(s->lanes[k] >> 8);
		signature->Signature[k * 4 + 2] = (unsigned char)(s->lanes[k] >> 16);
		signature->Signature[k * 4 + 3] = (unsigned char)(s->lanes[k] >> 24);
	}
	s->used = 0;
	return ERROR_SUCCESS;
}

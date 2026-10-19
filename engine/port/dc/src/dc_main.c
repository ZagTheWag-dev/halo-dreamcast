/*
DC_MAIN.C

The Dreamcast build's entry point and system services (the n64.h API of
port/n64/src/n64_main.c, over KallistiOS): start-up, the log (on the screen
through KallistiOS's framebuffer console, and on the serial port), time, the
heap, and the jump into the game's own main (source/shell/shell_xbox.c,
compiled as halo_shell_main) on a stack of its own.
*/

#include <kos.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "n64.h"

int halo_shell_main(void);

/* the game wants about 97 KB of stack (n64_main.c gives it 192 KB) */
#define HALO_STACK_SIZE (256 * 1024)

extern dbgio_handler_t dbgio_fb;

/* the log on the screen until the PowerVR takes the framebuffer
(dc_render.c n64_render_initialize); the last lines kept, for a fatal
error's screen */
int dc_screen_log = 1;
#define DC_RING_LINES 30
static char dc_ring[DC_RING_LINES][128];
static int dc_ring_next;

/* The serial port is slow: 115200 baud, about 11 characters a millisecond,
and KallistiOS waits on its FIFO for each. The game's statistics every 5 s
(n64_game.c: about 1.5 KB) held a frame up by 100 ms, the "dcf:" line's 5+
refreshes. The lines go into a queue instead, written out by a thread of its
own while the game is idle (waiting for the display's refresh, mostly:
dc_pace.c), a few characters at a time, each time looking whether the game
is still idle. (Not by priority alone: KallistiOS's scheduler tick gives the
CPU to any thread that is ready, of whatever priority, for a whole 10 ms.)
A full queue is written out there and then (nothing lost); a fatal error
writes out the rest first. */
static kthread_t *dc_game_thread;

#define DC_SERIAL_QUEUE (16 * 1024)
#define DC_SERIAL_CHUNK 8
static char dc_serial_queue[DC_SERIAL_QUEUE];
static volatile unsigned dc_serial_head, dc_serial_tail;
static semaphore_t dc_serial_ready = SEM_INITIALIZER(0);
static mutex_t dc_serial_lock = MUTEX_INITIALIZER;
static kthread_t *dc_serial_thread;
/* the game asleep (dc_idle_begin, dc_idle_end) */
static volatile int dc_game_idle;

/* the queue out to the port, in order (one writer at a time); while_idle:
only while the game is idle */
static void dc_serial_drain(int while_idle)
{
	char chunk[DC_SERIAL_CHUNK];
	unsigned count;

	mutex_lock(&dc_serial_lock);
	do
	{
		int flags;

		if (while_idle && !dc_game_idle)
			break;
		flags = irq_disable();
		count = 0;
		while (count < sizeof(chunk) && dc_serial_tail != dc_serial_head)
			chunk[count++] = dc_serial_queue[dc_serial_tail++ % DC_SERIAL_QUEUE];
		irq_restore(flags);
		if (count)
			dbgio_write_buffer_xlat((const uint8 *)chunk, count);
	} while (count);
	mutex_unlock(&dc_serial_lock);
}

static void *dc_serial_writer(void *argument)
{
	(void)argument;
	for (;;)
	{
		sem_wait(&dc_serial_ready);
		dc_serial_drain(1);
	}
	return NULL;
}

static void dc_serial_write(const char *text, int count)
{
	int flags;

	if (!dc_serial_thread || irq_inside_int())
	{
		dbgio_write_buffer_xlat((const uint8 *)text, count);
		return;
	}
	if (dc_serial_head - dc_serial_tail + count > DC_SERIAL_QUEUE)
		dc_serial_drain(0);
	flags = irq_disable();
	while (count-- > 0)
		dc_serial_queue[dc_serial_head++ % DC_SERIAL_QUEUE] = *text++;
	irq_restore(flags);
}

/* the game going to sleep (n64_wait_microseconds, the wait for the
refresh): the queue's writer woken if there is anything to write */
void dc_idle_begin(void)
{
	if (thd_get_current() != dc_game_thread)
		return;
	dc_game_idle = 1;
	if (dc_serial_thread && dc_serial_tail != dc_serial_head && sem_count(&dc_serial_ready) <= 0)
		sem_signal(&dc_serial_ready);
}

void dc_idle_end(void)
{
	if (thd_get_current() == dc_game_thread)
		dc_game_idle = 0;
}

/* a line on the screen (KallistiOS's framebuffer console, written to
directly) and on the serial port (the debug output KallistiOS starts with,
left selected: Flycast shows it in its "Flycast Serial Output" console) */
static void dc_line(const char *prefix, const char *text, size_t length)
{
	char line[512];
	int count = snprintf(line, sizeof(line), "%s%.*s\n", prefix, (int)length, text);

	if (count < 0)
		return;
	if (count >= (int)sizeof(line))
	{
		count = sizeof(line) - 1;
		line[count - 1] = '\n';
	}
	if (dc_screen_log)
		dbgio_fb.write_buffer((const uint8 *)line, count, 1);
	dc_serial_write(line, count);
	snprintf(dc_ring[dc_ring_next++ % DC_RING_LINES], sizeof(dc_ring[0]), "%s", line);
}

void n64_log(const char *text)
{
	size_t length = strlen(text);

	while (length && (text[length - 1] == '\n' || text[length - 1] == '\r'))
		length--;
	dc_line("", text, length);
}

void n64_fatal(const char *text)
{
	extern int dc_pvr_up;

	/* the log back on the screen: the PowerVR shut down, the last lines
	drawn again */
	if (dc_pvr_up && !irq_inside_int())
	{
		int i;

		pvr_shutdown();
		dc_pvr_up = 0;
		vid_set_mode(DM_640x480, PM_RGB565);
		dbgio_fb.init();
		dc_screen_log = 1;
		for (i = 0; i < DC_RING_LINES; i++)
		{
			const char *line = dc_ring[(dc_ring_next + i) % DC_RING_LINES];

			if (*line)
				dbgio_fb.write_buffer((const uint8 *)line, strlen(line), 1);
		}
	}
	if (dc_serial_thread && !irq_inside_int())
		dc_serial_drain(0);
	dc_serial_thread = NULL;
	dc_line("FATAL: ", text, strlen(text));
	/* (the screen keeps the log: nothing returns from here) */
	for (;;)
		thd_sleep(1000);
}

void n64_backtrace(void)
{
}

/* the game's clock runs at the N64's 46.875 MHz count (N64_TICKS_PER_SECOND) */
unsigned long long n64_ticks64(void)
{
	return timer_us_gettime64() * 46875ULL / 1000ULL;
}

unsigned long n64_ticks(void)
{
	return (unsigned long)n64_ticks64();
}

unsigned long long n64_microseconds(void)
{
	return timer_us_gettime64();
}

/* to the microsecond: asleep for the part KallistiOS can time (thd_sleep
wakes a thread on the scheduler's 10 ms tick, HZ 100, so up to a tick
late), the CPU free for the other threads, and the last 10 ms or less
spun on the microsecond clock. (Halo's Sleep, port/n64/src/xdk_kernel.c;
the frames are paced by the display instead now: dc_pace.c) */
void n64_wait_microseconds(unsigned long microseconds)
{
	extern unsigned long long dc_idle_us;
	unsigned long long start = timer_us_gettime64();
	unsigned long long end = start + microseconds;

	if (microseconds > 11000)
	{
		dc_idle_begin();
		thd_sleep((microseconds - 11000) / 1000);
		dc_idle_end();
	}
	while (timer_us_gettime64() < end)
		;
	/* (the game's waits, for the "dcf:" line) */
	if (thd_get_current() == dc_game_thread)
		dc_idle_us += timer_us_gettime64() - start;
}

/* what malloc has free, and the memory it has not taken from the system yet
(between the heap's end and the top of the 16 MB, less the room KallistiOS
keeps for the stacks) */
unsigned long n64_heap_free(void)
{
	struct mallinfo info = mallinfo();
	unsigned long end = (unsigned long)sbrk(0);
	unsigned long top = (unsigned long)_arch_mem_top - 64 * 1024;

	return (unsigned long)info.fordblks + (top > end ? top - end : 0);
}

static void *volatile heap_probe;

int n64_heap_has(unsigned long size)
{
	heap_probe = malloc(size);
	if (!heap_probe)
		return 0;
	free(heap_probe);
	heap_probe = NULL;
	return 1;
}

unsigned long n64_heap_describe(char *text, unsigned long size)
{
	if (size)
		snprintf(text, size, "%lu KB free", n64_heap_free() / 1024);
	return n64_heap_free();
}

void n64_heap_dump(const char *tag, const void *start)
{
	(void)tag;
	(void)start;
}

static void *halo_thread(void *argument)
{
	int result;

	(void)argument;
	result = halo_shell_main();
	printf("halo_shell_main returned %d\n", result);
	return NULL;
}

int main(int argc, char **argv)
{
	kthread_attr_t attributes;
	kthread_t *thread;

	(void)argc;
	(void)argv;
	/* unaligned loads and stores emulated (a real Dreamcast traps them:
	dc_exceptions.c) */
	{
		extern void dc_exceptions_install(void);

		dc_exceptions_install();
	}
	vid_set_mode(DM_640x480, PM_RGB565);
	dbgio_fb.init();
	n64_log("HALO: COMBAT EVOLVED - DREAMCAST");
	{
		char line[64];

		snprintf(line, sizeof(line), "SH-4 build, %lu KB heap free", n64_heap_free() / 1024);
		n64_log(line);
	}
	/* testing setting "autotest" (dc_files.c dc_settings): the N64's scripted
	controllers (walk, turn, fire, jump; port/n64/src/xdk_input.c) on every
	port; otherwise the Dreamcast's own (dc_input.c) */
	{
		extern int n64_test_input;

		n64_test_input = n64_debug_flag("autotest");
	}

	/* the serial port's writer, below the game (PRIO_DEFAULT) */
	memset(&attributes, 0, sizeof(attributes));
	attributes.stack_size = 8 * 1024;
	attributes.prio = PRIO_DEFAULT + 5;
	attributes.label = "serial log";
	dc_serial_thread = thd_create_ex(&attributes, dc_serial_writer, NULL);

	memset(&attributes, 0, sizeof(attributes));
	attributes.stack_size = HALO_STACK_SIZE;
	attributes.label = "halo";
	thread = thd_create_ex(&attributes, halo_thread, NULL);
	if (!thread)
		n64_fatal("no thread for the game");
	dc_game_thread = thread;
	thd_join(thread, NULL);
	for (;;)
		thd_sleep(1000);
	return 0;
}

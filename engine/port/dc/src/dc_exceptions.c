/*
DC_EXCEPTIONS.C

Where the SH-4 and the Xbox's x86 disagree about what is an error, the game
gets the x86's answer (port/n64/src/n64_exceptions.c's, on the N64): a load
or store not aligned to its size raises an address error on the SH-4 and
works on x86, and the game has a few (byte buffers read as longs). The
access is emulated with byte loads and stores, and the program goes on
after it.

Flycast does not raise these (it reads an unaligned long as it is: a test
showed it), so only a real Dreamcast comes here. The forms handled: MOV.W
and MOV.L, loads and stores, by register, post-increment, pre-decrement,
displacement, R0 index and GBR displacement, and FMOV.S by register,
post-increment, pre-decrement and R0 index (single precision); an access in
a branch's delay slot (the exception's PC the branch's) is emulated, then
the branch. Each place is logged once (the serial port) so that hot ones
can be fixed in the source; anything else goes to KallistiOS's own
handler (its register dump).

The FPU's exceptions stay disabled, as KallistiOS starts it (FPSCR's DN
on: denormals are zero, NaNs flow on quietly): the game makes NaNs as x86
lets it and expects them to.
*/

#include <kos.h>
#include <arch/irq.h>
#include <stdio.h>

#define MAXIMUM_REPORTED 32
#define FPSCR_FR (1u << 21)
#define FPSCR_SZ (1u << 20)
#define SR_T 1u

static uint32_t reported[MAXIMUM_REPORTED];
static int reported_count;
unsigned long dc_unaligned_count;

static uint32_t load(uint32_t address, int size)
{
	const volatile uint8_t *p = (const volatile uint8_t *)address;

	if (size == 2)
		return (uint32_t)(int32_t)(int16_t)(p[0] | p[1] << 8);
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void store(uint32_t address, uint32_t value, int size)
{
	volatile uint8_t *p = (volatile uint8_t *)address;

	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	if (size == 4)
	{
		p[2] = (uint8_t)(value >> 16);
		p[3] = (uint8_t)(value >> 24);
	}
}

/* the visible FR register n (FPSCR's FR picks the bank) */
static uint32_t *fr(irq_context_t *c, int n)
{
	return (c->fpscr & FPSCR_FR) ? &c->frbank[n] : &c->fr[n];
}

/* one memory instruction emulated; 0 if it is not one of the forms */
static int emulate(irq_context_t *c, uint16_t op)
{
	uint32_t *r = c->r;
	int n = (op >> 8) & 15, m = (op >> 4) & 15, d = op & 15, d8 = op & 255;
	int size;

	switch (op >> 12)
	{
	case 0x0:
		switch (op & 15)
		{
		case 0xD: r[n] = load(r[0] + r[m], 2); return 1;          /* mov.w @(r0,rm),rn */
		case 0xE: r[n] = load(r[0] + r[m], 4); return 1;          /* mov.l @(r0,rm),rn */
		case 0x5: store(r[0] + r[n], r[m], 2); return 1;          /* mov.w rm,@(r0,rn) */
		case 0x6: store(r[0] + r[n], r[m], 4); return 1;          /* mov.l rm,@(r0,rn) */
		}
		return 0;
	case 0x1: store(r[n] + d * 4, r[m], 4); return 1;              /* mov.l rm,@(disp,rn) */
	case 0x2:
		switch (op & 15)
		{
		case 0x1: store(r[n], r[m], 2); return 1;                  /* mov.w rm,@rn */
		case 0x2: store(r[n], r[m], 4); return 1;                  /* mov.l rm,@rn */
		case 0x5: case 0x6:                                        /* mov.w/l rm,@-rn */
			size = (op & 15) == 0x5 ? 2 : 4;
			store(r[n] - size, r[m], size);
			r[n] -= size;
			return 1;
		}
		return 0;
	case 0x5: r[n] = load(r[m] + d * 4, 4); return 1;              /* mov.l @(disp,rm),rn */
	case 0x6:
		switch (op & 15)
		{
		case 0x1: r[n] = load(r[m], 2); return 1;                  /* mov.w @rm,rn */
		case 0x2: r[n] = load(r[m], 4); return 1;                  /* mov.l @rm,rn */
		case 0x5: case 0x6:                                        /* mov.w/l @rm+,rn */
		{
			uint32_t address = r[m];

			size = (op & 15) == 0x5 ? 2 : 4;
			if (n != m)
				r[m] += size;
			r[n] = load(address, size);
			return 1;
		}
		}
		return 0;
	case 0x8:
		switch ((op >> 8) & 15)
		{
		case 0x1: store(r[m] + d * 2, r[0], 2); return 1;          /* mov.w r0,@(disp,rn) (rn in m's bits) */
		case 0x5: r[0] = load(r[m] + d * 2, 2); return 1;          /* mov.w @(disp,rm),r0 */
		}
		return 0;
	case 0xC:
		switch ((op >> 8) & 15)
		{
		case 0x1: store(c->gbr + d8 * 2, r[0], 2); return 1;       /* mov.w r0,@(disp,gbr) */
		case 0x2: store(c->gbr + d8 * 4, r[0], 4); return 1;       /* mov.l r0,@(disp,gbr) */
		case 0x5: r[0] = load(c->gbr + d8 * 2, 2); return 1;       /* mov.w @(disp,gbr),r0 */
		case 0x6: r[0] = load(c->gbr + d8 * 4, 4); return 1;       /* mov.l @(disp,gbr),r0 */
		}
		return 0;
	case 0xF:
		if (c->fpscr & FPSCR_SZ)
			return 0;
		switch (op & 15)
		{
		case 0x8: *fr(c, n) = load(r[m], 4); return 1;             /* fmov.s @rm,frn */
		case 0x6: *fr(c, n) = load(r[0] + r[m], 4); return 1;      /* fmov.s @(r0,rm),frn */
		case 0x9: *fr(c, n) = load(r[m], 4); r[m] += 4; return 1;  /* fmov.s @rm+,frn */
		case 0xA: store(r[n], *fr(c, m), 4); return 1;             /* fmov.s frm,@rn */
		case 0xB: store(r[n] - 4, *fr(c, m), 4); r[n] -= 4; return 1; /* fmov.s frm,@-rn */
		case 0x7: store(r[0] + r[n], *fr(c, m), 4); return 1;      /* fmov.s frm,@(r0,rn) */
		}
		return 0;
	}
	return 0;
}

/* whether op is a branch with a delay slot, and where it goes (from the
registers before the slot runs) */
static int delayed_branch(irq_context_t *c, uint16_t op, uint32_t pc, uint32_t *target, int *link)
{
	int n = (op >> 8) & 15;
	int32_t d12 = (int32_t)((op & 0xFFF) << 20) >> 20, d8 = (int8_t)(op & 255);

	*link = 0;
	if ((op & 0xF000) == 0xA000) { *target = pc + 4 + d12 * 2; return 1; }              /* bra */
	if ((op & 0xF000) == 0xB000) { *target = pc + 4 + d12 * 2; *link = 1; return 1; }   /* bsr */
	if ((op & 0xFF00) == 0x8D00)                                                        /* bt/s */
	{
		*target = (c->sr & SR_T) ? pc + 4 + d8 * 2 : pc + 4;
		return 1;
	}
	if ((op & 0xFF00) == 0x8F00)                                                        /* bf/s */
	{
		*target = (c->sr & SR_T) ? pc + 4 : pc + 4 + d8 * 2;
		return 1;
	}
	if ((op & 0xF0FF) == 0x0023) { *target = pc + 4 + c->r[n]; return 1; }             /* braf */
	if ((op & 0xF0FF) == 0x0003) { *target = pc + 4 + c->r[n]; *link = 1; return 1; }  /* bsrf */
	if ((op & 0xF0FF) == 0x402B) { *target = c->r[n]; return 1; }                      /* jmp */
	if ((op & 0xF0FF) == 0x400B) { *target = c->r[n]; *link = 1; return 1; }           /* jsr */
	if (op == 0x000B) { *target = c->pr; return 1; }                                    /* rts */
	return 0;
}

static void dc_address_error(irq_t code, irq_context_t *c, void *data)
{
	uint32_t pc = c->pc, target;
	uint16_t op = *(const uint16_t *)pc;
	int link, i;

	(void)data;
	if (delayed_branch(c, op, pc, &target, &link))
	{
		if (!emulate(c, *(const uint16_t *)(pc + 2)))
			goto unhandled;
		if (link)
			c->pr = pc + 4;
		c->pc = target;
	}
	else
	{
		if (!emulate(c, op))
			goto unhandled;
		c->pc = pc + 2;
	}
	dc_unaligned_count++;
	for (i = 0; i < reported_count && reported[i] != pc; i++)
		;
	if (i == reported_count && reported_count < MAXIMUM_REPORTED)
	{
		reported[reported_count++] = pc;
		dbglog(DBG_WARNING, "dce: unaligned %s emulated at %08lx (op %04x)\n",
			code == EXC_DATA_ADDRESS_WRITE ? "store" : "load", (unsigned long)pc, op);
	}
	return;
unhandled:
	/* (KallistiOS's own: the register dump, the stop) */
	irq_set_handler(EXC_DATA_ADDRESS_READ, NULL, NULL);
	irq_set_handler(EXC_DATA_ADDRESS_WRITE, NULL, NULL);
	dbglog(DBG_CRITICAL, "dce: address error at %08lx (op %04x), not one emulated\n", (unsigned long)pc, op);
}

void dc_exceptions_install(void)
{
	irq_set_handler(EXC_DATA_ADDRESS_READ, dc_address_error, NULL);
	irq_set_handler(EXC_DATA_ADDRESS_WRITE, dc_address_error, NULL);
}

/*
HALO_DC_PREFIX.H

Force-included ahead of every game unit in the Dreamcast build (gcc
-include). The Dreamcast build is the N64 build's game side (port/n64/game,
the HALO_N64 trims in source/) on a different platform layer (port/dc/src):
HALO_N64 stays defined, HALO_DC marks the places where the two differ. The
Dreamcast is little-endian SH-4 with 32-bit pointers and longs, as the Xbox
is, so the game's structures and the Xbox tag data keep their layouts and
byte order.
*/

#ifndef __HALO_DC_PREFIX_H
#define __HALO_DC_PREFIX_H

#if !defined(__sh__) || !defined(__LITTLE_ENDIAN__)
#error the Dreamcast build targets little-endian SH-4
#endif

#define HALO_DC 1
#define HALO_N64 1
#define HALO_LINUX 1

/* ---------- XDK architecture selection (the SDK declarations need one) */

#define _X86_ 1
#define _STDCALL_SUPPORTED 1
#define _INTEGRAL_MAX_BITS 64
typedef __WCHAR_TYPE__ wchar_t;
#define _WCHAR_T_DEFINED
#define _USE_MATH_DEFINES
#define __export

/* ---------- calling conventions and MSVC keywords that mean nothing here */

#define __cdecl
#define __stdcall
#define __fastcall
#define _cdecl
#define _stdcall
#define _fastcall
#define __declspec(x)
#define __int8 char
#define __int16 short
#define __int32 int
#define __int64 long long
#define __unaligned
#define __w64
#define __ptr32

/* ---------- MSVC inline semantics (see halo_n64_prefix.h) */

#define __inline __inline__
#define _inline __inline__
#define __forceinline __inline__ __attribute__((always_inline))
#define __NO_INLINE__ 1

#define DECLSPEC_SELECTANY __attribute__((weak))

#define _InterlockedCompareExchange halo_linux_InterlockedCompareExchange
#define _InterlockedDecrement halo_linux_InterlockedDecrement
#define _InterlockedExchange halo_linux_InterlockedExchange
#define _InterlockedExchangeAdd halo_linux_InterlockedExchangeAdd
#define _InterlockedIncrement halo_linux_InterlockedIncrement

/* ---------- structured exception handling: compiled out */

#define __try if (1)
#define __except(filter) else if (0)
#define __finally
#define __leave

/* ---------- the N64's limits (4 local players, small pools) */

#include "../../n64/include/halo_n64_limits.h"

#ifndef HALO_LINUX_PLATFORM_LAYER
#define FD_SETSIZE HALO_PORT_FD_SETSIZE
#include "../../linux/include/halo_linux_winsock_names.h"
#include "../../linux/include/halo_linux_source_fixups.h"
#endif

#include <stddef.h>

#endif /* __HALO_DC_PREFIX_H */

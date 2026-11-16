/*
 * mmtypes.h - basic types shared by the kernel driver and the user-mode tests.
 *
 * The protocol parser (mmproto.c) and gesture engine (gesture.c) contain no
 * WDK dependencies so they can be unit tested on any OS. Build them with
 * -DMM_USERMODE outside the WDK.
 */
#pragma once

#ifdef MM_USERMODE
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef int32_t  LONG;
typedef uint32_t ULONG;
typedef uint16_t USHORT;
typedef uint8_t  UCHAR;
typedef uint8_t  BOOLEAN;
typedef int64_t  LONGLONG;
typedef uint64_t ULONGLONG;
#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif
#define MmZero(p, n) memset((p), 0, (n))
#else
#include <ntddk.h>
#define MmZero(p, n) RtlZeroMemory((p), (n))
#endif

#define MM_ABS(v) ((v) < 0 ? -(v) : (v))
#define MM_MAX(a, b) ((a) > (b) ? (a) : (b))
#define MM_MIN(a, b) ((a) < (b) ? (a) : (b))
#define MM_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))

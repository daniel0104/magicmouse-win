/*
 * gesture.h - turns Magic Mouse touch frames into a 3-button mouse with a
 * high-resolution vertical wheel and horizontal pan (AC Pan).
 *
 * Pure C, no WDK dependencies. The caller serialises access (the driver holds
 * a spin lock around every call).
 */
#pragma once
#include "mmtypes.h"

#define MM_MAX_TOUCHES        16
#define MM_REPORT_ID          0x01
#define MM_REPORT_LEN         10     /* id, buttons, X(16), Y(16), wheel(16), pan(16) */
#define MM_HIRES_PER_NOTCH    120    /* matches WHEEL_DELTA */
#define MM_INERTIA_TICK_MS    10

/* MiddleClickMode bits */
#define MM_MIDDLE_OFF         0
#define MM_MIDDLE_ZONE        1      /* click with the finger in the centre strip */
#define MM_MIDDLE_TWO_FINGER  2      /* click with two or more fingers on the surface */

/* Resolution multiplier feature bits (set by Windows through the virtual device) */
#define MM_RESMULT_WHEEL      0x01
#define MM_RESMULT_PAN        0x04

typedef struct _MM_SETTINGS {
    ULONG MiddleClickMode;      /* MM_MIDDLE_* bit mask, default ZONE */
    ULONG MiddleZoneWidth;      /* half width of the centre strip, touch units (default 350) */
    ULONG ScrollEnabled;        /* 0/1 */
    ULONG HorizontalScroll;     /* 0/1 */
    ULONG NaturalScrolling;     /* 0/1, 1 = content follows the finger (macOS default) */
    ULONG ScrollSpeed;          /* percent, 10..500 (default 100) */
    ULONG Acceleration;         /* 0/1 */
    ULONG Inertia;              /* 0/1, momentum after lifting the finger */
    ULONG AxisLock;             /* 0/1, lock to one axis when the swipe is clearly straight */
    ULONG ScrollThreshold;      /* touch units a finger must travel before it scrolls (default 25) */
    ULONG EnableMethod;         /* 0 send on interrupt channel, 1 open our own control channel (default) */
} MM_SETTINGS;

typedef struct _MM_TOUCH_IN {
    LONG    Id;
    LONG    X;                  /* roughly -1100 (left) .. 1258 (right) */
    LONG    Y;                  /* roughly -1589 (front, cable end) .. 2047 (back, palm) */
    BOOLEAN Down;
} MM_TOUCH_IN;

typedef struct _MM_TOUCH {
    BOOLEAN Active;
    BOOLEAN Seen;
    BOOLEAN Blocked;            /* touched during a click: never scrolls until lifted */
    LONG    X, Y;
    LONG    OriginX, OriginY;
    LONG    LastX, LastY;
    ULONG   Seq;                /* touch-down order */
} MM_TOUCH;

typedef struct _MM_AXIS {
    LONGLONG RemQ8;             /* sub-count remainder, 1/256 hi-res count units */
    LONG     VelQ8;             /* velocity, 1/256 hi-res counts per ms */
} MM_AXIS;

typedef struct _MM_GESTURE {
    MM_TOUCH  Touches[MM_MAX_TOUCHES];
    ULONG     Seq;
    LONG      ScrollFinger;     /* -1 when not scrolling */
    LONG      Lock;             /* 0 free, 1 vertical, 2 horizontal */
    UCHAR     PhysButtons;      /* raw device bits (1 left, 2 right) */
    UCHAR     Buttons;          /* latched output buttons (1 L, 2 R, 4 M) */
    UCHAR     LastSentButtons;
    UCHAR     ResMultiplier;    /* MM_RESMULT_* bits */
    MM_AXIS   Wheel;
    MM_AXIS   Pan;
    LONGLONG  LastFrameMs;
    LONGLONG  LastMoveMs;
    LONGLONG  LastInertiaMs;
    BOOLEAN   InertiaActive;
} MM_GESTURE;

void MmSettingsDefaults(MM_SETTINGS *s);
void MmSettingsClamp(MM_SETTINGS *s);

void MmGestureInit(MM_GESTURE *g);

/* Feed one touch frame. Returns TRUE if out[] holds a report to send.
   Pass touches == NULL for a report that carries only motion and buttons
   (current touches are left as they are). */
BOOLEAN MmGestureFrame(MM_GESTURE *g, const MM_SETTINGS *s, LONGLONG nowMs,
                       const MM_TOUCH_IN *touches, ULONG count,
                       LONG dx, LONG dy, UCHAR clicks,
                       UCHAR out[MM_REPORT_LEN]);

/* Advance momentum scrolling. Returns TRUE if out[] holds a report to send.
   Keep calling every MM_INERTIA_TICK_MS while g->InertiaActive is TRUE. */
BOOLEAN MmGestureTick(MM_GESTURE *g, const MM_SETTINGS *s, LONGLONG nowMs,
                      UCHAR out[MM_REPORT_LEN]);

/* Drop all touches (disconnect, power down). Returns TRUE if out[] holds a
   button-release report that must be sent. */
BOOLEAN MmGestureReset(MM_GESTURE *g, UCHAR out[MM_REPORT_LEN]);

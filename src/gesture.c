/*
 * gesture.c - Magic Mouse touch frames -> mouse report.
 *
 * Scrolling model
 *   - Any finger that moves more than ScrollThreshold units from where it
 *     touched down becomes the scroll finger. Fingers that just rest on the
 *     shell never cross the threshold, so they never scroll.
 *   - Movement of the scroll finger is converted to hi-res wheel counts
 *     (120 per notch) with optional speed-based acceleration.
 *   - Both axes scroll (2D). With AxisLock a clearly straight swipe is locked
 *     to its axis so vertical scrolling doesn't drift sideways.
 *   - On lift, if the finger was still moving, momentum (inertia) continues
 *     and decays, like macOS. Touching or clicking stops it.
 *
 * Buttons
 *   - The mouse reports left/right itself (it guesses from finger position).
 *   - Middle click emulation (configurable):
 *       ZONE:       the most recent finger is in the centre strip -> middle
 *       TWO_FINGER: two or more fingers on the surface -> middle
 *   - The button chosen at press time is latched until release.
 *
 * All arithmetic is integer (no floating point in the kernel).
 */
#include "gesture.h"

#define MM_UNITS_PER_NOTCH      150     /* touch units per wheel notch at 100% speed */
#define MM_ACCEL_MAX_Q8         (3 * 256)
#define MM_MOVE_STALE_MS        60      /* finger must have moved this recently to start inertia */
#define MM_INERTIA_START_Q8     128     /* 0.5 counts/ms  (~4 notches/s) */
#define MM_INERTIA_STOP_Q8      8       /* 0.03 counts/ms */
#define MM_INERTIA_MAX_Q8       (8 * 256)
#define MM_DECAY_Q16_PER_MS     65303   /* 0.99644/ms -> ~280 ms time constant */

void MmSettingsDefaults(MM_SETTINGS *s)
{
    s->MiddleClickMode  = MM_MIDDLE_ZONE;
    s->MiddleZoneWidth  = 350;
    s->ScrollEnabled    = 1;
    s->HorizontalScroll = 1;
    s->NaturalScrolling = 1;
    s->ScrollSpeed      = 100;
    s->Acceleration     = 1;
    s->Inertia          = 1;
    s->AxisLock         = 1;
    s->ScrollThreshold  = 25;
    s->EnableMethod     = 1;
}

void MmSettingsClamp(MM_SETTINGS *s)
{
    s->MiddleClickMode &= (MM_MIDDLE_ZONE | MM_MIDDLE_TWO_FINGER);
    s->MiddleZoneWidth  = MM_CLAMP(s->MiddleZoneWidth, 50u, 1200u);
    s->ScrollEnabled    = s->ScrollEnabled ? 1 : 0;
    s->HorizontalScroll = s->HorizontalScroll ? 1 : 0;
    s->NaturalScrolling = s->NaturalScrolling ? 1 : 0;
    s->ScrollSpeed      = MM_CLAMP(s->ScrollSpeed, 10u, 500u);
    s->Acceleration     = s->Acceleration ? 1 : 0;
    s->Inertia          = s->Inertia ? 1 : 0;
    s->AxisLock         = s->AxisLock ? 1 : 0;
    s->ScrollThreshold  = MM_CLAMP(s->ScrollThreshold, 1u, 400u);
    s->EnableMethod     = MM_MIN(s->EnableMethod, 1u);
}

void MmGestureInit(MM_GESTURE *g)
{
    UCHAR resMult = g->ResMultiplier;   /* survives resets: owned by Windows */
    MmZero(g, sizeof(*g));
    g->ScrollFinger = -1;
    g->ResMultiplier = resMult;
}

static void MmBuildReport(UCHAR out[MM_REPORT_LEN], UCHAR buttons,
                          LONG dx, LONG dy, LONG wheel, LONG pan)
{
    dx = MM_CLAMP(dx, -32767, 32767);
    dy = MM_CLAMP(dy, -32767, 32767);
    out[0] = MM_REPORT_ID;
    out[1] = buttons & 0x07;
    out[2] = (UCHAR)(dx & 0xFF);
    out[3] = (UCHAR)((dx >> 8) & 0xFF);
    out[4] = (UCHAR)(dy & 0xFF);
    out[5] = (UCHAR)((dy >> 8) & 0xFF);
    wheel = MM_CLAMP(wheel, -32767, 32767);
    pan = MM_CLAMP(pan, -32767, 32767);
    out[6] = (UCHAR)(wheel & 0xFF);
    out[7] = (UCHAR)((wheel >> 8) & 0xFF);
    out[8] = (UCHAR)(pan & 0xFF);
    out[9] = (UCHAR)((pan >> 8) & 0xFF);
}

/* Add countsQ8 (1/256 of a hi-res count) to an axis; return the value to put
   in the report. In low-res mode (Windows didn't set the multiplier) each
   report unit is one whole notch, so we hold back until 120 counts gathered. */
static LONG MmAxisEmit(MM_AXIS *a, LONGLONG countsQ8, BOOLEAN hires)
{
    LONGLONG counts;
    LONG out;

    a->RemQ8 += countsQ8;
    counts = a->RemQ8 / 256;                 /* truncates toward zero */

    if (hires) {
        out = (LONG)MM_CLAMP(counts, -32767, 32767);
        a->RemQ8 -= (LONGLONG)out * 256;
    } else {
        LONGLONG notches = counts / MM_HIRES_PER_NOTCH;
        out = (LONG)MM_CLAMP(notches, -32767, 32767);
        a->RemQ8 -= (LONGLONG)out * MM_HIRES_PER_NOTCH * 256;
    }
    return out;
}

static LONGLONG MmUnitsToCountsQ8(LONG units, const MM_SETTINGS *s, LONG accelQ8)
{
    /* units * (120 / UNITS_PER_NOTCH) * speed% * accel, in Q8 */
    return ((LONGLONG)units * MM_HIRES_PER_NOTCH * (LONGLONG)s->ScrollSpeed * accelQ8)
           / (100LL * MM_UNITS_PER_NOTCH);
}

static void MmStopInertia(MM_GESTURE *g)
{
    g->InertiaActive = FALSE;
    g->Wheel.VelQ8 = 0;
    g->Pan.VelQ8 = 0;
}

static void MmEndScroll(MM_GESTURE *g, const MM_SETTINGS *s, LONGLONG nowMs)
{
    BOOLEAN fast;

    g->ScrollFinger = -1;
    fast = MM_ABS(g->Wheel.VelQ8) >= MM_INERTIA_START_Q8 ||
           MM_ABS(g->Pan.VelQ8)   >= MM_INERTIA_START_Q8;

    if (s->Inertia && fast && (nowMs - g->LastMoveMs) <= MM_MOVE_STALE_MS) {
        g->Wheel.VelQ8 = MM_CLAMP(g->Wheel.VelQ8, -MM_INERTIA_MAX_Q8, MM_INERTIA_MAX_Q8);
        g->Pan.VelQ8   = MM_CLAMP(g->Pan.VelQ8,   -MM_INERTIA_MAX_Q8, MM_INERTIA_MAX_Q8);
        g->InertiaActive = TRUE;
        g->LastInertiaMs = nowMs;
    } else {
        MmStopInertia(g);
    }
}

BOOLEAN MmGestureFrame(MM_GESTURE *g, const MM_SETTINGS *s, LONGLONG nowMs,
                       const MM_TOUCH_IN *touches, ULONG count,
                       LONG dx, LONG dy, UCHAR clicks,
                       UCHAR out[MM_REPORT_LEN])
{
    ULONG i;
    LONG active = 0, latest = -1;
    ULONG latestSeq = 0;
    UCHAR phys;
    LONG wheel = 0, pan = 0;
    LONG frameDt;
    BOOLEAN send;

    frameDt = (LONG)MM_CLAMP(nowMs - g->LastFrameMs, 1, 100);
    g->LastFrameMs = nowMs;

    /* ---- touch tracking (touches == NULL: motion/buttons only, keep touches) ---- */
    for (i = 0; touches != NULL && i < MM_MAX_TOUCHES; i++) {
        g->Touches[i].Seen = FALSE;
    }

    for (i = 0; touches != NULL && i < count; i++) {
        MM_TOUCH *t;
        if (!touches[i].Down) {
            continue;
        }
        t = &g->Touches[touches[i].Id & (MM_MAX_TOUCHES - 1)];
        t->Seen = TRUE;
        if (!t->Active) {
            t->Active = TRUE;
            t->Blocked = (g->Buttons != 0);
            t->OriginX = t->LastX = touches[i].X;
            t->OriginY = t->LastY = touches[i].Y;
            t->Seq = ++g->Seq;
            MmStopInertia(g);            /* touching stops momentum */
        }
        t->X = touches[i].X;
        t->Y = touches[i].Y;
    }

    for (i = 0; i < MM_MAX_TOUCHES; i++) {
        MM_TOUCH *t = &g->Touches[i];
        if (touches != NULL && t->Active && !t->Seen) {
            t->Active = FALSE;
            if ((LONG)i == g->ScrollFinger) {
                MmEndScroll(g, s, nowMs);
            }
        }
        if (t->Active) {
            active++;
            if (t->Seq >= latestSeq) {
                latestSeq = t->Seq;
                latest = (LONG)i;
            }
        }
    }

    /* ---- buttons ---- */
    phys = clicks & 0x03;
    if (phys != 0 && g->PhysButtons == 0) {
        UCHAR mapped = (phys == 0x03) ? 0x01 : phys;

        if ((s->MiddleClickMode & MM_MIDDLE_TWO_FINGER) && (active >= 2 || phys == 0x03)) {
            mapped = 0x04;
        } else if ((s->MiddleClickMode & MM_MIDDLE_ZONE) && latest >= 0 &&
                   MM_ABS(g->Touches[latest].X) <= (LONG)s->MiddleZoneWidth) {
            mapped = 0x04;
        }
        g->Buttons = mapped;

        /* A click ends any scroll and the fingers involved never scroll. */
        if (g->ScrollFinger >= 0) {
            g->ScrollFinger = -1;
        }
        MmStopInertia(g);
        for (i = 0; i < MM_MAX_TOUCHES; i++) {
            if (g->Touches[i].Active) {
                g->Touches[i].Blocked = TRUE;
            }
        }
    } else if (phys == 0) {
        g->Buttons = 0;
    }
    g->PhysButtons = phys;

    /* ---- scrolling ---- */
    if (s->ScrollEnabled && g->Buttons == 0) {
        if (g->ScrollFinger < 0) {
            LONG best = -1, bestDist = 0;
            for (i = 0; i < MM_MAX_TOUCHES; i++) {
                MM_TOUCH *t = &g->Touches[i];
                LONG dist;
                if (!t->Active || t->Blocked) {
                    continue;
                }
                dist = MM_MAX(MM_ABS(t->X - t->OriginX), MM_ABS(t->Y - t->OriginY));
                if (dist >= (LONG)s->ScrollThreshold && dist > bestDist) {
                    best = (LONG)i;
                    bestDist = dist;
                }
            }
            if (best >= 0) {
                MM_TOUCH *t = &g->Touches[best];
                LONG ax = MM_ABS(t->X - t->OriginX);
                LONG ay = MM_ABS(t->Y - t->OriginY);

                g->ScrollFinger = best;
                g->Lock = 0;
                if (!s->HorizontalScroll) {
                    g->Lock = 1;
                } else if (s->AxisLock) {
                    if (ay >= 2 * ax) {
                        g->Lock = 1;
                    } else if (ax >= 2 * ay) {
                        g->Lock = 2;
                    }
                }
                t->LastX = t->X;
                t->LastY = t->Y;
                g->Wheel.VelQ8 = 0;
                g->Pan.VelQ8 = 0;
                g->Wheel.RemQ8 = 0;
                g->Pan.RemQ8 = 0;
                g->LastMoveMs = nowMs;
            }
        } else {
            MM_TOUCH *t = &g->Touches[g->ScrollFinger];
            LONG ddx = t->X - t->LastX;
            LONG ddy = t->Y - t->LastY;
            LONGLONG cy = 0, cx = 0;

            t->LastX = t->X;
            t->LastY = t->Y;

            if (g->Lock == 1) {
                ddx = 0;
            } else if (g->Lock == 2) {
                ddy = 0;
            }

            if (ddx != 0 || ddy != 0) {
                LONG accelQ8 = 256;
                if (s->Acceleration) {
                    LONG speedQ8 = (MM_MAX(MM_ABS(ddx), MM_ABS(ddy)) * 256) / frameDt;
                    accelQ8 = MM_MIN(256 + speedQ8 / 6, MM_ACCEL_MAX_Q8);
                }
                /* Touch Y grows toward the palm, X grows to the right.
                   Wheel + = scroll up, Pan + = scroll right. */
                cy = MmUnitsToCountsQ8(s->NaturalScrolling ? ddy : -ddy, s, accelQ8);
                cx = MmUnitsToCountsQ8(s->NaturalScrolling ? -ddx : ddx, s, accelQ8);

                wheel = MmAxisEmit(&g->Wheel, cy, (g->ResMultiplier & MM_RESMULT_WHEEL) != 0);
                pan   = MmAxisEmit(&g->Pan,   cx, (g->ResMultiplier & MM_RESMULT_PAN) != 0);
                g->LastMoveMs = nowMs;
            }

            /* Exponential moving average of velocity (also decays while still). */
            g->Wheel.VelQ8 = (LONG)((g->Wheel.VelQ8 + cy / frameDt) / 2);
            g->Pan.VelQ8   = (LONG)((g->Pan.VelQ8   + cx / frameDt) / 2);
        }
    }

    MmBuildReport(out, g->Buttons, dx, dy, wheel, pan);
    send = (dx != 0 || dy != 0 || wheel != 0 || pan != 0 || g->Buttons != g->LastSentButtons);
    g->LastSentButtons = g->Buttons;
    return send;
}

BOOLEAN MmGestureTick(MM_GESTURE *g, const MM_SETTINGS *s, LONGLONG nowMs,
                      UCHAR out[MM_REPORT_LEN])
{
    LONG dt, k, wheel, pan;

    (void)s;
    if (!g->InertiaActive) {
        return FALSE;
    }

    dt = (LONG)MM_CLAMP(nowMs - g->LastInertiaMs, 0, 50);
    if (dt == 0) {
        return FALSE;
    }
    g->LastInertiaMs = nowMs;

    wheel = MmAxisEmit(&g->Wheel, (LONGLONG)g->Wheel.VelQ8 * dt,
                       (g->ResMultiplier & MM_RESMULT_WHEEL) != 0);
    pan   = MmAxisEmit(&g->Pan, (LONGLONG)g->Pan.VelQ8 * dt,
                       (g->ResMultiplier & MM_RESMULT_PAN) != 0);

    for (k = 0; k < dt; k++) {
        g->Wheel.VelQ8 = (LONG)(((LONGLONG)g->Wheel.VelQ8 * MM_DECAY_Q16_PER_MS) / 65536);
        g->Pan.VelQ8   = (LONG)(((LONGLONG)g->Pan.VelQ8   * MM_DECAY_Q16_PER_MS) / 65536);
    }

    if (MM_ABS(g->Wheel.VelQ8) < MM_INERTIA_STOP_Q8 && MM_ABS(g->Pan.VelQ8) < MM_INERTIA_STOP_Q8) {
        MmStopInertia(g);
    }

    if (wheel == 0 && pan == 0) {
        return FALSE;
    }
    MmBuildReport(out, g->Buttons, 0, 0, wheel, pan);
    g->LastSentButtons = g->Buttons;
    return TRUE;
}

BOOLEAN MmGestureReset(MM_GESTURE *g, UCHAR out[MM_REPORT_LEN])
{
    BOOLEAN release = (g->LastSentButtons != 0);
    MmGestureInit(g);
    if (release) {
        MmBuildReport(out, 0, 0, 0, 0, 0);
    }
    return release;
}

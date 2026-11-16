/*
 * User-mode tests for the protocol parser and gesture engine.
 *   cc -DMM_USERMODE -I../src -Wall -Wextra -o test_gesture test_gesture.c ../src/gesture.c ../src/mmproto.c
 */
#include <stdio.h>
#include <stdlib.h>
#include "mmproto.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Inverse of MmDecodeTouch. */
static void EncodeTouch(UCHAR *t, int id, int x, int y, UCHAR state)
{
    int raw = -y;
    memset(t, 0, 8);
    t[0] = (UCHAR)(x & 0xFF);
    t[1] = (UCHAR)(((x >> 8) & 0x0F) | ((raw & 0x0F) << 4));
    t[2] = (UCHAR)((raw >> 4) & 0xFF);
    t[3] = 40; t[4] = 30;                    /* touch major/minor */
    t[5] = (UCHAR)(((id & 3) << 6) | 10);    /* size */
    t[6] = (UCHAR)((id >> 2) & 3) | (32 << 2);
    t[7] = state;
}

typedef struct { int id, x, y; UCHAR state; } T;

/* Build an MM2 report (ID 0x12) with touches. Returns length. */
static ULONG BuildMM2(UCHAR *r, UCHAR clicks, short dx, short dy, const T *t, int n)
{
    int i;
    memset(r, 0, 14 + 8 * n);
    r[0] = 0x12; r[1] = clicks;
    r[2] = (UCHAR)(dx & 0xFF); r[3] = (UCHAR)((dx >> 8) & 0xFF);
    r[4] = (UCHAR)(dy & 0xFF); r[5] = (UCHAR)((dy >> 8) & 0xFF);
    for (i = 0; i < n; i++) EncodeTouch(r + 14 + 8 * i, t[i].id, t[i].x, t[i].y, t[i].state);
    return 14 + 8 * n;
}

static ULONG BuildMM1(UCHAR *r, UCHAR clicks, int dx, int dy, const T *t, int n)
{
    int i;
    memset(r, 0, 6 + 8 * n);
    r[0] = 0x29;
    r[1] = (UCHAR)(dx & 0xFF);
    r[2] = (UCHAR)(dy & 0xFF);
    r[3] = (UCHAR)((clicks & 3) | (((dx >> 8) & 3) << 2) | (((dy >> 8) & 3) << 4));
    for (i = 0; i < n; i++) EncodeTouch(r + 6 + 8 * i, t[i].id, t[i].x, t[i].y, t[i].state);
    return 6 + 8 * n;
}

typedef struct {
    MM_GESTURE g; MM_SETTINGS s; LONGLONG now;
    long wheel, pan, dx, dy; UCHAR buttons; int reports;
} Rig;

static void RigInit(Rig *rig)
{
    memset(rig, 0, sizeof(*rig));
    MmSettingsDefaults(&rig->s);
    MmGestureInit(&rig->g);
    rig->g.ResMultiplier = MM_RESMULT_WHEEL | MM_RESMULT_PAN;
    rig->now = 1000;
}

static void Consume(Rig *rig, const UCHAR *out)
{
    rig->reports++;
    rig->buttons = out[1];
    rig->dx += (short)(out[2] | (out[3] << 8));
    rig->dy += (short)(out[4] | (out[5] << 8));
    rig->wheel += (short)(out[6] | (out[7] << 8));
    rig->pan += (short)(out[8] | (out[9] << 8));
}

static void Feed(Rig *rig, const UCHAR *r, ULONG len)
{
    MM_FRAME f[2]; ULONG n, i; UCHAR out[MM_REPORT_LEN];
    MM_REPORT_KIND k = MmParseReport(r, len, f, &n);
    CHECK(k == MM_KIND_TOUCH_MM1 || k == MM_KIND_TOUCH_MM2 || k == MM_KIND_NATIVE_MM2, "kind %d", k);
    for (i = 0; i < n; i++) {
        if (MmGestureFrame(&rig->g, &rig->s, rig->now, f[i].Touches, f[i].Count, f[i].Dx, f[i].Dy, f[i].Clicks, out))
            Consume(rig, out);
    }
    rig->now += 11;     /* ~90 Hz */
}

static void RunInertia(Rig *rig)
{
    UCHAR out[MM_REPORT_LEN]; int guard = 0;
    while (rig->g.InertiaActive && guard++ < 1000) {
        rig->now += MM_INERTIA_TICK_MS;
        if (MmGestureTick(&rig->g, &rig->s, rig->now, out)) Consume(rig, out);
    }
    CHECK(guard < 1000, "inertia never stopped");
}

/* One finger swipe along a path; ends with lift. */
static void Swipe(Rig *rig, int mm1, int x0, int y0, int x1, int y1, int steps)
{
    UCHAR r[256]; int i; T t;
    for (i = 0; i <= steps; i++) {
        t.id = 3; t.x = x0 + (x1 - x0) * i / steps; t.y = y0 + (y1 - y0) * i / steps;
        t.state = i == 0 ? 0x30 : 0x40;
        Feed(rig, r, mm1 ? BuildMM1(r, 0, 0, 0, &t, 1) : BuildMM2(r, 0, 0, 0, &t, 1));
    }
    Feed(rig, r, mm1 ? BuildMM1(r, 0, 0, 0, NULL, 0) : BuildMM2(r, 0, 0, 0, NULL, 0));
}

static void TestDecode(void)
{
    UCHAR r[64]; MM_FRAME f[2]; ULONG n;
    T t[2] = { { 5, -1000, 2000, 0x40 }, { 12, 1200, -1500, 0x30 } };
    ULONG len = BuildMM2(r, 2, -300, 450, t, 2);
    MM_REPORT_KIND k = MmParseReport(r, len, f, &n);
    CHECK(k == MM_KIND_TOUCH_MM2 && n == 1 && f[0].Count == 2, "mm2 parse");
    CHECK(f[0].Dx == -300 && f[0].Dy == 450 && f[0].Clicks == 2, "mm2 motion %d %d %d", f[0].Dx, f[0].Dy, f[0].Clicks);
    CHECK(f[0].Touches[0].Id == 5 && f[0].Touches[0].X == -1000 && f[0].Touches[0].Y == 2000 && f[0].Touches[0].Down, "touch0 %d %d %d", f[0].Touches[0].Id, f[0].Touches[0].X, f[0].Touches[0].Y);
    CHECK(f[0].Touches[1].Id == 12 && f[0].Touches[1].X == 1200 && f[0].Touches[1].Y == -1500, "touch1");

    len = BuildMM1(r, 1, -200, 300, t, 1);
    k = MmParseReport(r, len, f, &n);
    CHECK(k == MM_KIND_TOUCH_MM1 && f[0].Dx == -200 && f[0].Dy == 300 && f[0].Clicks == 1, "mm1 %d %d", f[0].Dx, f[0].Dy);

    /* Double report */
    {
        UCHAR d[128]; ULONG l1 = BuildMM1(d + 2, 0, 1, 1, t, 1), l2;
        d[0] = 0xF7; d[1] = (UCHAR)l1;
        l2 = BuildMM1(d + 2 + l1, 0, 2, 2, t, 2);
        k = MmParseReport(d, 2 + l1 + l2, f, &n);
        CHECK(k == MM_KIND_TOUCH_MM1 && n == 2 && f[1].Count == 2 && f[1].Dx == 2, "double");
    }

    r[0] = 0x12; memset(r + 1, 0, 7);
    CHECK(MmParseReport(r, 8, f, &n) == MM_KIND_NATIVE_MM2, "native mm2");
    r[0] = 0x10;
    CHECK(MmParseReport(r, 6, f, &n) == MM_KIND_NATIVE_MM1, "native mm1");
    r[0] = 0x90;
    CHECK(MmParseReport(r, 3, f, &n) == MM_KIND_OTHER, "battery passthrough");
    r[0] = 0x12;
    CHECK(MmParseReport(r, 13, f, &n) == MM_KIND_OTHER, "bad length");

    {
        UCHAR b[8];
        CHECK(MmBuildEnablePacket(MM_MODEL_MM2, b, 8) == 4 && b[0] == 0x53 && b[1] == 0xF1, "enable mm2");
        CHECK(MmBuildEnablePacket(MM_MODEL_MM1, b, 8) == 3 && b[1] == 0xD7, "enable mm1");
    }
}

static void TestVerticalScroll(void)
{
    Rig rig;
    /* finger moves toward the palm (+Y), natural scrolling -> wheel positive (scroll up) */
    RigInit(&rig); rig.s.Inertia = 0;
    Swipe(&rig, 0, 0, -800, 0, 400, 30);
    printf("  vertical swipe 1200 units: wheel=%ld (%.1f notches) pan=%ld\n", rig.wheel, rig.wheel / 120.0, rig.pan);
    CHECK(rig.wheel > 120 * 3, "expected several notches up, got %ld", rig.wheel);
    CHECK(rig.pan == 0, "axis lock should suppress pan, got %ld", rig.pan);
    CHECK(rig.buttons == 0, "no buttons");

    RigInit(&rig); rig.s.Inertia = 0; rig.s.NaturalScrolling = 0;
    Swipe(&rig, 0, 0, -800, 0, 400, 30);
    CHECK(rig.wheel < -120 * 3, "traditional direction should be negative, got %ld", rig.wheel);
}

static void TestHorizontalAndDiagonal(void)
{
    Rig rig;
    RigInit(&rig); rig.s.Inertia = 0;
    Swipe(&rig, 0, -600, 0, 600, 0, 30);       /* finger moves right */
    printf("  horizontal swipe: wheel=%ld pan=%ld\n", rig.wheel, rig.pan);
    CHECK(rig.pan < -120, "natural: finger right -> pan left (negative), got %ld", rig.pan);
    CHECK(rig.wheel == 0, "locked to horizontal");

    RigInit(&rig); rig.s.Inertia = 0;
    Swipe(&rig, 0, -500, -500, 500, 500, 30);  /* diagonal: both axes */
    printf("  diagonal swipe: wheel=%ld pan=%ld\n", rig.wheel, rig.pan);
    CHECK(rig.wheel > 0 && rig.pan < 0, "2D scroll on a diagonal");

    RigInit(&rig); rig.s.Inertia = 0; rig.s.HorizontalScroll = 0;
    Swipe(&rig, 0, -600, 0, 600, 0, 30);
    CHECK(rig.pan == 0, "horizontal disabled");
}

static void TestRestingFingerAndMM1(void)
{
    Rig rig; UCHAR r[64]; int i;
    RigInit(&rig);
    /* resting finger with jitter + pointer movement: no scroll, motion passes through */
    for (i = 0; i < 50; i++) {
        T t = { 1, 300 + (i % 3) - 1, 900 + (i % 5) - 2, (UCHAR)(i ? 0x40 : 0x30) };
        Feed(&rig, r, BuildMM2(r, 0, 4, -3, &t, 1));
    }
    CHECK(rig.wheel == 0 && rig.pan == 0, "resting finger must not scroll");
    CHECK(rig.dx == 200 && rig.dy == -150, "pointer motion %ld %ld", rig.dx, rig.dy);

    RigInit(&rig); rig.s.Inertia = 0;
    Swipe(&rig, 1, 0, 600, 0, -600, 30);       /* MM1, finger toward the front */
    CHECK(rig.wheel < -120, "MM1 scroll down, got %ld", rig.wheel);
}

static void TestInertia(void)
{
    Rig a, b;
    RigInit(&a); a.s.Inertia = 0;
    RigInit(&b);
    Swipe(&a, 0, 0, -1000, 0, 1000, 12);       /* fast flick */
    Swipe(&b, 0, 0, -1000, 0, 1000, 12);
    CHECK(b.g.InertiaActive, "flick should start inertia");
    RunInertia(&b);
    printf("  flick: without inertia %ld, with inertia %ld\n", a.wheel, b.wheel);
    CHECK(b.wheel > a.wheel + 240, "inertia adds momentum");

    /* slow drag then hold still then lift: no inertia */
    {
        Rig c; UCHAR r[64]; int i; T t;
        RigInit(&c);
        for (i = 0; i <= 40; i++) {
            t.id = 2; t.x = 0; t.y = -600 + i * 20; t.state = i ? 0x40 : 0x30;
            Feed(&c, r, BuildMM2(r, 0, 0, 0, &t, 1));
        }
        for (i = 0; i < 20; i++) { Feed(&c, r, BuildMM2(r, 0, 0, 0, &t, 1)); }
        Feed(&c, r, BuildMM2(r, 0, 0, 0, NULL, 0));
        CHECK(!c.g.InertiaActive, "no inertia after holding still");
    }

    /* touching during inertia stops it */
    {
        UCHAR r[64]; T t = { 7, 0, 0, 0x30 };
        RigInit(&b);
        Swipe(&b, 0, 0, -1000, 0, 1000, 12);
        Feed(&b, r, BuildMM2(r, 0, 0, 0, &t, 1));
        CHECK(!b.g.InertiaActive, "touch stops inertia");
    }
}

static void Click(Rig *rig, const T *t, int n, UCHAR deviceBits)
{
    UCHAR r[64]; T tt[4]; int i;
    for (i = 0; i < n; i++) { tt[i] = t[i]; tt[i].state = 0x30; }
    Feed(rig, r, BuildMM2(r, 0, 0, 0, tt, n));
    for (i = 0; i < n; i++) tt[i].state = 0x40;
    Feed(rig, r, BuildMM2(r, deviceBits, 0, 0, tt, n));
    for (i = 0; i < n; i++) tt[i].y += 60;       /* finger slides while held: must not scroll */
    Feed(rig, r, BuildMM2(r, deviceBits, 0, 0, tt, n));
}

static void Release(Rig *rig)
{
    UCHAR r[64];
    Feed(rig, r, BuildMM2(r, 0, 0, 0, NULL, 0));
}

static void TestMiddleClick(void)
{
    Rig rig;
    T centre = { 1, 50, 800, 0 }, left = { 1, -700, 800, 0 }, right = { 1, 800, 800, 0 };
    T two[2] = { { 1, -500, 800, 0 }, { 2, 400, 800, 0 } };

    RigInit(&rig);
    Click(&rig, &centre, 1, 1);
    CHECK(rig.buttons == 4, "zone: centre click -> middle, got %d", rig.buttons);
    CHECK(rig.wheel == 0, "no scroll while clicking");
    Release(&rig);
    CHECK(rig.buttons == 0, "released");

    Click(&rig, &left, 1, 1);
    CHECK(rig.buttons == 1, "left side -> left, got %d", rig.buttons);
    Release(&rig);
    Click(&rig, &right, 1, 2);
    CHECK(rig.buttons == 2, "right side -> right, got %d", rig.buttons);
    Release(&rig);

    RigInit(&rig); rig.s.MiddleClickMode = MM_MIDDLE_OFF;
    Click(&rig, &centre, 1, 1);
    CHECK(rig.buttons == 1, "off: centre -> left");
    Release(&rig);

    RigInit(&rig); rig.s.MiddleClickMode = MM_MIDDLE_TWO_FINGER;
    Click(&rig, two, 2, 1);
    CHECK(rig.buttons == 4, "two-finger -> middle, got %d", rig.buttons);
    Release(&rig);
    Click(&rig, &centre, 1, 1);
    CHECK(rig.buttons == 1, "two-finger mode: single centre finger -> left");
    Release(&rig);

    /* latch: button chosen at press time persists */
    RigInit(&rig);
    {
        UCHAR r[64]; T t = { 1, 50, 800, 0x30 };
        Feed(&rig, r, BuildMM2(r, 0, 0, 0, &t, 1));
        t.state = 0x40;
        Feed(&rig, r, BuildMM2(r, 1, 0, 0, &t, 1));
        t.x = -900;  /* finger moves to the left half while held */
        Feed(&rig, r, BuildMM2(r, 1, 5, 5, &t, 1));
        CHECK(rig.buttons == 4, "latched middle");
    }
}

static void TestLowResAndReset(void)
{
    Rig rig; UCHAR out[MM_REPORT_LEN];
    RigInit(&rig); rig.s.Inertia = 0;
    rig.g.ResMultiplier = 0;                   /* Windows left multiplier at 1 */
    Swipe(&rig, 0, 0, -800, 0, 400, 30);
    printf("  low-res swipe: %ld notches\n", rig.wheel);
    CHECK(rig.wheel >= 3 && rig.wheel < 60, "low-res notches %ld", rig.wheel);

    RigInit(&rig);
    {
        T c = { 1, 0, 0, 0 };
        Click(&rig, &c, 1, 1);
    }
    CHECK(MmGestureReset(&rig.g, out) && out[1] == 0, "reset releases buttons");
    CHECK(rig.g.ResMultiplier == (MM_RESMULT_WHEEL | MM_RESMULT_PAN), "multiplier survives reset");
}

static void TestDescriptor(void)
{
    /* Walk the descriptor: balanced collections and total input bits. */
    ULONG i = 0; int depth = 0; ULONG size = 0, count = 0, inBits = 0, featBits = 0;
    while (i < g_MmReportDescriptorLength) {
        UCHAR b = g_MmReportDescriptor[i];
        ULONG n = b & 3; ULONG v = 0, j;
        if (n == 3) n = 4;
        for (j = 0; j < n; j++) v |= (ULONG)g_MmReportDescriptor[i + 1 + j] << (8 * j);
        switch (b & 0xFC) {
        case 0xA0: depth++; break;
        case 0xC0: depth--; break;
        case 0x74: size = v; break;
        case 0x94: count = v; break;
        case 0x80: inBits += size * count; break;
        case 0xB0: featBits += size * count; break;
        }
        i += 1 + n;
    }
    CHECK(depth == 0, "collections balanced");
    CHECK(inBits == (MM_REPORT_LEN - 1) * 8, "input report bits %lu", (unsigned long)inBits);
    CHECK(featBits == 8, "feature report bits %lu", (unsigned long)featBits);
}

static void TestMotionOnlyKeepsTouches(void)
{
    Rig rig; UCHAR r[64], out[MM_REPORT_LEN]; T t = { 4, 0, -500, 0x30 }; int i;
    RigInit(&rig); rig.s.Inertia = 0;
    for (i = 0; i < 10; i++) { Feed(&rig, r, BuildMM2(r, 0, 0, 0, &t, 1)); t.state = 0x40; t.y += 40; }
    CHECK(rig.g.ScrollFinger == 4, "scrolling with finger 4");
    if (MmGestureFrame(&rig.g, &rig.s, rig.now, NULL, 0, 3, 3, 0, out)) Consume(&rig, out);
    CHECK(rig.g.ScrollFinger == 4 && rig.g.Touches[4].Active, "motion-only report must not lift fingers");
}

int main(void)
{
    printf("motion-only\n");      TestMotionOnlyKeepsTouches();
    printf("decode\n");           TestDecode();
    printf("vertical\n");         TestVerticalScroll();
    printf("horizontal/2D\n");    TestHorizontalAndDiagonal();
    printf("resting/MM1\n");      TestRestingFingerAndMM1();
    printf("inertia\n");          TestInertia();
    printf("middle click\n");     TestMiddleClick();
    printf("low-res/reset\n");    TestLowResAndReset();
    printf("descriptor\n");       TestDescriptor();
    printf(failures ? "\n%d FAILURE(S)\n" : "\nall tests passed\n", failures);
    return failures ? 1 : 0;
}

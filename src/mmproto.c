/*
 * mmproto.c - parse Magic Mouse input reports, build the enable packet,
 * and define the HID descriptor of the virtual mouse.
 */
#include "mmproto.h"

#define TOUCH_STATE_MASK   0xF0
#define TOUCH_STATE_START  0x30
#define TOUCH_STATE_DRAG   0x40

static void MmDecodeTouch(const UCHAR *t, MM_TOUCH_IN *out)
{
    UCHAR state = t[7] & TOUCH_STATE_MASK;

    out->Id = ((t[6] << 2) | (t[5] >> 6)) & 0x0F;
    /* 12-bit signed X: t[0] low byte, low nibble of t[1] high bits. */
    out->X = ((LONG)(((ULONG)t[1] << 28) | ((ULONG)t[0] << 20))) >> 20;
    /* 12-bit signed Y: high nibble of t[1] low bits, t[2] high byte; negated
       so that Y grows toward the back (palm) of the mouse. */
    out->Y = -(((LONG)(((ULONG)t[2] << 24) | ((ULONG)t[1] << 16))) >> 20);
    out->Down = (state == TOUCH_STATE_START || state == TOUCH_STATE_DRAG);
}

static BOOLEAN MmParseTouchMM1(const UCHAR *r, ULONG len, MM_FRAME *f)
{
    ULONG i, n;

    /* id + 5 bytes prefix, then 8 bytes per touch */
    if (len < 6 || ((len - 6) % 8) != 0) {
        return FALSE;
    }
    n = (len - 6) / 8;
    if (n > MM_MAX_TOUCHES) {
        return FALSE;
    }
    MmZero(f, sizeof(*f));
    for (i = 0; i < n; i++) {
        MmDecodeTouch(r + 6 + i * 8, &f->Touches[i]);
    }
    f->Count = n;
    /* 10-bit signed deltas: 8 bits in r[1]/r[2], top 2 bits in r[3]. */
    f->Dx = ((LONG)((((ULONG)r[3] & 0x0C) << 28) | ((ULONG)r[1] << 22))) >> 22;
    f->Dy = ((LONG)((((ULONG)r[3] & 0x30) << 26) | ((ULONG)r[2] << 22))) >> 22;
    f->Clicks = r[3] & 0x03;
    return TRUE;
}

static BOOLEAN MmParseMM2(const UCHAR *r, ULONG len, MM_FRAME *f)
{
    ULONG i, n = 0;

    if (len != 8 && (len < 14 || ((len - 14) % 8) != 0)) {
        return FALSE;
    }
    if (len >= 14) {
        n = (len - 14) / 8;
        if (n > MM_MAX_TOUCHES) {
            return FALSE;
        }
    }
    MmZero(f, sizeof(*f));
    for (i = 0; i < n; i++) {
        MmDecodeTouch(r + 14 + i * 8, &f->Touches[i]);
    }
    f->Count = n;
    f->Dx = (short)(r[2] | (r[3] << 8));
    f->Dy = (short)(r[4] | (r[5] << 8));
    f->Clicks = r[1] & 0x03;
    return TRUE;
}

MM_REPORT_KIND MmParseReport(const UCHAR *report, ULONG length,
                             MM_FRAME frames[2], ULONG *frameCount)
{
    *frameCount = 0;
    if (length < 1) {
        return MM_KIND_OTHER;
    }

    switch (report[0]) {
    case MM1_NATIVE_REPORT_ID:
        return MM_KIND_NATIVE_MM1;

    case MM1_TOUCH_REPORT_ID:
        if (MmParseTouchMM1(report, length, &frames[0])) {
            *frameCount = 1;
            return MM_KIND_TOUCH_MM1;
        }
        return MM_KIND_OTHER;

    case MM2_REPORT_ID:
        if (MmParseMM2(report, length, &frames[0])) {
            *frameCount = 1;
            return (length == 8) ? MM_KIND_NATIVE_MM2 : MM_KIND_TOUCH_MM2;
        }
        return MM_KIND_OTHER;

    case MM_DOUBLE_REPORT_ID: {
        /* [F7][len1][report1 ...][report2 ...] */
        ULONG len1, len2;
        const UCHAR *r1, *r2;
        if (length < 3) {
            return MM_KIND_OTHER;
        }
        len1 = report[1];
        if (len1 == 0 || 2 + len1 >= length) {
            return MM_KIND_OTHER;
        }
        r1 = report + 2;
        r2 = report + 2 + len1;
        len2 = length - 2 - len1;
        if (r1[0] != MM1_TOUCH_REPORT_ID || r2[0] != MM1_TOUCH_REPORT_ID) {
            return MM_KIND_OTHER;
        }
        if (!MmParseTouchMM1(r1, len1, &frames[0]) || !MmParseTouchMM1(r2, len2, &frames[1])) {
            return MM_KIND_OTHER;
        }
        *frameCount = 2;
        return MM_KIND_TOUCH_MM1;
    }

    default:
        return MM_KIND_OTHER;
    }
}

ULONG MmBuildEnablePacket(MM_MODEL model, UCHAR *buffer, ULONG bufferLength)
{
    if (bufferLength < 4) {
        return 0;
    }
    buffer[0] = HIDP_SET_REPORT | HIDP_REPORT_FEATURE;     /* 0x53 */
    switch (model) {
    case MM_MODEL_MM1:
        buffer[1] = 0xD7;
        buffer[2] = 0x01;
        return 3;
    case MM_MODEL_MM2:
        buffer[1] = 0xF1;
        buffer[2] = 0x02;
        buffer[3] = 0x01;
        return 4;
    default:
        return 0;
    }
}

/*
 * Virtual mouse: report 1 = buttons(3) + X/Y + wheel + AC Pan (all 16-bit relative).
 * Wheel and Pan each sit in their own logical collection together with a
 * Resolution Multiplier feature (physical 1..120). Windows sets the multiplier,
 * turning on high-resolution (smooth) scrolling for both axes.
 */
const UCHAR g_MmReportDescriptor[] = {
    0x05, 0x01,                 /* Usage Page (Generic Desktop) */
    0x09, 0x02,                 /* Usage (Mouse) */
    0xA1, 0x01,                 /* Collection (Application) */
    0x85, MM_REPORT_ID,         /*   Report ID (1) */
    0x09, 0x02,                 /*   Usage (Mouse) */
    0xA1, 0x02,                 /*   Collection (Logical) */
    0x09, 0x01,                 /*     Usage (Pointer) */
    0xA1, 0x00,                 /*     Collection (Physical) */

    0x05, 0x09,                 /*       Usage Page (Button) */
    0x19, 0x01,                 /*       Usage Minimum (1) */
    0x29, 0x03,                 /*       Usage Maximum (3) */
    0x15, 0x00,                 /*       Logical Minimum (0) */
    0x25, 0x01,                 /*       Logical Maximum (1) */
    0x75, 0x01,                 /*       Report Size (1) */
    0x95, 0x03,                 /*       Report Count (3) */
    0x81, 0x02,                 /*       Input (Data,Var,Abs) */
    0x75, 0x05,                 /*       Report Size (5) */
    0x95, 0x01,                 /*       Report Count (1) */
    0x81, 0x03,                 /*       Input (Const) */

    0x05, 0x01,                 /*       Usage Page (Generic Desktop) */
    0x09, 0x30,                 /*       Usage (X) */
    0x09, 0x31,                 /*       Usage (Y) */
    0x16, 0x01, 0x80,           /*       Logical Minimum (-32767) */
    0x26, 0xFF, 0x7F,           /*       Logical Maximum (32767) */
    0x75, 0x10,                 /*       Report Size (16) */
    0x95, 0x02,                 /*       Report Count (2) */
    0x81, 0x06,                 /*       Input (Data,Var,Rel) */

    0xA1, 0x02,                 /*       Collection (Logical) */
    0x09, 0x48,                 /*         Usage (Resolution Multiplier) */
    0x15, 0x00,                 /*         Logical Minimum (0) */
    0x25, 0x01,                 /*         Logical Maximum (1) */
    0x35, 0x01,                 /*         Physical Minimum (1) */
    0x45, 0x78,                 /*         Physical Maximum (120) */
    0x75, 0x02,                 /*         Report Size (2) */
    0x95, 0x01,                 /*         Report Count (1) */
    0xB1, 0x02,                 /*         Feature (Data,Var,Abs) */
    0x35, 0x00,                 /*         Physical Minimum (0) */
    0x45, 0x00,                 /*         Physical Maximum (0) */
    0x09, 0x38,                 /*         Usage (Wheel) */
    0x16, 0x01, 0x80,           /*         Logical Minimum (-32767) */
    0x26, 0xFF, 0x7F,           /*         Logical Maximum (32767) */
    0x75, 0x10,                 /*         Report Size (16) */
    0x95, 0x01,                 /*         Report Count (1) */
    0x81, 0x06,                 /*         Input (Data,Var,Rel) */
    0xC0,                       /*       End Collection */

    0xA1, 0x02,                 /*       Collection (Logical) */
    0x09, 0x48,                 /*         Usage (Resolution Multiplier) */
    0x15, 0x00,                 /*         Logical Minimum (0) */
    0x25, 0x01,                 /*         Logical Maximum (1) */
    0x35, 0x01,                 /*         Physical Minimum (1) */
    0x45, 0x78,                 /*         Physical Maximum (120) */
    0x75, 0x02,                 /*         Report Size (2) */
    0x95, 0x01,                 /*         Report Count (1) */
    0xB1, 0x02,                 /*         Feature (Data,Var,Abs) */
    0x75, 0x04,                 /*         Report Size (4) */
    0xB1, 0x03,                 /*         Feature (Const) - pad to a byte */
    0x35, 0x00,                 /*         Physical Minimum (0) */
    0x45, 0x00,                 /*         Physical Maximum (0) */
    0x05, 0x0C,                 /*         Usage Page (Consumer) */
    0x0A, 0x38, 0x02,           /*         Usage (AC Pan) */
    0x16, 0x01, 0x80,           /*         Logical Minimum (-32767) */
    0x26, 0xFF, 0x7F,           /*         Logical Maximum (32767) */
    0x75, 0x10,                 /*         Report Size (16) */
    0x95, 0x01,                 /*         Report Count (1) */
    0x81, 0x06,                 /*         Input (Data,Var,Rel) */
    0xC0,                       /*       End Collection */

    0xC0,                       /*     End Collection (Physical) */
    0xC0,                       /*   End Collection (Logical) */
    0xC0                        /* End Collection (Application) */
};

const USHORT g_MmReportDescriptorLength = (USHORT)sizeof(g_MmReportDescriptor);

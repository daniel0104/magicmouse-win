/*
 * mmproto.h - Apple Magic Mouse Bluetooth report formats.
 *
 * Layouts follow the Linux hid-magicmouse driver:
 *   Magic Mouse 1 (05AC:030D)   native report 0x10, touch report 0x29,
 *                               enable multitouch: SET_REPORT(Feature) D7 01
 *   Magic Mouse 2 (004C:0269)   report 0x12 (8 bytes native, 14+8n with touches),
 *   Magic Mouse USB-C (004C:0323) enable multitouch: SET_REPORT(Feature) F1 02 01
 *   0xF7 = two reports packed into one packet.
 */
#pragma once
#include "mmtypes.h"
#include "gesture.h"

#define MM1_NATIVE_REPORT_ID   0x10
#define MM1_TOUCH_REPORT_ID    0x29
#define MM2_REPORT_ID          0x12
#define MM_DOUBLE_REPORT_ID    0xF7

/* Bluetooth HID (HIDP) transaction headers */
#define HIDP_HANDSHAKE         0x00
#define HIDP_HID_CONTROL       0x10
#define HIDP_GET_REPORT        0x40
#define HIDP_SET_REPORT        0x50
#define HIDP_GET_PROTOCOL      0x60
#define HIDP_SET_PROTOCOL      0x70
#define HIDP_DATA              0xA0
#define HIDP_REPORT_INPUT      0x01
#define HIDP_REPORT_FEATURE    0x03

typedef enum _MM_MODEL {
    MM_MODEL_UNKNOWN = 0,
    MM_MODEL_MM1,
    MM_MODEL_MM2            /* Magic Mouse 2 and the 2024 USB-C model */
} MM_MODEL;

typedef enum _MM_REPORT_KIND {
    MM_KIND_OTHER = 0,      /* not a pointer report: pass through untouched */
    MM_KIND_NATIVE_MM1,     /* MM1 legacy mouse report: multitouch not enabled yet */
    MM_KIND_NATIVE_MM2,     /* MM2 8-byte mouse report (frame has dx/dy/clicks, no touches) */
    MM_KIND_TOUCH_MM1,      /* multitouch report(s), frames filled */
    MM_KIND_TOUCH_MM2
} MM_REPORT_KIND;

typedef struct _MM_FRAME {
    MM_TOUCH_IN Touches[MM_MAX_TOUCHES];
    ULONG       Count;
    LONG        Dx, Dy;
    UCHAR       Clicks;
} MM_FRAME;

/* report points at the report ID byte (HIDP header already stripped). */
MM_REPORT_KIND MmParseReport(const UCHAR *report, ULONG length,
                             MM_FRAME frames[2], ULONG *frameCount);

/* Builds the HIDP SET_REPORT(Feature) packet that switches on touch reports.
   Returns the packet length, 0 if the model is unknown. */
ULONG MmBuildEnablePacket(MM_MODEL model, UCHAR *buffer, ULONG bufferLength);

/* HID report descriptor for the virtual mouse we expose. */
extern const UCHAR  g_MmReportDescriptor[];
extern const USHORT g_MmReportDescriptorLength;

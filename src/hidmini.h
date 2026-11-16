/*
 * hidmini.h - the few HID class definitions enable.c needs.
 *
 * <hidclass.h> can't be included here: it redefines HID_XFER_PACKET, which
 * <vhf.h> (pulled in by driver.h) already defines, and the two collide in one
 * translation unit. These three definitions are stable public values.
 */
#pragma once

/* GUID_DEVINTERFACE_HID {4D1E55B2-F16F-11CF-88CB-001111000030} */
static const GUID MM_GUID_DEVINTERFACE_HID =
    { 0x4D1E55B2, 0xF16F, 0x11CF, { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };

typedef struct _MM_HID_COLLECTION_INFORMATION {
    ULONG   DescriptorSize;
    BOOLEAN Polled;
    UCHAR   Reserved1[1];
    USHORT  VendorID;
    USHORT  ProductID;
    USHORT  VersionNumber;
} MM_HID_COLLECTION_INFORMATION;

/* CTL_CODE(FILE_DEVICE_KEYBOARD=0x0B, 100, METHOD_BUFFERED,  FILE_ANY_ACCESS) */
#define MM_IOCTL_HID_GET_COLLECTION_INFORMATION   0x000B0190
/* CTL_CODE(FILE_DEVICE_KEYBOARD=0x0B, 100, METHOD_IN_DIRECT, FILE_ANY_ACCESS)
   IN_DIRECT: the report buffer (report ID first) is passed as the OUTPUT buffer. */
#define MM_IOCTL_HID_SET_FEATURE                  0x000B0191
/* CTL_CODE(FILE_DEVICE_KEYBOARD=0x0B, 100, METHOD_OUT_DIRECT, FILE_ANY_ACCESS) */
#define MM_IOCTL_HID_GET_FEATURE                  0x000B0192

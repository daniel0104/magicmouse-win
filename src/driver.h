/*
 * driver.h - MagicMouseFlt: KMDF lower filter for Apple Magic Mouse over Bluetooth.
 *
 * Device stack (per Magic Mouse devnode, BTHENUM\{00001124-...}_VID&..._PID&...):
 *
 *     hidclass / HidBth      (in-box Bluetooth HID function driver)
 *     MagicMouseFlt          <- this driver: sees every L2CAP packet as a BRB
 *     vhf                    (in-box Virtual HID Framework, creates our virtual mouse)
 *     BTHENUM PDO
 *
 * What the filter does
 *   1. Tracks the HID control and interrupt L2CAP channels HidBth opens.
 *   2. Sends SET_REPORT(Feature) on the control channel to switch the mouse into
 *      multitouch mode (the same thing macOS / Linux do).
 *   3. Takes the touch reports off the interrupt channel before HidBth sees them,
 *      runs the gesture engine and reports a full mouse (3 buttons, X/Y,
 *      hi-res wheel and hi-res horizontal pan) through a VHF virtual HID device.
 *      The in-box mouse collection goes quiet while multitouch mode is on.
 *   4. Everything else (battery reports, other traffic) passes through untouched.
 */
#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <bthdef.h>
#include <bthguid.h>
#include <bthioctl.h>
#include <bthddi.h>
#include <vhf.h>

#include "mmtypes.h"
#include "gesture.h"
#include "mmproto.h"

#define MM_POOL_TAG             'fMgM'
#define MM_MAX_CHANNELS         4
#ifndef PSM_HID_CONTROL
#define PSM_HID_CONTROL         0x0011
#endif
#ifndef PSM_HID_INTERRUPT
#define PSM_HID_INTERRUPT       0x0013
#endif
#define MM_ENABLE_DELAY_MS      500     /* after the channels open, like Linux */
#define MM_ENABLE_RETRY_MS      1000
#define MM_HANDSHAKE_WINDOW_MS  2000

/* MMLOG("format\n", args...) - the format must be a string literal. */
#if DBG
#define MMLOG(...) DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "MagicMouseFlt: " __VA_ARGS__)
#else
#define MMLOG(...) DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "MagicMouseFlt: " __VA_ARGS__)
#endif

typedef struct _MM_CHANNEL {
    L2CAP_CHANNEL_HANDLE Handle;
    USHORT               Psm;           /* 0 = not known yet */
} MM_CHANNEL;

/* Diagnostic counters, written to ...\MagicMouseFlt\Parameters\Status every 2 s
   (volatile key) and read by tools/Diagnose.ps1. */
typedef struct _MM_STATS {
    volatile LONG Version;
    volatile LONG Brbs, AclIn, AclOut, Opens, Closes, OtherIoctls;
    volatile LONG InputReports, Swallowed, VhfReports, VhfSubmitFailed;
    volatile LONG EnableSent, EnableFailed, Handshakes, LastHandshake;
    NTSTATUS QiStatus, VhfCreateStatus, VhfStartStatus, LastEnableStatus, LastVhfSubmitStatus;
    volatile LONG ReportIds[256];
    USHORT   BrbTypes[16];
    volatile LONG BrbTypeCount;
    UCHAR    LastInput[48];
    volatile LONG LastInputLen;
    UCHAR    LastControlIn[16];
    volatile LONG LastControlInLen;
} MM_STATS;

#define MM_STAT_INC(ctx, f) do { InterlockedIncrement(&(ctx)->Stats.f); InterlockedIncrement(&(ctx)->Stats.Version); } while (0)
#define MM_STAT_SET(ctx, f, v) do { (ctx)->Stats.f = (v); InterlockedIncrement(&(ctx)->Stats.Version); } while (0)

typedef struct _DEVICE_CONTEXT {
    WDFDEVICE           Device;
    WDFIOTARGET         Target;
    MM_MODEL            Model;
    MM_SETTINGS         Settings;

    /* Bluetooth profile interface (BRB helpers) */
    BTH_PROFILE_DRIVER_INTERFACE BthIf;
    BOOLEAN             BthIfValid;

    /* Everything below is protected by Lock */
    WDFSPINLOCK         Lock;
    BTH_ADDR            BtAddress;
    MM_CHANNEL          Channels[MM_MAX_CHANNELS];
    L2CAP_CHANNEL_HANDLE ControlChannel;
    L2CAP_CHANNEL_HANDLE InterruptChannel;
    BOOLEAN             MultitouchActive;
    BOOLEAN             ExpectHandshake;
    LONGLONG            LastEnableMs;
    MM_GESTURE          Gesture;
    BOOLEAN             InertiaTimerRunning;

    /* Our own SET_REPORT request */
    volatile LONG       FeatureInFlight;
    WDFREQUEST          FeatureRequest;
    WDFMEMORY           FeatureBrbMemory;
    struct _BRB_L2CA_ACL_TRANSFER FeatureBrb;
    UCHAR               FeatureBuffer[8];

    WDFTIMER            EnableTimer;
    WDFTIMER            InertiaTimer;

    /* Virtual HID mouse */
    VHFHANDLE           Vhf;
    volatile BOOLEAN    VhfStarted;

    /* Diagnostics */
    MM_STATS            Stats;
    LONG                StatsWritten;
    WDFTIMER            StatsTimer;
    WDFWORKITEM         StatsWorkItem;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, MmGetContext)

typedef struct _REQUEST_CONTEXT {
    PBRB    Brb;
    ULONG   OrigBufferSize;
    ULONG   OrigRemaining;
    USHORT  Psm;                        /* saved before send: shares a union with Response */
} REQUEST_CONTEXT, *PREQUEST_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(REQUEST_CONTEXT, MmGetRequestContext)

/* driver.c */
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD MmEvtDeviceAdd;
LONGLONG MmNowMs(VOID);
VOID MmSubmitReport(_In_ PDEVICE_CONTEXT Ctx, _In_reads_(MM_REPORT_LEN) PUCHAR Report);

/* bthfilter.c */
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL MmEvtIoInternalDeviceControl;
EVT_WDF_TIMER MmEvtEnableTimer;
EVT_WDF_TIMER MmEvtInertiaTimer;
VOID MmStatsNoteBrbType(_In_ PDEVICE_CONTEXT Ctx, _In_ USHORT Type);

/* stats.c */
EVT_WDF_TIMER MmEvtStatsTimer;
EVT_WDF_WORKITEM MmEvtStatsWorkItem;
VOID MmResetConnectionState(_In_ PDEVICE_CONTEXT Ctx);

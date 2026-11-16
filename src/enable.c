/*
 * enable.c - switch the mouse into multitouch mode.
 *
 * The earlier approach sent the enable as a raw L2CAP packet, which needs the
 * HID control channel handle. On the 2024 USB-C Magic Mouse that channel is
 * never opened through this filter (it attaches to a live connection), so the
 * handle can't be learned. Instead we hand the feature report to Windows' HID
 * stack: we open the device's own HID collection and send MM_IOCTL_HID_SET_FEATURE.
 * HidBth, which does know the control channel, puts it on the wire for us. The
 * request travels back down through this filter as an outgoing ACL transfer,
 * which we simply pass through.
 *
 * Enable feature reports (first byte is the report ID), from hid-magicmouse:
 *   Magic Mouse 1 (05AC:030D):            D7 01
 *   Magic Mouse 2 / USB-C (004C:0269/0323): F1 02 01
 */
#include "driver.h"
#include "hidmini.h"

#define MM_VID_APPLE_USB   0x05AC
#define MM_VID_APPLE_BT    0x004C
#define MM_PID_MM1         0x030D
#define MM_PID_MM2         0x0269
#define MM_PID_MM2C        0x0323

static BOOLEAN MmEnableBufferFor(_In_ USHORT Vid, _In_ USHORT Pid,
                                 _Out_writes_(4) UCHAR *Buf, _Out_ ULONG *Len)
{
    if ((Vid == MM_VID_APPLE_USB || Vid == MM_VID_APPLE_BT) && Pid == MM_PID_MM1) {
        Buf[0] = 0xD7; Buf[1] = 0x01; *Len = 2;
        return TRUE;
    }
    if ((Vid == MM_VID_APPLE_USB || Vid == MM_VID_APPLE_BT) && (Pid == MM_PID_MM2 || Pid == MM_PID_MM2C)) {
        Buf[0] = 0xF1; Buf[1] = 0x02; Buf[2] = 0x01; *Len = 3;
        return TRUE;
    }
    return FALSE;
}

/* Open one HID collection, check it's our mouse, and send the enable feature
   report. Returns TRUE if a feature report was accepted. */
static BOOLEAN MmEnableOneCollection(_In_ PDEVICE_CONTEXT Ctx, _In_ PUNICODE_STRING SymbolicName)
{
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_IO_TARGET_OPEN_PARAMS openParams;
    WDFIOTARGET target = NULL;
    WDF_MEMORY_DESCRIPTOR desc;
    MM_HID_COLLECTION_INFORMATION info;
    UCHAR enableBuf[4] = { 0 };
    ULONG enableLen = 0;
    NTSTATUS status;
    BOOLEAN sent = FALSE;

    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = Ctx->Device;
    status = WdfIoTargetCreate(Ctx->Device, &attr, &target);
    if (!NT_SUCCESS(status)) {
        Ctx->Stats.LastHidEnumStatus = status;
        return FALSE;
    }

    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&openParams, SymbolicName,
                                                STANDARD_RIGHTS_ALL | FILE_READ_DATA | FILE_WRITE_DATA);
    openParams.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
    status = WdfIoTargetOpen(target, &openParams);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(target);
        return FALSE;      /* not openable (e.g. someone holds it exclusive): skip */
    }

    RtlZeroMemory(&info, sizeof(info));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&desc, &info, sizeof(info));
    status = WdfIoTargetSendIoctlSynchronously(target, NULL, MM_IOCTL_HID_GET_COLLECTION_INFORMATION,
                                               NULL, &desc, NULL, NULL);
    Ctx->Stats.LastHidInfoStatus = status;
    if (!NT_SUCCESS(status)) {
        goto done;
    }

    if (!MmEnableBufferFor(info.VendorID, info.ProductID, enableBuf, &enableLen)) {
        goto done;         /* not an Apple mouse collection */
    }

    InterlockedIncrement(&Ctx->Stats.HidCollectionsMatched);
    Ctx->Stats.LastHidVidPid = ((LONG)info.VendorID << 16) | info.ProductID;
    MM_STAT_SET(Ctx, LastHidVidPid, Ctx->Stats.LastHidVidPid);

    /* MM_IOCTL_HID_SET_FEATURE carries the report (report ID first) in the output
       buffer (the IOCTL is METHOD_IN_DIRECT). */
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&desc, enableBuf, enableLen);
    status = WdfIoTargetSendIoctlSynchronously(target, NULL, MM_IOCTL_HID_SET_FEATURE,
                                               NULL, &desc, NULL, NULL);
    Ctx->Stats.LastHidSetFeatureStatus = status;
    if (NT_SUCCESS(status)) {
        InterlockedIncrement(&Ctx->Stats.HidSetFeatureOk);
        MMLOG("multitouch enabled via HID SET_FEATURE (%04X:%04X, %lu bytes)\n",
              info.VendorID, info.ProductID, enableLen);
        sent = TRUE;
    } else {
        MMLOG("HID SET_FEATURE failed 0x%08X (%04X:%04X)\n", status, info.VendorID, info.ProductID);
    }

done:
    WdfIoTargetClose(target);
    WdfObjectDelete(target);
    return sent;
}

VOID MmEvtEnableWorkItem(_In_ WDFWORKITEM WorkItem)
{
    PDEVICE_CONTEXT ctx = MmGetContext(WdfWorkItemGetParentObject(WorkItem));
    PWSTR list = NULL, p;
    UNICODE_STRING name;
    NTSTATUS status;
    ULONG found = 0;

    if (ctx->MultitouchActive) {
        InterlockedExchange(&ctx->EnableInProgress, 0);
        return;
    }

    MM_STAT_INC(ctx, HidEnableAttempts);

    /* All HID collections currently present. The device's own collections are
       among them; we match ours by Apple VID/PID rather than by instance path. */
    status = IoGetDeviceInterfaces((LPGUID)&MM_GUID_DEVINTERFACE_HID, NULL, 0, &list);
    ctx->Stats.LastHidEnumStatus = status;
    if (!NT_SUCCESS(status) || list == NULL) {
        InterlockedExchange(&ctx->EnableInProgress, 0);
        return;
    }

    for (p = list; *p != L'\0'; p += (wcslen(p) + 1)) {
        found++;
        RtlInitUnicodeString(&name, p);
        if (MmEnableOneCollection(ctx, &name)) {
            break;
        }
        if (ctx->MultitouchActive) {
            break;
        }
    }
    ctx->Stats.HidCollectionsFound = (LONG)found;
    MM_STAT_SET(ctx, HidCollectionsFound, (LONG)found);

    ExFreePool(list);
    InterlockedExchange(&ctx->EnableInProgress, 0);
}

/* Called when a legacy (non-multitouch) report is seen. Schedules one enable
   attempt at PASSIVE_LEVEL, throttled so retries don't pile up. */
VOID MmRequestEnable(_In_ PDEVICE_CONTEXT Ctx)
{
    LONGLONG now = MmNowMs();

    if (Ctx->MultitouchActive || Ctx->EnableWorkItem == NULL) {
        return;
    }
    WdfSpinLockAcquire(Ctx->Lock);
    if (Ctx->LastEnableMs != 0 && (now - Ctx->LastEnableMs) < MM_ENABLE_RETRY_MS) {
        WdfSpinLockRelease(Ctx->Lock);
        return;
    }
    Ctx->LastEnableMs = now;
    WdfSpinLockRelease(Ctx->Lock);

    if (InterlockedCompareExchange(&Ctx->EnableInProgress, 1, 0) == 0) {
        WdfWorkItemEnqueue(Ctx->EnableWorkItem);
    }
}

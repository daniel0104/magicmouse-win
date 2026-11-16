/*
 * enable.c - switch the mouse into multitouch mode.
 *
 * The control channel is never opened through this filter and a second one is
 * refused, so the enable SET_REPORT can't reach the mouse directly. But a HID
 * feature *read* on the mouse's own collection makes HidBth run a GET_REPORT on
 * its control channel - and that traffic does pass through this filter, so we
 * learn the control channel's handle from it. Once we have the handle, we send
 * the real enable SET_REPORT ("53 F1 02 01") on it as a raw L2CAP transfer,
 * which Windows' HID layer would never allow (report 0xF1 isn't in the parsed
 * descriptor). The multitouch reports then arrive on HidBth's interrupt channel,
 * which the filter already intercepts.
 *
 * "Baiting": open the device's HID collection (any access) and issue
 * IOCTL_HID_GET_FEATURE for report 0x55 (the 64-byte vendor feature in the mouse
 * collection). HidBth transacts on the control channel; the BRB dispatch sees
 * the outgoing transfer and records that channel as the control channel.
 */
#include "driver.h"
#include "hidmini.h"

#define MM_BAIT_REPORT_ID   0x55
#define MM_BAIT_REPORT_LEN  65

static BOOLEAN MmWStrContainsI(_In_z_ PCWSTR Str, _In_z_ PCWSTR Pat)
{
    SIZE_T i, j, n = 0, m = 0;
    while (Str[n] != L'\0') n++;
    while (Pat[m] != L'\0') m++;
    for (i = 0; m > 0 && i + m <= n; i++) {
        for (j = 0; j < m; j++) {
            if (RtlUpcaseUnicodeChar(Str[i + j]) != RtlUpcaseUnicodeChar(Pat[j])) break;
        }
        if (j == m) return TRUE;
    }
    return FALSE;
}

static BOOLEAN MmPathIsMagicMouse(_In_z_ PCWSTR Path)
{
    return MmWStrContainsI(Path, L"pid&0323") ||   /* USB-C 2024 */
           MmWStrContainsI(Path, L"pid&0269") ||   /* Magic Mouse 2 */
           MmWStrContainsI(Path, L"pid&030d");     /* Magic Mouse 1 */
}

/* Feature-read one collection to force control-channel traffic. */
static VOID MmBaitOne(_In_ PDEVICE_CONTEXT Ctx, _In_ PUNICODE_STRING Name)
{
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_IO_TARGET_OPEN_PARAMS openParams;
    WDFIOTARGET target = NULL;
    WDF_MEMORY_DESCRIPTOR desc;
    UCHAR buffer[MM_BAIT_REPORT_LEN];
    NTSTATUS status;
    ULONG access;

    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = Ctx->Device;
    if (!NT_SUCCESS(WdfIoTargetCreate(Ctx->Device, &attr, &target))) {
        return;
    }

    /* The mouse collection only opens with minimal (query) access; mouhid holds
       it for read/write. Try 0, then read. */
    status = STATUS_UNSUCCESSFUL;
    for (access = 0; ; access = FILE_READ_ACCESS) {
        WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&openParams, Name, access);
        openParams.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
        status = WdfIoTargetOpen(target, &openParams);
        if (NT_SUCCESS(status) || access == FILE_READ_ACCESS) {
            break;
        }
    }
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(target);
        return;
    }

    RtlZeroMemory(buffer, sizeof(buffer));
    buffer[0] = MM_BAIT_REPORT_ID;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&desc, buffer, sizeof(buffer));
    /* GET_FEATURE is METHOD_OUT_DIRECT: report buffer is the output buffer. */
    status = WdfIoTargetSendIoctlSynchronously(target, NULL, MM_IOCTL_HID_GET_FEATURE,
                                               NULL, &desc, NULL, NULL);
    Ctx->Stats.LastHidInfoStatus = status;
    if (NT_SUCCESS(status)) {
        InterlockedIncrement(&Ctx->Stats.HidSetFeatureOk);   /* reuse as "bait ok" */
    }
    WdfIoTargetClose(target);
    WdfObjectDelete(target);
}

VOID MmEvtEnableWorkItem(_In_ WDFWORKITEM WorkItem)
{
    PDEVICE_CONTEXT ctx = MmGetContext(WdfWorkItemGetParentObject(WorkItem));
    PWSTR list = NULL, p;
    UNICODE_STRING name;
    ULONG found = 0;

    if (ctx->MultitouchActive) {
        InterlockedExchange(&ctx->EnableInProgress, 0);
        return;
    }

    MM_STAT_INC(ctx, HidEnableAttempts);

    /* Bait: make HidBth transact on the control channel so we learn its handle. */
    if (ctx->ControlChannel == NULL &&
        NT_SUCCESS(IoGetDeviceInterfaces((LPGUID)&MM_GUID_DEVINTERFACE_HID, NULL, 0, &list)) &&
        list != NULL) {
        for (p = list; *p != L'\0'; p += (wcslen(p) + 1)) {
            if (MmPathIsMagicMouse(p)) {
                found++;
                RtlInitUnicodeString(&name, p);
                MmBaitOne(ctx, &name);
                if (ctx->ControlChannel != NULL) {
                    break;
                }
            }
        }
        ExFreePool(list);
    }
    MM_STAT_SET(ctx, HidCollectionsFound, (LONG)found);
    MM_STAT_SET(ctx, OwnChannelState, ctx->ControlChannel != NULL ? 2 : 3);

    /* Whether or not the bait ran, if the control channel is known, enable now. */
    if (ctx->ControlChannel != NULL) {
        MmMaybeEnableMultitouch(ctx);
    }
    InterlockedExchange(&ctx->EnableInProgress, 0);
}

/* No-op kept so reset/D0-exit callers stay valid (we no longer open our own channel). */
VOID MmCloseOwnControlChannel(_In_ PDEVICE_CONTEXT Ctx)
{
    UNREFERENCED_PARAMETER(Ctx);
}

/* Called from the data path (DISPATCH) when a legacy report is seen. */
VOID MmRequestEnable(_In_ PDEVICE_CONTEXT Ctx)
{
    LONGLONG now = MmNowMs();
    ULONG method;

    if (Ctx->MultitouchActive || Ctx->EnableWorkItem == NULL) {
        return;
    }

    WdfSpinLockAcquire(Ctx->Lock);
    method = Ctx->Settings.EnableMethod;
    WdfSpinLockRelease(Ctx->Lock);

    /* Control channel known (learned via bait/traffic), or method 0 (interrupt
       channel): send directly at DISPATCH. */
    if (Ctx->ControlChannel != NULL || method == 0) {
        MmMaybeEnableMultitouch(Ctx);
        return;
    }

    /* Method 1, control channel not yet known: bait for it at PASSIVE, throttled. */
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

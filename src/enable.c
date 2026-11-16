/*
 * enable.c - open our own L2CAP HID-control channel to the mouse and send the
 * multitouch-enable SET_REPORT on it (EnableMethod 1, the default).
 *
 * Why: the mouse's control channel is never opened through this filter, so its
 * handle can't be learned passively; the enable feature report can't go through
 * the Windows HID stack either (mouhid owns the collection that carries it); and
 * the mouse ignores the feature command on the interrupt channel. So we open a
 * second L2CAP control channel (PSM 0x11) ourselves - the same thing the Linux
 * tooling does - and send the enable there. HidBth's own channels are left
 * alone, so if the mouse refuses a second control channel the open just fails.
 *
 * Once enabled, the long multitouch reports arrive on HidBth's interrupt
 * channel, which the filter already intercepts. We keep our control channel open
 * for the connection's lifetime (some devices drop multitouch when it closes).
 */
#include "driver.h"

#ifndef PSM_HID_CONTROL
#define PSM_HID_CONTROL 0x0011
#endif

static VOID MmL2capCallback(_In_ PVOID Context, _In_ INDICATION_CODE Indication,
                            _In_ PINDICATION_PARAMETERS Parameters)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;

    UNREFERENCED_PARAMETER(Parameters);
    switch (Indication) {
    case IndicationAddReference:
    case IndicationReleaseReference:
        break;
    case IndicationRemoteDisconnect:
        InterlockedExchange(&ctx->OwnChannelState, 0);
        WdfSpinLockAcquire(ctx->Lock);
        if (ctx->ControlChannel == ctx->OwnControlChannel) {
            ctx->ControlChannel = NULL;
        }
        ctx->OwnControlChannel = NULL;
        WdfSpinLockRelease(ctx->Lock);
        MMLOG("own control channel disconnected by device\n");
        break;
    default:
        break;
    }
}

static NTSTATUS MmOpenOwnControlChannel(_In_ PDEVICE_CONTEXT Ctx)
{
    WDF_MEMORY_DESCRIPTOR desc;
    struct _BRB_L2CA_OPEN_CHANNEL *brb = &Ctx->OpenBrb;
    BTH_ADDR address;
    NTSTATUS status;

    WdfSpinLockAcquire(Ctx->Lock);
    address = Ctx->BtAddress;
    WdfSpinLockRelease(Ctx->Lock);
    if (address == 0 || !Ctx->BthIfValid) {
        return STATUS_DEVICE_NOT_READY;
    }

    Ctx->BthIf.BthInitializeBrb((PBRB)brb, BRB_L2CA_OPEN_CHANNEL);
    brb->BtAddress     = address;
    brb->Psm           = PSM_HID_CONTROL;
    brb->ChannelFlags  = CF_ROLE_EITHER;
    brb->ConfigIn.Flags = 0;
    brb->CallbackFlags = CALLBACK_DISCONNECT;
    brb->Callback      = MmL2capCallback;
    brb->CallbackContext = Ctx;
    brb->ReferenceObject = (PVOID)WdfDeviceWdmGetDeviceObject(Ctx->Device);
    brb->IncomingQueueDepth = 1;

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&desc, brb, sizeof(*brb));
    status = WdfIoTargetSendInternalIoctlOthersSynchronously(Ctx->Target, NULL,
                 IOCTL_INTERNAL_BTH_SUBMIT_BRB, &desc, NULL, NULL, NULL, NULL);
    if (NT_SUCCESS(status)) {
        status = brb->Hdr.Status;
    }
    Ctx->Stats.OwnChannelOpenStatus = status;

    if (NT_SUCCESS(status) && brb->ChannelHandle != NULL) {
        WdfSpinLockAcquire(Ctx->Lock);
        Ctx->OwnControlChannel = brb->ChannelHandle;
        Ctx->ControlChannel = brb->ChannelHandle;   /* the normal enable path now targets it */
        WdfSpinLockRelease(Ctx->Lock);
        MMLOG("opened own control channel\n");
        return STATUS_SUCCESS;
    }
    MMLOG("own control channel open failed 0x%08X\n", status);
    return NT_SUCCESS(status) ? STATUS_UNSUCCESSFUL : status;
}

VOID MmEvtEnableWorkItem(_In_ WDFWORKITEM WorkItem)
{
    PDEVICE_CONTEXT ctx = MmGetContext(WdfWorkItemGetParentObject(WorkItem));
    NTSTATUS status;

    if (ctx->MultitouchActive || ctx->OwnControlChannel != NULL) {
        InterlockedExchange(&ctx->EnableInProgress, 0);
        MM_STAT_SET(ctx, OwnChannelState, ctx->OwnChannelState);
        return;
    }

    status = MmOpenOwnControlChannel(ctx);
    InterlockedExchange(&ctx->OwnChannelState, NT_SUCCESS(status) ? 2 : 3);
    MM_STAT_SET(ctx, OwnChannelState, ctx->OwnChannelState);

    if (NT_SUCCESS(status)) {
        MmMaybeEnableMultitouch(ctx);     /* sends the SET_REPORT on the new channel */
    }
    InterlockedExchange(&ctx->EnableInProgress, 0);
}

VOID MmCloseOwnControlChannel(_In_ PDEVICE_CONTEXT Ctx)
{
    WDF_MEMORY_DESCRIPTOR desc;
    L2CAP_CHANNEL_HANDLE handle;

    WdfSpinLockAcquire(Ctx->Lock);
    handle = Ctx->OwnControlChannel;
    Ctx->OwnControlChannel = NULL;
    if (Ctx->ControlChannel == handle) {
        Ctx->ControlChannel = NULL;
    }
    WdfSpinLockRelease(Ctx->Lock);

    if (handle == NULL || !Ctx->BthIfValid) {
        return;
    }
    Ctx->BthIf.BthInitializeBrb((PBRB)&Ctx->CloseBrb, BRB_L2CA_CLOSE_CHANNEL);
    Ctx->CloseBrb.ChannelHandle = handle;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&desc, &Ctx->CloseBrb, sizeof(Ctx->CloseBrb));
    (VOID)WdfIoTargetSendInternalIoctlOthersSynchronously(Ctx->Target, NULL,
              IOCTL_INTERNAL_BTH_SUBMIT_BRB, &desc, NULL, NULL, NULL, NULL);
    InterlockedExchange(&Ctx->OwnChannelState, 0);
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

    /* Method 0, or once our channel is open: send on the known channel directly. */
    if (method == 0 || Ctx->OwnControlChannel != NULL) {
        MmMaybeEnableMultitouch(Ctx);
        return;
    }

    /* Method 1: open our own control channel first, at PASSIVE, throttled. */
    WdfSpinLockAcquire(Ctx->Lock);
    if (Ctx->LastEnableMs != 0 && (now - Ctx->LastEnableMs) < MM_ENABLE_RETRY_MS) {
        WdfSpinLockRelease(Ctx->Lock);
        return;
    }
    Ctx->LastEnableMs = now;
    WdfSpinLockRelease(Ctx->Lock);

    if (InterlockedCompareExchange(&Ctx->EnableInProgress, 1, 0) == 0) {
        InterlockedExchange(&Ctx->OwnChannelState, 1);
        WdfWorkItemEnqueue(Ctx->EnableWorkItem);
    }
}

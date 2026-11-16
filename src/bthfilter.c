/*
 * bthfilter.c - Bluetooth Request Block (BRB) interception.
 *
 * HidBth talks to the mouse with IOCTL_INTERNAL_BTH_SUBMIT_BRB:
 *   BRB_L2CA_OPEN_CHANNEL / _RESPONSE  open the HID control (PSM 0x11) and
 *                                      interrupt (PSM 0x13) channels
 *   BRB_L2CA_ACL_TRANSFER              read/write HIDP packets
 *   BRB_L2CA_CLOSE_CHANNEL             tear down
 *
 * Every incoming HIDP packet passes through MmAclInComplete. Pointer and touch
 * reports are consumed here (the read is re-submitted so HidBth never sees
 * them); everything else is completed back to HidBth unchanged.
 */
#include "driver.h"

static EVT_WDF_REQUEST_COMPLETION_ROUTINE MmAclInComplete;
static EVT_WDF_REQUEST_COMPLETION_ROUTINE MmOpenChannelComplete;
static EVT_WDF_REQUEST_COMPLETION_ROUTINE MmFeatureComplete;

static VOID MmMaybeEnableMultitouch(_In_ PDEVICE_CONTEXT Ctx);

/* ------------------------------------------------------------------------ */
/* helpers */

static VOID MmPassThrough(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST Request)
{
    WDF_REQUEST_SEND_OPTIONS options;

    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
    if (!WdfRequestSend(Request, Ctx->Target, &options)) {
        WdfRequestComplete(Request, WdfRequestGetStatus(Request));
    }
}

static VOID MmSendWithCompletion(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST Request,
                                 _In_ PFN_WDF_REQUEST_COMPLETION_ROUTINE Routine)
{
    WdfRequestFormatRequestUsingCurrentType(Request);
    WdfRequestSetCompletionRoutine(Request, Routine, Ctx);
    if (!WdfRequestSend(Request, Ctx->Target, WDF_NO_SEND_OPTIONS)) {
        WdfRequestComplete(Request, WdfRequestGetStatus(Request));
    }
}

static PUCHAR MmAclBuffer(_In_ struct _BRB_L2CA_ACL_TRANSFER *Acl)
{
    if (Acl->Buffer != NULL) {
        return (PUCHAR)Acl->Buffer;
    }
    if (Acl->BufferMDL != NULL) {
        return (PUCHAR)MmGetSystemAddressForMdlSafe(Acl->BufferMDL, NormalPagePriority | MdlMappingNoExecute);
    }
    return NULL;
}

/* Lock held. Work out which open channel is the control channel. */
static VOID MmRecomputeRolesLocked(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONG i;
    L2CAP_CHANNEL_HANDLE control = NULL;

    for (i = 0; i < MM_MAX_CHANNELS; i++) {
        if (Ctx->Channels[i].Handle != NULL && Ctx->Channels[i].Psm == PSM_HID_INTERRUPT) {
            Ctx->InterruptChannel = Ctx->Channels[i].Handle;
        }
        if (Ctx->Channels[i].Handle != NULL && Ctx->Channels[i].Psm == PSM_HID_CONTROL) {
            control = Ctx->Channels[i].Handle;
        }
    }
    /* PSM unknown (the mouse connected to us): HID opens control first. */
    for (i = 0; control == NULL && i < MM_MAX_CHANNELS; i++) {
        if (Ctx->Channels[i].Handle != NULL &&
            Ctx->Channels[i].Handle != Ctx->InterruptChannel &&
            Ctx->Channels[i].Psm != PSM_HID_INTERRUPT) {
            control = Ctx->Channels[i].Handle;
        }
    }
    Ctx->ControlChannel = control;
}

/* Lock held. */
static ULONG MmAddChannelLocked(_In_ PDEVICE_CONTEXT Ctx, _In_ L2CAP_CHANNEL_HANDLE Handle, _In_ USHORT Psm)
{
    ULONG i, count = 0, freeSlot = MM_MAX_CHANNELS;

    for (i = 0; i < MM_MAX_CHANNELS; i++) {
        if (Ctx->Channels[i].Handle == Handle) {
            if (Psm != 0) {
                Ctx->Channels[i].Psm = Psm;
            }
            freeSlot = MM_MAX_CHANNELS + 1;      /* already present */
        } else if (Ctx->Channels[i].Handle == NULL && freeSlot == MM_MAX_CHANNELS) {
            freeSlot = i;
        }
    }
    if (freeSlot < MM_MAX_CHANNELS) {
        Ctx->Channels[freeSlot].Handle = Handle;
        Ctx->Channels[freeSlot].Psm = Psm;
    }
    for (i = 0; i < MM_MAX_CHANNELS; i++) {
        if (Ctx->Channels[i].Handle != NULL) {
            count++;
        }
    }
    MmRecomputeRolesLocked(Ctx);
    return count;
}

VOID MmResetConnectionState(_In_ PDEVICE_CONTEXT Ctx)
{
    UCHAR report[MM_REPORT_LEN];
    BOOLEAN release;

    WdfSpinLockAcquire(Ctx->Lock);
    RtlZeroMemory(Ctx->Channels, sizeof(Ctx->Channels));
    Ctx->ControlChannel = NULL;
    Ctx->InterruptChannel = NULL;
    Ctx->MultitouchActive = FALSE;
    Ctx->ExpectHandshake = FALSE;
    Ctx->InertiaTimerRunning = FALSE;
    release = MmGestureReset(&Ctx->Gesture, report);
    WdfSpinLockRelease(Ctx->Lock);

    if (release) {
        MmSubmitReport(Ctx, report);
    }
}

/* ------------------------------------------------------------------------ */
/* Dispatch */

VOID
MmEvtIoInternalDeviceControl(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request,
                             _In_ size_t OutputBufferLength, _In_ size_t InputBufferLength,
                             _In_ ULONG IoControlCode)
{
    PDEVICE_CONTEXT ctx = MmGetContext(WdfIoQueueGetDevice(Queue));
    PREQUEST_CONTEXT rc = MmGetRequestContext(Request);
    WDF_REQUEST_PARAMETERS params;
    PBRB brb;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    if (IoControlCode != IOCTL_INTERNAL_BTH_SUBMIT_BRB) {
        MM_STAT_INC(ctx, OtherIoctls);
        MmPassThrough(ctx, Request);
        return;
    }

    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);
    brb = (PBRB)params.Parameters.Others.Arg1;
    if (brb == NULL) {
        MmPassThrough(ctx, Request);
        return;
    }
    rc->Brb = brb;
    MM_STAT_INC(ctx, Brbs);
    MmStatsNoteBrbType(ctx, brb->BrbHeader.Type);

    switch (brb->BrbHeader.Type) {

    case BRB_L2CA_ACL_TRANSFER: {
        struct _BRB_L2CA_ACL_TRANSFER *acl = &brb->BrbL2caAclTransfer;

        if (acl->TransferFlags & ACL_TRANSFER_DIRECTION_IN) {
            rc->OrigBufferSize = acl->BufferSize;
            rc->OrigRemaining = acl->RemainingBufferSize;
            MmSendWithCompletion(ctx, Request, MmAclInComplete);
            return;
        }

        /* Outgoing traffic reveals the control channel and the mouse address.
           Anything HidBth sends on a channel that isn't the interrupt channel is
           on the control channel - this is how we recover the handle when we
           attached to a connection that was already open (no OPEN_CHANNEL seen). */
        MM_STAT_INC(ctx, AclOut);
        {
            PUCHAR data = (acl->BufferSize >= 1) ? MmAclBuffer(acl) : NULL;
            BOOLEAN isControl = FALSE;
            if (data != NULL) {
                UCHAR type = data[0] & 0xF0;
                if (type == HIDP_HID_CONTROL || type == HIDP_GET_REPORT || type == HIDP_SET_REPORT ||
                    type == HIDP_GET_PROTOCOL || type == HIDP_SET_PROTOCOL) {
                    isControl = TRUE;
                }
            }
            WdfSpinLockAcquire(ctx->Lock);
            if (acl->BtAddress != 0) {
                ctx->BtAddress = acl->BtAddress;
            }
            if (isControl || (ctx->InterruptChannel != NULL && acl->ChannelHandle != ctx->InterruptChannel)) {
                (VOID)MmAddChannelLocked(ctx, acl->ChannelHandle, PSM_HID_CONTROL);
            }
            WdfSpinLockRelease(ctx->Lock);
        }
        break;
    }

    case BRB_L2CA_OPEN_CHANNEL:
        rc->Psm = brb->BrbL2caOpenChannel.Psm;
        MmSendWithCompletion(ctx, Request, MmOpenChannelComplete);
        return;

    case BRB_L2CA_OPEN_CHANNEL_RESPONSE:
        rc->Psm = 0;
        MmSendWithCompletion(ctx, Request, MmOpenChannelComplete);
        return;

    case BRB_L2CA_CLOSE_CHANNEL: {
        L2CAP_CHANNEL_HANDLE handle = brb->BrbL2caCloseChannel.ChannelHandle;
        BOOLEAN lostInterrupt = FALSE;
        ULONG i;

        MM_STAT_INC(ctx, Closes);
        WdfSpinLockAcquire(ctx->Lock);
        for (i = 0; i < MM_MAX_CHANNELS; i++) {
            if (ctx->Channels[i].Handle == handle) {
                ctx->Channels[i].Handle = NULL;
                ctx->Channels[i].Psm = 0;
            }
        }
        if (handle == ctx->InterruptChannel) {
            ctx->InterruptChannel = NULL;
            lostInterrupt = TRUE;
        }
        MmRecomputeRolesLocked(ctx);
        WdfSpinLockRelease(ctx->Lock);

        if (lostInterrupt) {
            MMLOG("interrupt channel closed\n");
            MmResetConnectionState(ctx);
        }
        break;
    }

    default:
        break;
    }

    MmPassThrough(ctx, Request);
}

/* ------------------------------------------------------------------------ */
/* Channel open */

static VOID
MmOpenChannelComplete(_In_ WDFREQUEST Request, _In_ WDFIOTARGET Target,
                      _In_ PWDF_REQUEST_COMPLETION_PARAMS Params, _In_ WDFCONTEXT Context)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;
    PREQUEST_CONTEXT rc = MmGetRequestContext(Request);
    struct _BRB_L2CA_OPEN_CHANNEL *oc = &rc->Brb->BrbL2caOpenChannel;
    NTSTATUS status = Params->IoStatus.Status;

    UNREFERENCED_PARAMETER(Target);

    if (NT_SUCCESS(status) && oc->ChannelHandle != NULL) {
        BOOLEAN accepted = (oc->Hdr.Type == BRB_L2CA_OPEN_CHANNEL) ||
                           (oc->Response == CONNECT_RSP_RESULT_SUCCESS);
        if (accepted) {
            ULONG count;
            WdfSpinLockAcquire(ctx->Lock);
            if (oc->BtAddress != 0) {
                ctx->BtAddress = oc->BtAddress;
            }
            count = MmAddChannelLocked(ctx, oc->ChannelHandle, rc->Psm);
            MM_STAT_INC(ctx, Opens);
            ctx->MultitouchActive = FALSE;      /* new connection: mouse starts in legacy mode */
            WdfSpinLockRelease(ctx->Lock);

            MMLOG("channel open psm=0x%04X count=%lu\n", rc->Psm, count);
            if (count >= 2) {
                WdfTimerStart(ctx->EnableTimer, WDF_REL_TIMEOUT_IN_MS(MM_ENABLE_DELAY_MS));
            }
        }
    }

    WdfRequestCompleteWithInformation(Request, status, Params->IoStatus.Information);
}

/* ------------------------------------------------------------------------ */
/* Incoming data */

typedef enum { MM_PASS = 0, MM_SWALLOW } MM_ACTION;

static MM_ACTION
MmHandleIncoming(_In_ PDEVICE_CONTEXT Ctx, _In_ L2CAP_CHANNEL_HANDLE Channel,
                 _In_reads_(Length) PUCHAR Data, _In_ ULONG Length)
{
    MM_FRAME frames[2];
    UCHAR reports[2][MM_REPORT_LEN];
    BOOLEAN send[2] = { FALSE, FALSE };
    ULONG frameCount = 0, i;
    MM_REPORT_KIND kind;
    MM_ACTION action = MM_PASS;
    BOOLEAN wantEnable = FALSE, startInertia = FALSE;
    LONGLONG now;

    if (Length < 1) {
        return MM_PASS;
    }
    now = MmNowMs();

    /* HANDSHAKE answering our SET_REPORT: HidBth didn't ask for it, so eat it. */
    if ((Data[0] & 0xF0) == HIDP_HANDSHAKE && Length == 1) {
        WdfSpinLockAcquire(Ctx->Lock);
        if (Ctx->ExpectHandshake && Channel == Ctx->ControlChannel &&
            now - Ctx->LastEnableMs <= MM_HANDSHAKE_WINDOW_MS) {
            Ctx->ExpectHandshake = FALSE;
            action = MM_SWALLOW;
            Ctx->Stats.LastHandshake = Data[0] & 0x0F;
            MM_STAT_INC(Ctx, Handshakes);
        }
        WdfSpinLockRelease(Ctx->Lock);
        if (action == MM_SWALLOW) {
            MMLOG("multitouch enable handshake result %u\n", (unsigned)(Data[0] & 0x0F));
        }
        return action;
    }

    if (Data[0] == (HIDP_DATA | HIDP_REPORT_INPUT) && Length >= 2) {
        ULONG n = MM_MIN(Length, (ULONG)sizeof(Ctx->Stats.LastInput));
        RtlCopyMemory(Ctx->Stats.LastInput, Data, n);
        Ctx->Stats.LastInputLen = (LONG)Length;
        InterlockedIncrement(&Ctx->Stats.ReportIds[Data[1]]);
        MM_STAT_INC(Ctx, InputReports);
    } else {
        ULONG n = MM_MIN(Length, (ULONG)sizeof(Ctx->Stats.LastControlIn));
        RtlCopyMemory(Ctx->Stats.LastControlIn, Data, n);
        Ctx->Stats.LastControlInLen = (LONG)Length;
        InterlockedIncrement(&Ctx->Stats.Version);
    }

    if (Data[0] != (HIDP_DATA | HIDP_REPORT_INPUT) || Length < 2 || !Ctx->VhfStarted) {
        return MM_PASS;
    }

    kind = MmParseReport(Data + 1, Length - 1, frames, &frameCount);
    if (kind == MM_KIND_OTHER) {
        return MM_PASS;
    }

    WdfSpinLockAcquire(Ctx->Lock);

    for (i = 0; i < MM_MAX_CHANNELS; i++) {
        if (Ctx->Channels[i].Handle == Channel && Ctx->Channels[i].Psm == PSM_HID_CONTROL) {
            /* A GET_REPORT reply on the control channel: not ours. */
            WdfSpinLockRelease(Ctx->Lock);
            return MM_PASS;
        }
    }
    if (Channel != Ctx->InterruptChannel) {
        Ctx->InterruptChannel = Channel;
        MmRecomputeRolesLocked(Ctx);
    }

    switch (kind) {
    case MM_KIND_NATIVE_MM1:
        if (Ctx->Model == MM_MODEL_UNKNOWN) {
            Ctx->Model = MM_MODEL_MM1;
        }
        wantEnable = !Ctx->MultitouchActive;
        break;                                          /* HidBth handles it */

    case MM_KIND_NATIVE_MM2:
        if (Ctx->Model == MM_MODEL_UNKNOWN) {
            Ctx->Model = MM_MODEL_MM2;
        }
        if (!Ctx->MultitouchActive) {
            wantEnable = TRUE;                          /* HidBth handles it */
            break;
        }
        /* In multitouch mode, short reports go through the gesture engine too. */
        send[0] = MmGestureFrame(&Ctx->Gesture, &Ctx->Settings, now, NULL, 0,
                                 frames[0].Dx, frames[0].Dy, frames[0].Clicks, reports[0]);
        action = MM_SWALLOW;
        break;

    case MM_KIND_TOUCH_MM1:
    case MM_KIND_TOUCH_MM2:
        if (Ctx->Model == MM_MODEL_UNKNOWN) {
            Ctx->Model = (kind == MM_KIND_TOUCH_MM1) ? MM_MODEL_MM1 : MM_MODEL_MM2;
        }
        if (!Ctx->MultitouchActive) {
            Ctx->MultitouchActive = TRUE;
            MMLOG("multitouch reports active\n");
        }
        for (i = 0; i < frameCount; i++) {
            send[i] = MmGestureFrame(&Ctx->Gesture, &Ctx->Settings, now,
                                     frames[i].Touches, frames[i].Count,
                                     frames[i].Dx, frames[i].Dy, frames[i].Clicks, reports[i]);
        }
        action = MM_SWALLOW;
        break;

    default:
        break;
    }

    if (Ctx->Gesture.InertiaActive && !Ctx->InertiaTimerRunning) {
        Ctx->InertiaTimerRunning = TRUE;
        startInertia = TRUE;
    }

    WdfSpinLockRelease(Ctx->Lock);

    for (i = 0; i < 2; i++) {
        if (send[i]) {
            MmSubmitReport(Ctx, reports[i]);
        }
    }
    if (startInertia) {
        WdfTimerStart(Ctx->InertiaTimer, WDF_REL_TIMEOUT_IN_MS(MM_INERTIA_TICK_MS));
    }
    if (wantEnable) {
        MmRequestEnable(Ctx);
    }
    return action;
}

static VOID
MmAclInComplete(_In_ WDFREQUEST Request, _In_ WDFIOTARGET Target,
                _In_ PWDF_REQUEST_COMPLETION_PARAMS Params, _In_ WDFCONTEXT Context)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;
    PREQUEST_CONTEXT rc = MmGetRequestContext(Request);
    struct _BRB_L2CA_ACL_TRANSFER *acl = &rc->Brb->BrbL2caAclTransfer;
    NTSTATUS status = Params->IoStatus.Status;

    if (NT_SUCCESS(status)) {
        PUCHAR data = MmAclBuffer(acl);
        MM_STAT_INC(ctx, AclIn);
        if (acl->BtAddress != 0 && ctx->BtAddress == 0) {
            WdfSpinLockAcquire(ctx->Lock);
            ctx->BtAddress = acl->BtAddress;
            WdfSpinLockRelease(ctx->Lock);
        }
        ULONG length = (acl->RemainingBufferSize <= acl->BufferSize)
                           ? acl->BufferSize - acl->RemainingBufferSize
                           : 0;

        if (data != NULL && length > 0 &&
            MmHandleIncoming(ctx, acl->ChannelHandle, data, length) == MM_SWALLOW) {
            MM_STAT_INC(ctx, Swallowed);
            /* Hand the same read back to the Bluetooth stack for the next packet. */
            acl->BufferSize = rc->OrigBufferSize;
            acl->RemainingBufferSize = rc->OrigRemaining;
            WdfRequestFormatRequestUsingCurrentType(Request);
            WdfRequestSetCompletionRoutine(Request, MmAclInComplete, ctx);
            if (WdfRequestSend(Request, Target, WDF_NO_SEND_OPTIONS)) {
                return;
            }
            status = WdfRequestGetStatus(Request);
            WdfRequestComplete(Request, status);
            return;
        }
    }

    WdfRequestCompleteWithInformation(Request, status, Params->IoStatus.Information);
}

/* ------------------------------------------------------------------------ */
/* Multitouch enable (SET_REPORT Feature on the control channel) */

static VOID MmMaybeEnableMultitouch(_In_ PDEVICE_CONTEXT Ctx)
{
    WDF_REQUEST_REUSE_PARAMS reuse;
    L2CAP_CHANNEL_HANDLE control;
    BTH_ADDR address;
    MM_MODEL model;
    LONGLONG now = MmNowMs();
    ULONG length;
    NTSTATUS status;

    if (!Ctx->BthIfValid || !Ctx->VhfStarted) {
        return;
    }

    WdfSpinLockAcquire(Ctx->Lock);
    control = Ctx->ControlChannel;
    address = Ctx->BtAddress;
    model = Ctx->Model;
    if (Ctx->MultitouchActive || control == NULL || address == 0 ||
        (Ctx->LastEnableMs != 0 && now - Ctx->LastEnableMs < MM_ENABLE_RETRY_MS)) {
        WdfSpinLockRelease(Ctx->Lock);
        return;
    }
    Ctx->LastEnableMs = now;
    WdfSpinLockRelease(Ctx->Lock);

    if (InterlockedCompareExchange(&Ctx->FeatureInFlight, 1, 0) != 0) {
        return;
    }

    length = MmBuildEnablePacket(model, Ctx->FeatureBuffer, sizeof(Ctx->FeatureBuffer));
    if (length == 0) {
        InterlockedExchange(&Ctx->FeatureInFlight, 0);
        return;
    }

    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
    (VOID)WdfRequestReuse(Ctx->FeatureRequest, &reuse);

    Ctx->BthIf.BthInitializeBrb((PBRB)&Ctx->FeatureBrb, BRB_L2CA_ACL_TRANSFER);
    Ctx->FeatureBrb.BtAddress = address;
    Ctx->FeatureBrb.ChannelHandle = control;
    Ctx->FeatureBrb.TransferFlags = ACL_TRANSFER_DIRECTION_OUT;
    Ctx->FeatureBrb.BufferSize = length;
    Ctx->FeatureBrb.Buffer = Ctx->FeatureBuffer;
    Ctx->FeatureBrb.BufferMDL = NULL;

    status = WdfIoTargetFormatRequestForInternalIoctlOthers(Ctx->Target, Ctx->FeatureRequest,
                                                            IOCTL_INTERNAL_BTH_SUBMIT_BRB,
                                                            Ctx->FeatureBrbMemory, NULL,
                                                            NULL, NULL, NULL, NULL);
    if (!NT_SUCCESS(status)) {
        InterlockedExchange(&Ctx->FeatureInFlight, 0);
        return;
    }
    WdfRequestSetCompletionRoutine(Ctx->FeatureRequest, MmFeatureComplete, Ctx);

    WdfSpinLockAcquire(Ctx->Lock);
    Ctx->ExpectHandshake = TRUE;
    WdfSpinLockRelease(Ctx->Lock);

    MMLOG("sending multitouch enable (model %d, %lu bytes)\n", (int)model, length);
    MM_STAT_INC(Ctx, EnableSent);
    if (!WdfRequestSend(Ctx->FeatureRequest, Ctx->Target, WDF_NO_SEND_OPTIONS)) {
        WdfSpinLockAcquire(Ctx->Lock);
        Ctx->ExpectHandshake = FALSE;
        WdfSpinLockRelease(Ctx->Lock);
        InterlockedExchange(&Ctx->FeatureInFlight, 0);
    }
}

static VOID
MmFeatureComplete(_In_ WDFREQUEST Request, _In_ WDFIOTARGET Target,
                  _In_ PWDF_REQUEST_COMPLETION_PARAMS Params, _In_ WDFCONTEXT Context)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;

    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(Target);

    if (!NT_SUCCESS(Params->IoStatus.Status)) {
        MMLOG("multitouch enable send failed 0x%08X\n", Params->IoStatus.Status);
        MM_STAT_INC(ctx, EnableFailed);
        WdfSpinLockAcquire(ctx->Lock);
        ctx->ExpectHandshake = FALSE;
        WdfSpinLockRelease(ctx->Lock);
    }
    ctx->Stats.LastEnableStatus = Params->IoStatus.Status;
    InterlockedExchange(&ctx->FeatureInFlight, 0);
}

VOID MmStatsNoteBrbType(_In_ PDEVICE_CONTEXT Ctx, _In_ USHORT Type)
{
    LONG i, n = MM_MIN(Ctx->Stats.BrbTypeCount, 16);

    for (i = 0; i < n; i++) {
        if (Ctx->Stats.BrbTypes[i] == Type) {
            return;
        }
    }
    i = InterlockedIncrement(&Ctx->Stats.BrbTypeCount) - 1;
    if (i < 16) {
        Ctx->Stats.BrbTypes[i] = Type;
    }
}

VOID MmEvtEnableTimer(_In_ WDFTIMER Timer)
{
    MmMaybeEnableMultitouch(MmGetContext(WdfTimerGetParentObject(Timer)));
}

/* ------------------------------------------------------------------------ */
/* Momentum scrolling */

VOID MmEvtInertiaTimer(_In_ WDFTIMER Timer)
{
    PDEVICE_CONTEXT ctx = MmGetContext(WdfTimerGetParentObject(Timer));
    UCHAR report[MM_REPORT_LEN];
    BOOLEAN send, again;

    WdfSpinLockAcquire(ctx->Lock);
    send = MmGestureTick(&ctx->Gesture, &ctx->Settings, MmNowMs(), report);
    again = ctx->Gesture.InertiaActive;
    if (!again) {
        ctx->InertiaTimerRunning = FALSE;
    }
    WdfSpinLockRelease(ctx->Lock);

    if (send) {
        MmSubmitReport(ctx, report);
    }
    if (again) {
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(MM_INERTIA_TICK_MS));
    }
}

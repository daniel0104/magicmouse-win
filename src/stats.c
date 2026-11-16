/*
 * stats.c - publish diagnostic counters to the registry so they can be read
 * without a kernel debugger:
 *   HKLM\SYSTEM\CurrentControlSet\Services\MagicMouseFlt\Parameters\Status (volatile)
 */
#include "driver.h"

static VOID MmPutULong(_In_ WDFKEY Key, _In_ PCWSTR Name, _In_ ULONG Value)
{
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, Name);
    (VOID)WdfRegistryAssignULong(Key, &name, Value);
}

static VOID MmPutBinary(_In_ WDFKEY Key, _In_ PCWSTR Name, _In_reads_bytes_(Length) PVOID Data, _In_ ULONG Length)
{
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, Name);
    (VOID)WdfRegistryAssignValue(Key, &name, REG_BINARY, Length, Data);
}

VOID MmEvtStatsTimer(_In_ WDFTIMER Timer)
{
    PDEVICE_CONTEXT ctx = MmGetContext(WdfTimerGetParentObject(Timer));

    if (ctx->Stats.Version != ctx->StatsWritten) {
        WdfWorkItemEnqueue(ctx->StatsWorkItem);
    }
}

VOID MmEvtStatsWorkItem(_In_ WDFWORKITEM WorkItem)
{
    PDEVICE_CONTEXT ctx = MmGetContext(WdfWorkItemGetParentObject(WorkItem));
    MM_STATS *st = &ctx->Stats;
    WDFKEY params, status;
    UNICODE_STRING sub;
    ULONG multitouch, model, channels = 0, control, interrupt, resmult, i;
    LONG version = st->Version;

    if (!NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_CREATE_SUB_KEY | KEY_SET_VALUE,
                                                       WDF_NO_OBJECT_ATTRIBUTES, &params))) {
        return;
    }
    RtlInitUnicodeString(&sub, L"Status");
    if (!NT_SUCCESS(WdfRegistryCreateKey(params, &sub, KEY_SET_VALUE, REG_OPTION_VOLATILE, NULL,
                                         WDF_NO_OBJECT_ATTRIBUTES, &status))) {
        WdfRegistryClose(params);
        return;
    }

    WdfSpinLockAcquire(ctx->Lock);
    multitouch = ctx->MultitouchActive;
    model = (ULONG)ctx->Model;
    for (i = 0; i < MM_MAX_CHANNELS; i++) {
        if (ctx->Channels[i].Handle != NULL) {
            channels++;
        }
    }
    control = ctx->ControlChannel != NULL;
    interrupt = ctx->InterruptChannel != NULL;
    resmult = ctx->Gesture.ResMultiplier;
    WdfSpinLockRelease(ctx->Lock);

    MmPutULong(status, L"Updated",          (ULONG)MmNowMs());
    MmPutULong(status, L"Model",            model);
    MmPutULong(status, L"BthInterface",     ctx->BthIfValid);
    MmPutULong(status, L"QiStatus",         (ULONG)st->QiStatus);
    MmPutULong(status, L"VhfCreateStatus",  (ULONG)st->VhfCreateStatus);
    MmPutULong(status, L"VhfStartStatus",   (ULONG)st->VhfStartStatus);
    MmPutULong(status, L"VhfStarted",       ctx->VhfStarted);
    MmPutULong(status, L"ResMultiplier",    resmult);
    MmPutULong(status, L"Brbs",             (ULONG)st->Brbs);
    MmPutULong(status, L"OtherIoctls",      (ULONG)st->OtherIoctls);
    MmPutULong(status, L"AclIn",            (ULONG)st->AclIn);
    MmPutULong(status, L"AclOut",           (ULONG)st->AclOut);
    MmPutULong(status, L"ChannelOpens",     (ULONG)st->Opens);
    MmPutULong(status, L"ChannelCloses",    (ULONG)st->Closes);
    MmPutULong(status, L"ChannelsOpen",     channels);
    MmPutULong(status, L"ControlKnown",     control);
    MmPutULong(status, L"InterruptKnown",   interrupt);
    MmPutULong(status, L"InputReports",     (ULONG)st->InputReports);
    MmPutULong(status, L"Swallowed",        (ULONG)st->Swallowed);
    MmPutULong(status, L"VhfReports",       (ULONG)st->VhfReports);
    MmPutULong(status, L"VhfSubmitFailed",  (ULONG)st->VhfSubmitFailed);
    MmPutULong(status, L"LastVhfSubmitStatus", (ULONG)st->LastVhfSubmitStatus);
    MmPutULong(status, L"HidEnableAttempts",   (ULONG)st->HidEnableAttempts);
    MmPutULong(status, L"HidCollectionsFound",  (ULONG)st->HidCollectionsFound);
    MmPutULong(status, L"HidCollectionsMatched",(ULONG)st->HidCollectionsMatched);
    MmPutULong(status, L"HidSetFeatureOk",      (ULONG)st->HidSetFeatureOk);
    MmPutULong(status, L"LastHidVidPid",        (ULONG)st->LastHidVidPid);
    MmPutULong(status, L"LastHidEnumStatus",    (ULONG)st->LastHidEnumStatus);
    MmPutULong(status, L"LastHidInfoStatus",    (ULONG)st->LastHidInfoStatus);
    MmPutULong(status, L"LastHidSetFeatureStatus", (ULONG)st->LastHidSetFeatureStatus);
    MmPutULong(status, L"EnableSent",       (ULONG)st->EnableSent);
    MmPutULong(status, L"EnableFailed",     (ULONG)st->EnableFailed);
    MmPutULong(status, L"LastEnableStatus", (ULONG)st->LastEnableStatus);
    MmPutULong(status, L"Handshakes",       (ULONG)st->Handshakes);
    MmPutULong(status, L"LastHandshake",    (ULONG)st->LastHandshake);
    MmPutULong(status, L"MultitouchActive", multitouch);
    MmPutBinary(status, L"ReportIds",       (PVOID)st->ReportIds, sizeof(st->ReportIds));
    MmPutBinary(status, L"BrbTypes",        st->BrbTypes,
                (ULONG)(MM_MIN(st->BrbTypeCount, 16) * sizeof(USHORT)));
    MmPutBinary(status, L"LastInput",       st->LastInput, (ULONG)MM_MIN(st->LastInputLen, (LONG)sizeof(st->LastInput)));
    MmPutULong(status, L"LastInputLen",     (ULONG)st->LastInputLen);
    MmPutBinary(status, L"LastControlIn",   st->LastControlIn,
                (ULONG)MM_MIN(st->LastControlInLen, (LONG)sizeof(st->LastControlIn)));

    WdfRegistryClose(status);
    WdfRegistryClose(params);
    ctx->StatsWritten = version;
}

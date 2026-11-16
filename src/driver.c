/*
 * driver.c - DriverEntry, device add, PnP/power callbacks, settings, VHF.
 */
#include <ntddk.h>
#include <initguid.h>       /* instantiate GUID_BTHDDI_PROFILE_DRIVER_INTERFACE in this file */
#include "driver.h"

EVT_WDF_DEVICE_PREPARE_HARDWARE        MmEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE        MmEvtReleaseHardware;
EVT_WDF_DEVICE_D0_EXIT                 MmEvtD0Exit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT    MmEvtSelfManagedIoInit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_RESTART MmEvtSelfManagedIoRestart;
EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP MmEvtSelfManagedIoCleanup;
EVT_VHF_ASYNC_OPERATION                MmVhfGetFeature;
EVT_VHF_ASYNC_OPERATION                MmVhfSetFeature;

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, MmEvtDeviceAdd)
#pragma alloc_text(PAGE, MmEvtPrepareHardware)
#pragma alloc_text(PAGE, MmEvtReleaseHardware)
#pragma alloc_text(PAGE, MmEvtSelfManagedIoInit)
#pragma alloc_text(PAGE, MmEvtSelfManagedIoRestart)
#pragma alloc_text(PAGE, MmEvtSelfManagedIoCleanup)
#endif

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    WDF_DRIVER_CONFIG_INIT(&config, MmEvtDeviceAdd);
    config.DriverPoolTag = MM_POOL_TAG;

    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        MMLOG("WdfDriverCreate failed 0x%08X\n", status);
    }
    return status;
}

LONGLONG MmNowMs(VOID)
{
    return (LONGLONG)(KeQueryInterruptTime() / 10000);
}

/* ------------------------------------------------------------------------ */
/* Settings: HKLM\SYSTEM\CurrentControlSet\Services\MagicMouseFlt\Parameters */

static VOID MmQueryULong(_In_ WDFKEY Key, _In_ PCWSTR Name, _Inout_ ULONG *Value)
{
    UNICODE_STRING name;
    ULONG v;

    RtlInitUnicodeString(&name, Name);
    if (NT_SUCCESS(WdfRegistryQueryULong(Key, &name, &v))) {
        *Value = v;
    }
}

static VOID MmLoadSettings(_In_ PDEVICE_CONTEXT Ctx)
{
    MM_SETTINGS s;
    WDFKEY key;

    PAGED_CODE();

    MmSettingsDefaults(&s);
    if (NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        MmQueryULong(key, L"MiddleClickMode",  &s.MiddleClickMode);
        MmQueryULong(key, L"MiddleZoneWidth",  &s.MiddleZoneWidth);
        MmQueryULong(key, L"ScrollEnabled",    &s.ScrollEnabled);
        MmQueryULong(key, L"HorizontalScroll", &s.HorizontalScroll);
        MmQueryULong(key, L"NaturalScrolling", &s.NaturalScrolling);
        MmQueryULong(key, L"ScrollSpeed",      &s.ScrollSpeed);
        MmQueryULong(key, L"Acceleration",     &s.Acceleration);
        MmQueryULong(key, L"Inertia",          &s.Inertia);
        MmQueryULong(key, L"AxisLock",         &s.AxisLock);
        MmQueryULong(key, L"ScrollThreshold",  &s.ScrollThreshold);
        WdfRegistryClose(key);
    }
    MmSettingsClamp(&s);

    WdfSpinLockAcquire(Ctx->Lock);
    Ctx->Settings = s;
    WdfSpinLockRelease(Ctx->Lock);

    MMLOG("settings: middle=%lu zone=%lu scroll=%lu h=%lu natural=%lu speed=%lu accel=%lu inertia=%lu lock=%lu\n",
          s.MiddleClickMode, s.MiddleZoneWidth, s.ScrollEnabled, s.HorizontalScroll,
          s.NaturalScrolling, s.ScrollSpeed, s.Acceleration, s.Inertia, s.AxisLock);
}

/* ------------------------------------------------------------------------ */
/* Model detection from the hardware ID */

static BOOLEAN MmContainsI(_In_reads_(Count) const WCHAR *Buffer, _In_ size_t Count, _In_z_ const WCHAR *Pattern)
{
    size_t m = 0, i, j;

    while (Pattern[m] != L'\0') {
        m++;
    }
    for (i = 0; i + m <= Count; i++) {
        for (j = 0; j < m; j++) {
            if (RtlUpcaseUnicodeChar(Buffer[i + j]) != Pattern[j]) {
                break;
            }
        }
        if (j == m) {
            return TRUE;
        }
    }
    return FALSE;
}

static MM_MODEL MmDetectModel(_In_ PWDFDEVICE_INIT DeviceInit)
{
    WDFMEMORY memory;
    size_t size = 0;
    const WCHAR *ids;
    MM_MODEL model = MM_MODEL_UNKNOWN;

    if (!NT_SUCCESS(WdfFdoInitAllocAndQueryProperty(DeviceInit, DevicePropertyHardwareID, NonPagedPoolNx,
                                                    WDF_NO_OBJECT_ATTRIBUTES, &memory))) {
        return model;
    }
    ids = (const WCHAR *)WdfMemoryGetBuffer(memory, &size);
    size /= sizeof(WCHAR);

    if (MmContainsI(ids, size, L"PID&030D")) {
        model = MM_MODEL_MM1;
    } else if (MmContainsI(ids, size, L"PID&0269") || MmContainsI(ids, size, L"PID&0323")) {
        model = MM_MODEL_MM2;
    }
    WdfObjectDelete(memory);
    return model;
}

/* ------------------------------------------------------------------------ */
/* Device add */

NTSTATUS
MmEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_TIMER_CONFIG timerConfig;
    VHF_CONFIG vhfConfig;
    WDFDEVICE device;
    PDEVICE_CONTEXT ctx;
    MM_MODEL model;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    WdfFdoInitSetFilter(DeviceInit);
    model = MmDetectModel(DeviceInit);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware      = MmEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware      = MmEvtReleaseHardware;
    pnp.EvtDeviceD0Exit               = MmEvtD0Exit;
    pnp.EvtDeviceSelfManagedIoInit    = MmEvtSelfManagedIoInit;
    pnp.EvtDeviceSelfManagedIoRestart = MmEvtSelfManagedIoRestart;
    pnp.EvtDeviceSelfManagedIoCleanup = MmEvtSelfManagedIoCleanup;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    /* Every request HidBth sends us gets a context for the completion routines. */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, REQUEST_CONTEXT);
    WdfDeviceInitSetRequestAttributes(DeviceInit, &attr);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attr, &device);
    if (!NT_SUCCESS(status)) {
        MMLOG("WdfDeviceCreate failed 0x%08X\n", status);
        return status;
    }

    ctx = MmGetContext(device);
    ctx->Device = device;
    ctx->Target = WdfDeviceGetIoTarget(device);
    ctx->Model = model;
    MmSettingsDefaults(&ctx->Settings);
    MmGestureInit(&ctx->Gesture);
    MMLOG("attached, model=%d\n", (int)model);

    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = device;
    status = WdfSpinLockCreate(&attr, &ctx->Lock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Only internal IOCTLs (BRBs) are intercepted; the framework passes
       everything else straight down because this is a filter. */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoInternalDeviceControl = MmEvtIoInternalDeviceControl;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        MMLOG("WdfIoQueueCreate failed 0x%08X\n", status);
        return status;
    }

    /* Request + BRB used to send the multitouch enable packet. */
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = device;
    status = WdfRequestCreate(&attr, ctx->Target, &ctx->FeatureRequest);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = device;
    status = WdfMemoryCreatePreallocated(&attr, &ctx->FeatureBrb, sizeof(ctx->FeatureBrb), &ctx->FeatureBrbMemory);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_TIMER_CONFIG_INIT(&timerConfig, MmEvtEnableTimer);
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = device;
    status = WdfTimerCreate(&timerConfig, &attr, &ctx->EnableTimer);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_TIMER_CONFIG_INIT(&timerConfig, MmEvtInertiaTimer);
    timerConfig.UseHighResolutionTimer = WdfTrue;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = device;
    status = WdfTimerCreate(&timerConfig, &attr, &ctx->InertiaTimer);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    {
        WDF_WORKITEM_CONFIG workConfig;

        WDF_TIMER_CONFIG_INIT_PERIODIC(&timerConfig, MmEvtStatsTimer, 2000);
        WDF_OBJECT_ATTRIBUTES_INIT(&attr);
        attr.ParentObject = device;
        status = WdfTimerCreate(&timerConfig, &attr, &ctx->StatsTimer);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        WDF_WORKITEM_CONFIG_INIT(&workConfig, MmEvtStatsWorkItem);
        WDF_OBJECT_ATTRIBUTES_INIT(&attr);
        attr.ParentObject = device;
        status = WdfWorkItemCreate(&workConfig, &attr, &ctx->StatsWorkItem);
        if (!NT_SUCCESS(status)) {
            return status;
        }

        WDF_WORKITEM_CONFIG_INIT(&workConfig, MmEvtEnableWorkItem);
        WDF_OBJECT_ATTRIBUTES_INIT(&attr);
        attr.ParentObject = device;
        status = WdfWorkItemCreate(&workConfig, &attr, &ctx->EnableWorkItem);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    /* Virtual HID mouse. vhf.sys sits below us (see the INF). If this fails the
       filter stays a pure pass-through and the mouse works as before. */
    VHF_CONFIG_INIT(&vhfConfig, WdfDeviceWdmGetDeviceObject(device),
                    g_MmReportDescriptorLength, (PUCHAR)g_MmReportDescriptor);
    vhfConfig.VhfClientContext = ctx;
    vhfConfig.EvtVhfAsyncOperationGetFeature = MmVhfGetFeature;
    vhfConfig.EvtVhfAsyncOperationSetFeature = MmVhfSetFeature;
    vhfConfig.VendorID = 0x1209;          /* pid.codes (open source) */
    vhfConfig.ProductID = 0x0001;
    vhfConfig.VersionNumber = 0x0100;
    status = VhfCreate(&vhfConfig, &ctx->Vhf);
    MM_STAT_SET(ctx, VhfCreateStatus, status);
    if (!NT_SUCCESS(status)) {
        MMLOG("VhfCreate failed 0x%08X - running as pass-through\n", status);
        ctx->Vhf = NULL;
    }

    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* PnP / power */

NTSTATUS
MmEvtPrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Raw, _In_ WDFCMRESLIST Translated)
{
    PDEVICE_CONTEXT ctx = MmGetContext(Device);
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Raw);
    UNREFERENCED_PARAMETER(Translated);
    PAGED_CODE();

    if (!ctx->BthIfValid) {
        status = WdfFdoQueryForInterface(Device, &GUID_BTHDDI_PROFILE_DRIVER_INTERFACE,
                                         (PINTERFACE)&ctx->BthIf, sizeof(ctx->BthIf),
                                         BTHDDI_PROFILE_DRIVER_INTERFACE_VERSION_FOR_QI, NULL);
        MM_STAT_SET(ctx, QiStatus, status);
        if (NT_SUCCESS(status)) {
            ctx->BthIfValid = TRUE;
        } else {
            MMLOG("profile interface query failed 0x%08X - multitouch can't be enabled\n", status);
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS
MmEvtReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Translated)
{
    PDEVICE_CONTEXT ctx = MmGetContext(Device);

    UNREFERENCED_PARAMETER(Translated);
    PAGED_CODE();

    if (ctx->BthIfValid) {
        ctx->BthIfValid = FALSE;
        if (ctx->BthIf.Interface.InterfaceDereference != NULL) {
            ctx->BthIf.Interface.InterfaceDereference(ctx->BthIf.Interface.Context);
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS
MmEvtD0Exit(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    PDEVICE_CONTEXT ctx = MmGetContext(Device);

    UNREFERENCED_PARAMETER(TargetState);

    WdfTimerStop(ctx->EnableTimer, TRUE);
    WdfTimerStop(ctx->InertiaTimer, TRUE);
    WdfTimerStop(ctx->StatsTimer, TRUE);
    WdfWorkItemFlush(ctx->StatsWorkItem);
    if (ctx->EnableWorkItem != NULL) { WdfWorkItemFlush(ctx->EnableWorkItem); }
    if (InterlockedCompareExchange(&ctx->FeatureInFlight, 0, 0) != 0) {
        WdfRequestCancelSentRequest(ctx->FeatureRequest);
    }
    MmResetConnectionState(ctx);
    return STATUS_SUCCESS;
}

NTSTATUS
MmEvtSelfManagedIoInit(_In_ WDFDEVICE Device)
{
    PDEVICE_CONTEXT ctx = MmGetContext(Device);
    NTSTATUS status;

    PAGED_CODE();

    MmLoadSettings(ctx);
    WdfTimerStart(ctx->StatsTimer, WDF_REL_TIMEOUT_IN_MS(500));
    if (ctx->Vhf != NULL) {
        status = VhfStart(ctx->Vhf);
        MM_STAT_SET(ctx, VhfStartStatus, status);
        if (NT_SUCCESS(status)) {
            ctx->VhfStarted = TRUE;
            MMLOG("virtual mouse started\n");
        } else {
            MMLOG("VhfStart failed 0x%08X - running as pass-through\n", status);
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS
MmEvtSelfManagedIoRestart(_In_ WDFDEVICE Device)
{
    PAGED_CODE();
    MmLoadSettings(MmGetContext(Device));
    WdfTimerStart(MmGetContext(Device)->StatsTimer, WDF_REL_TIMEOUT_IN_MS(500));
    return STATUS_SUCCESS;
}

VOID
MmEvtSelfManagedIoCleanup(_In_ WDFDEVICE Device)
{
    PDEVICE_CONTEXT ctx = MmGetContext(Device);

    PAGED_CODE();

    ctx->VhfStarted = FALSE;
    if (ctx->Vhf != NULL) {
        VhfDelete(ctx->Vhf, TRUE);
        ctx->Vhf = NULL;
    }
}

/* ------------------------------------------------------------------------ */
/* VHF */

VOID MmSubmitReport(_In_ PDEVICE_CONTEXT Ctx, _In_reads_(MM_REPORT_LEN) PUCHAR Report)
{
    HID_XFER_PACKET packet;
    NTSTATUS status;

    if (!Ctx->VhfStarted) {
        return;
    }
    packet.reportBuffer = Report;
    packet.reportBufferLen = MM_REPORT_LEN;
    packet.reportId = Report[0];
    status = VhfReadReportSubmit(Ctx->Vhf, &packet);
    if (NT_SUCCESS(status)) {
        MM_STAT_INC(Ctx, VhfReports);
    } else {
        Ctx->Stats.LastVhfSubmitStatus = status;
        MM_STAT_INC(Ctx, VhfSubmitFailed);
    }
}

/* Feature report 1 = resolution multipliers: bits 0-1 wheel, bits 2-3 pan. */
VOID
MmVhfGetFeature(_In_ PVOID ClientContext, _In_ VHFOPERATIONHANDLE Operation,
                _In_opt_ PVOID OperationContext, _In_ PHID_XFER_PACKET Packet)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)ClientContext;
    NTSTATUS status = STATUS_INVALID_PARAMETER;

    UNREFERENCED_PARAMETER(OperationContext);

    if (Packet->reportId == MM_REPORT_ID && Packet->reportBufferLen >= 2) {
        Packet->reportBuffer[0] = MM_REPORT_ID;
        Packet->reportBuffer[1] = ctx->Gesture.ResMultiplier;
        status = STATUS_SUCCESS;
    }
    VhfAsyncOperationComplete(Operation, status);
}

VOID
MmVhfSetFeature(_In_ PVOID ClientContext, _In_ VHFOPERATIONHANDLE Operation,
                _In_opt_ PVOID OperationContext, _In_ PHID_XFER_PACKET Packet)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)ClientContext;
    NTSTATUS status = STATUS_INVALID_PARAMETER;

    UNREFERENCED_PARAMETER(OperationContext);

    if (Packet->reportId == MM_REPORT_ID && Packet->reportBufferLen >= 2) {
        WdfSpinLockAcquire(ctx->Lock);
        ctx->Gesture.ResMultiplier = Packet->reportBuffer[1] & (MM_RESMULT_WHEEL | MM_RESMULT_PAN);
        WdfSpinLockRelease(ctx->Lock);
        MMLOG("resolution multiplier set to 0x%02X\n", Packet->reportBuffer[1]);
        status = STATUS_SUCCESS;
    }
    VhfAsyncOperationComplete(Operation, status);
}

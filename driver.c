// driver.c - KMDF driver for Apple gmux access on T2 Mac (MMIO variant).
// Maps 16 bytes at 0xFE0B0200. Protocol taken from Linux drivers/platform/x86/apple-gmux.c.
//
// Adds, versus the original raw read/write-only version:
//   - GmuxSetDiscretePower(): mirrors gmux_set_discrete_state() - two-write
//     power-rail sequence + settle delay (no ACPI GPE hookup here, so the
//     interrupt-based completion wait Linux uses is approximated with a
//     fixed delay; Linux itself skips the wait when no GPE is found, so
//     this is not worse than that fallback path).
//   - GmuxSwitchDisplayRoute(): mirrors gmux_switchto() - panel route only.
//   - GmuxSwitchTo(): the full sequenced operation - power target up first,
//     route the panel, power the now-unused GPU down last. Never call
//     GmuxSwitchDisplayRoute() alone; that is what caused the black screen.
//   - ForceIGD-on-load, read from the driver's Parameters registry key,
//     mirroring Linux's apple_gmux.force_igd=1 module option.

#include <ntddk.h>
#include <wdf.h>
#include "public.h"

// ---- Hardware base ----
#define GMUX_PHYS_BASE   0xFE0B0200
#define GMUX_MMIO_SIZE   16

#define OFF_DATA         0x00
#define OFF_PORT_SELECT  0x0E
#define OFF_COMMAND      0x0F

// ---- Device context ----
typedef struct _DEVICE_CONTEXT {
    PUCHAR  MappedBase;
    BOOLEAN Mapped;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

// ---- Prototypes ----
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP EvtDeviceContextCleanup;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL EvtIoDeviceControl;

static BOOLEAN PollCommandClear(PUCHAR Base, ULONG TimeoutMs);
static BOOLEAN GmuxRead8(PUCHAR Base, UCHAR Port, PUCHAR Value);
static BOOLEAN GmuxWrite8(PUCHAR Base, UCHAR Port, UCHAR Value);
static VOID    GmuxSetDiscretePower(PUCHAR Base, BOOLEAN PowerOn, ULONG SettleMs);
static BOOLEAN GmuxSwitchDisplayRoute(PUCHAR Base, BOOLEAN ToDiscrete);
static BOOLEAN GmuxSwitchTo(PUCHAR Base, BOOLEAN ToDiscrete);
static NTSTATUS EnsureMapped(PDEVICE_CONTEXT Context);
static BOOLEAN GmuxDetect(PUCHAR Base);
static ULONG   ReadForceIgdSetting(WDFDRIVER Driver);

// ---- Wait for command register to clear ----
static BOOLEAN
PollCommandClear(_In_ PUCHAR Base, _In_ ULONG TimeoutMs)
{
    ULONG waited = 0;
    LARGE_INTEGER interval;
    for (;;) {
        UCHAR cmd = READ_REGISTER_UCHAR(Base + OFF_COMMAND);
        if (cmd == 0) return TRUE;
        if (waited >= TimeoutMs) return FALSE;
        interval.QuadPart = -10000; // 1 ms
        KeDelayExecutionThread(KernelMode, FALSE, &interval);
        waited++;
    }
}

// ---- Read8 per protocol ----
static BOOLEAN
GmuxRead8(_In_ PUCHAR Base, _In_ UCHAR Port, _Out_ PUCHAR Value)
{
    if (!PollCommandClear(Base, 200)) return FALSE;
    WRITE_REGISTER_UCHAR(Base + OFF_PORT_SELECT, Port);
    WRITE_REGISTER_UCHAR(Base + OFF_COMMAND, 0x01); // read len=1
    if (!PollCommandClear(Base, 200)) return FALSE;
    *Value = READ_REGISTER_UCHAR(Base + OFF_DATA);
    return TRUE;
}

// ---- Write8 per protocol ----
static BOOLEAN
GmuxWrite8(_In_ PUCHAR Base, _In_ UCHAR Port, _In_ UCHAR Value)
{
    if (!PollCommandClear(Base, 200)) return FALSE;
    WRITE_REGISTER_UCHAR(Base + OFF_DATA, Value);
    WRITE_REGISTER_UCHAR(Base + OFF_PORT_SELECT, Port);
    WRITE_REGISTER_UCHAR(Base + OFF_COMMAND, 0x41); // write len=1
    return PollCommandClear(Base, 200);
}

// ---- dGPU power rail sequencing (mirrors gmux_set_discrete_state) ----
static VOID
GmuxSetDiscretePower(_In_ PUCHAR Base, _In_ BOOLEAN PowerOn, _In_ ULONG SettleMs)
{
    GmuxWrite8(Base, GMUX_PORT_DISCRETE_POWER, 1);
    GmuxWrite8(Base, GMUX_PORT_DISCRETE_POWER, PowerOn ? 3 : 0);

    // No ACPI GPE completion signal available here; approximate the
    // interrupt-based wait Linux performs with a fixed settle delay.
    LARGE_INTEGER interval;
    interval.QuadPart = -((LONGLONG)SettleMs * 10000);
    KeDelayExecutionThread(KernelMode, FALSE, &interval);

    DbgPrint("[Gmux] Discrete power %s\n", PowerOn ? "ON" : "OFF");
}

// ---- Panel route only (mirrors gmux_switchto) ----
static BOOLEAN
GmuxSwitchDisplayRoute(_In_ PUCHAR Base, _In_ BOOLEAN ToDiscrete)
{
    BOOLEAN ok = GmuxWrite8(Base, GMUX_PORT_SWITCH_DISPLAY, ToDiscrete ? 3 : 2);
    DbgPrint("[Gmux] Display route -> %s: %d\n", ToDiscrete ? "dGPU" : "iGPU", ok);
    return ok;
}

// ---- Full sequenced switch: power target up, route, power old one down ----
static BOOLEAN
GmuxSwitchTo(_In_ PUCHAR Base, _In_ BOOLEAN ToDiscrete)
{
    if (ToDiscrete) {
        GmuxSetDiscretePower(Base, TRUE, 250);
    }

    BOOLEAN ok = GmuxSwitchDisplayRoute(Base, ToDiscrete);

    if (!ToDiscrete) {
        GmuxSetDiscretePower(Base, FALSE, 250);
    }

    return ok;
}

// ---- MMIO mapping (deferred until first IOCTL) ----
static NTSTATUS
EnsureMapped(_In_ PDEVICE_CONTEXT Context)
{
    if (Context->Mapped) {
        return STATUS_SUCCESS;
    }

    PHYSICAL_ADDRESS physAddr;
    physAddr.QuadPart = GMUX_PHYS_BASE;

    Context->MappedBase = (PUCHAR)MmMapIoSpace(physAddr, GMUX_MMIO_SIZE, MmNonCached);
    if (Context->MappedBase == NULL) {
        DbgPrint("[Gmux] MmMapIoSpace failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (!GmuxDetect(Context->MappedBase)) {
        DbgPrint("[Gmux] Detect failed - wrong address or hardware not present\n");
        MmUnmapIoSpace(Context->MappedBase, GMUX_MMIO_SIZE);
        Context->MappedBase = NULL;
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    Context->Mapped = TRUE;
    DbgPrint("[Gmux] MMIO mapped successfully, version check passed\n");
    return STATUS_SUCCESS;
}

// ---- Version check (port 0x04) ----
static BOOLEAN
GmuxDetect(_In_ PUCHAR Base)
{
    UCHAR ver;
    if (!GmuxRead8(Base, GMUX_PORT_VERSION_MAJOR, &ver)) return FALSE;
    DbgPrint("[Gmux] Version major = %d (0x%02X)\n", ver, ver);
    return (ver == 0x01 || ver == 0x02);
}

// ---- Read ForceIGD DWORD from the driver's Parameters registry key ----
// HKLM\SYSTEM\CurrentControlSet\Services\<ServiceName>\Parameters\ForceIGD
// Default 1 (force iGPU), mirroring Linux's apple_gmux.force_igd=1.
static ULONG
ReadForceIgdSetting(_In_ WDFDRIVER Driver)
{
    ULONG value = 1;
    WDFKEY key;

    if (NT_SUCCESS(WdfDriverOpenParametersRegistryKey(Driver, KEY_READ,
                                                       WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        UNICODE_STRING valName;
        ULONG readVal;

        RtlInitUnicodeString(&valName, L"ForceIGD");

        if (NT_SUCCESS(WdfRegistryQueryULong(key, &valName, &readVal))) {
            value = readVal;
        }
        WdfRegistryClose(key);
    }

    return value;
}

// ---- DriverEntry ----
NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath,
                           WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

// ---- Cleanup ----
VOID
EvtDeviceContextCleanup(_In_ WDFOBJECT Object)
{
    WDFDEVICE device = (WDFDEVICE)Object;
    PDEVICE_CONTEXT context = GetDeviceContext(device);
    if (context->Mapped && context->MappedBase != NULL) {
        MmUnmapIoSpace(context->MappedBase, GMUX_MMIO_SIZE);
        context->MappedBase = NULL;
        context->Mapped = FALSE;
        DbgPrint("[Gmux] MMIO unmapped\n");
    }
}

// ---- Device creation + ForceIGD-on-load ----
NTSTATUS
EvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS status;
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFQUEUE queue;
    PDEVICE_CONTEXT context;

    UNICODE_STRING symbolicLinkName;
    RtlInitUnicodeString(&symbolicLinkName, L"\\DosDevices\\GmuxCtl");

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    attributes.EvtCleanupCallback = EvtDeviceContextCleanup;

    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;

    status = WdfDeviceCreateSymbolicLink(device, &symbolicLinkName);
    if (!NT_SUCCESS(status)) return status;

    context = GetDeviceContext(device);
    context->MappedBase = NULL;
    context->Mapped = FALSE;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = EvtIoDeviceControl;

    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) return status;

    // ForceIGD-on-load, mirroring Linux apple_gmux.force_igd=1. Uses the
    // full sequenced GmuxSwitchTo(), not a raw display-route-only write.
    if (NT_SUCCESS(EnsureMapped(context))) {
        ULONG forceIgd = ReadForceIgdSetting(Driver);
        if (forceIgd) {
            GmuxSwitchTo(context->MappedBase, FALSE); // FALSE = route to iGPU
            DbgPrint("[Gmux] ForceIGD applied at load\n");
        }
    } else {
        DbgPrint("[Gmux] Device added, MMIO mapping deferred/failed at load\n");
    }

    return STATUS_SUCCESS;
}

// ---- IOCTL handling ----
VOID
EvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    NTSTATUS status = STATUS_SUCCESS;
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PDEVICE_CONTEXT context = GetDeviceContext(device);
    size_t bytesReturned = 0;

    status = EnsureMapped(context);
    if (!NT_SUCCESS(status)) {
        WdfRequestCompleteWithInformation(Request, status, 0);
        return;
    }

    switch (IoControlCode) {
    case IOCTL_GMUX_READ8: {
        PGMUX_READ_REQUEST req = NULL;
        PGMUX_READ_RESPONSE resp = NULL;
        UCHAR value = 0;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(GMUX_READ_REQUEST), (PVOID*)&req, NULL);
        if (!NT_SUCCESS(status)) break;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(GMUX_READ_RESPONSE), (PVOID*)&resp, NULL);
        if (!NT_SUCCESS(status)) break;

        resp->Success = GmuxRead8(context->MappedBase, req->Port, &value);
        resp->Value = value;
        bytesReturned = sizeof(GMUX_READ_RESPONSE);
        DbgPrint("[Gmux] READ8 port=0x%02X value=0x%02X success=%d\n",
                 req->Port, value, resp->Success);
        break;
    }
    case IOCTL_GMUX_WRITE8: {
        PGMUX_WRITE_REQUEST req = NULL;
        PGMUX_WRITE_RESPONSE resp = NULL;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(GMUX_WRITE_REQUEST), (PVOID*)&req, NULL);
        if (!NT_SUCCESS(status)) break;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(GMUX_WRITE_RESPONSE), (PVOID*)&resp, NULL);
        if (!NT_SUCCESS(status)) break;

        resp->Success = GmuxWrite8(context->MappedBase, req->Port, req->Value);
        bytesReturned = sizeof(GMUX_WRITE_RESPONSE);
        DbgPrint("[Gmux] WRITE8 port=0x%02X value=0x%02X success=%d\n",
                 req->Port, req->Value, resp->Success);
        break;
    }
    case IOCTL_GMUX_SWITCH_TO: {
        PGMUX_SWITCH_REQUEST req = NULL;
        PGMUX_SWITCH_RESPONSE resp = NULL;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(GMUX_SWITCH_REQUEST), (PVOID*)&req, NULL);
        if (!NT_SUCCESS(status)) break;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(GMUX_SWITCH_RESPONSE), (PVOID*)&resp, NULL);
        if (!NT_SUCCESS(status)) break;

        resp->Success = GmuxSwitchTo(context->MappedBase, req->TargetIsDiscrete);
        bytesReturned = sizeof(GMUX_SWITCH_RESPONSE);
        DbgPrint("[Gmux] SWITCH_TO target=%s success=%d\n",
                 req->TargetIsDiscrete ? "dGPU" : "iGPU", resp->Success);
        break;
    }
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, bytesReturned);
}
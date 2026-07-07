// driver.c
//
// Мінімальний KMDF-драйвер для доступу до Apple gmux (T2 Mac) через MMIO.
// Протокол взятий з Linux apple-gmux.c / apple-gmux.h.
//
// Мапить 16 байт фізичної пам'яті з бази 0xFE0B0200 (MmMapIoSpace) і
// надає два IOCTL: IOCTL_GMUX_READ8 / IOCTL_GMUX_WRITE8 (див. public.h).
//
// Проєкт: Kernel Mode Driver, Empty (KMDF) у Visual Studio + WDK.

#include <ntddk.h>
#include <wdf.h>
#include "public.h"

// ---- Апаратна база ----
#define GMUX_PHYS_BASE   0xFE0B0200
#define GMUX_MMIO_SIZE   16

#define OFF_DATA         0x00
#define OFF_PORT_SELECT  0x0E
#define OFF_COMMAND      0x0F

typedef struct _DEVICE_CONTEXT {
    PUCHAR MappedBase;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP EvtDeviceContextCleanup;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL EvtIoDeviceControl;

// ---- Очікування, поки регістр COMMAND (0x0F) не стане 0 ----
// Виконується на PASSIVE_LEVEL (черга WDF за замовчуванням), тому можна
// використовувати KeDelayExecutionThread замість busy-spin.
static BOOLEAN
PollCommandClear(_In_ PUCHAR Base, _In_ ULONG TimeoutMs)
{
    ULONG waited = 0;
    LARGE_INTEGER interval;

    for (;;)
    {
        UCHAR cmd = READ_REGISTER_UCHAR(Base + OFF_COMMAND);
        if (cmd == 0)
        {
            return TRUE;
        }
        if (waited >= TimeoutMs)
        {
            return FALSE;
        }
        interval.QuadPart = -10000; // 1 мс, відносний інтервал
        KeDelayExecutionThread(KernelMode, FALSE, &interval);
        waited += 1;
    }
}

// ---- read8(port) за протоколом gmux ----
static BOOLEAN
GmuxRead8(_In_ PUCHAR Base, _In_ UCHAR Port, _Out_ PUCHAR Value)
{
    if (!PollCommandClear(Base, 200)) return FALSE;
    WRITE_REGISTER_UCHAR(Base + OFF_PORT_SELECT, Port);
    WRITE_REGISTER_UCHAR(Base + OFF_COMMAND, 0x01); // 0x00 | len(1)
    if (!PollCommandClear(Base, 200)) return FALSE;
    *Value = READ_REGISTER_UCHAR(Base + OFF_DATA);
    return TRUE;
}

// ---- write8(port, val) за протоколом gmux ----
static BOOLEAN
GmuxWrite8(_In_ PUCHAR Base, _In_ UCHAR Port, _In_ UCHAR Value)
{
    if (!PollCommandClear(Base, 200)) return FALSE;
    WRITE_REGISTER_UCHAR(Base + OFF_DATA, Value);
    WRITE_REGISTER_UCHAR(Base + OFF_PORT_SELECT, Port);
    WRITE_REGISTER_UCHAR(Base + OFF_COMMAND, 0x41); // 0x40 | len(1)
    return PollCommandClear(Base, 200);
}

// ---- DriverEntry ----
NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;

    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);

    return WdfDriverCreate(
        DriverObject,
        RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &config,
        WDF_NO_HANDLE);
}

// ---- Прибирання при видаленні девайса: анмапимо MMIO ----
VOID
EvtDeviceContextCleanup(_In_ WDFOBJECT Object)
{
    WDFDEVICE device = (WDFDEVICE)Object;
    PDEVICE_CONTEXT context = GetDeviceContext(device);

    if (context->MappedBase != NULL)
    {
        MmUnmapIoSpace(context->MappedBase, GMUX_MMIO_SIZE);
        context->MappedBase = NULL;
    }
}

// ---- EvtDeviceAdd: створення девайса, мапінг MMIO, черга IOCTL ----
NTSTATUS
EvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS status;
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFQUEUE queue;
    PDEVICE_CONTEXT context;
    PHYSICAL_ADDRESS physAddr;

    UNREFERENCED_PARAMETER(Driver);

    DECLARE_CONST_UNICODE_STRING(symbolicLinkName, L"\\DosDevices\\GmuxCtl");

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    attributes.EvtCleanupCallback = EvtDeviceContextCleanup;

    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    status = WdfDeviceCreateSymbolicLink(device, &symbolicLinkName);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    context = GetDeviceContext(device);

    physAddr.QuadPart = GMUX_PHYS_BASE;
    context->MappedBase = (PUCHAR)MmMapIoSpace(physAddr, GMUX_MMIO_SIZE, MmNonCached);
    if (context->MappedBase == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = EvtIoDeviceControl;

    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    return STATUS_SUCCESS;
}

// ---- Обробка IOCTL з usermode ----
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

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode)
    {
    case IOCTL_GMUX_READ8:
    {
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
        break;
    }
    case IOCTL_GMUX_WRITE8:
    {
        PGMUX_WRITE_REQUEST req = NULL;
        PGMUX_WRITE_RESPONSE resp = NULL;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(GMUX_WRITE_REQUEST), (PVOID*)&req, NULL);
        if (!NT_SUCCESS(status)) break;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(GMUX_WRITE_RESPONSE), (PVOID*)&resp, NULL);
        if (!NT_SUCCESS(status)) break;

        resp->Success = GmuxWrite8(context->MappedBase, req->Port, req->Value);
        bytesReturned = sizeof(GMUX_WRITE_RESPONSE);
        break;
    }
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, bytesReturned);
}
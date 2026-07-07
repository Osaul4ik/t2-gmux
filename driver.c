// driver.c – KMDF драйвер для доступу до Apple gmux (T2 Mac)
// Відкладений мапінг MMIO – тільки при першому IOCTL.
// Протокол взятий з Linux apple-gmux.c / apple-gmux.h
// Мапить 16 байт за адресою 0xFE0B0200.

#include <ntddk.h>
#include <wdf.h>
#include "public.h"

// ---- Апаратна база ----
#define GMUX_PHYS_BASE   0xFE0B0200
#define GMUX_MMIO_SIZE   16

#define OFF_DATA         0x00
#define OFF_PORT_SELECT  0x0E
#define OFF_COMMAND      0x0F

// ---- Контекст пристрою ----
typedef struct _DEVICE_CONTEXT {
    PUCHAR  MappedBase;
    BOOLEAN Mapped;          // чи виконано мапінг
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

// ---- Прототипи ----
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP EvtDeviceContextCleanup;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL EvtIoDeviceControl;

static BOOLEAN PollCommandClear(PUCHAR Base, ULONG TimeoutMs);
static BOOLEAN GmuxRead8(PUCHAR Base, UCHAR Port, PUCHAR Value);
static BOOLEAN GmuxWrite8(PUCHAR Base, UCHAR Port, UCHAR Value);
static NTSTATUS EnsureMapped(PDEVICE_CONTEXT Context);
static BOOLEAN GmuxDetect(PUCHAR Base);

// ---- Очікування готовності ----
static BOOLEAN
PollCommandClear(_In_ PUCHAR Base, _In_ ULONG TimeoutMs)
{
    ULONG waited = 0;
    LARGE_INTEGER interval;
    for (;;) {
        UCHAR cmd = READ_REGISTER_UCHAR(Base + OFF_COMMAND);
        if (cmd == 0) return TRUE;
        if (waited >= TimeoutMs) return FALSE;
        interval.QuadPart = -10000; // 1 мс
        KeDelayExecutionThread(KernelMode, FALSE, &interval);
        waited++;
    }
}

// ---- Read8 за протоколом ----
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

// ---- Write8 за протоколом ----
static BOOLEAN
GmuxWrite8(_In_ PUCHAR Base, _In_ UCHAR Port, _In_ UCHAR Value)
{
    if (!PollCommandClear(Base, 200)) return FALSE;
    WRITE_REGISTER_UCHAR(Base + OFF_DATA, Value);
    WRITE_REGISTER_UCHAR(Base + OFF_PORT_SELECT, Port);
    WRITE_REGISTER_UCHAR(Base + OFF_COMMAND, 0x41); // write len=1
    return PollCommandClear(Base, 200);
}

// ---- Мапінг MMIO (тільки при першому виклику) ----
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

    // Перевірка версії (безпечне читання)
    if (!GmuxDetect(Context->MappedBase)) {
        DbgPrint("[Gmux] Detect failed – wrong address or hardware not present\n");
        MmUnmapIoSpace(Context->MappedBase, GMUX_MMIO_SIZE);
        Context->MappedBase = NULL;
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    Context->Mapped = TRUE;
    DbgPrint("[Gmux] MMIO mapped successfully, version check passed\n");
    return STATUS_SUCCESS;
}

// ---- Перевірка версії (порт 0x04) ----
static BOOLEAN
GmuxDetect(_In_ PUCHAR Base)
{
    UCHAR ver;
    if (!GmuxRead8(Base, GMUX_PORT_VERSION_MAJOR, &ver)) return FALSE;
    DbgPrint("[Gmux] Version major = %d (0x%02X)\n", ver, ver);
    // Версія має бути 1 або 2 (не 0xFF і не 0x00)
    return (ver == 0x01 || ver == 0x02);
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

// ---- Очищення ----
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

// ---- Створення пристрою (БЕЗ мапінгу!) ----
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

    DbgPrint("[Gmux] Device added, MMIO not yet mapped\n");
    return STATUS_SUCCESS;
}

// ---- Обробка IOCTL ----
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

    // Відкладений мапінг – виконується ТІЛЬКИ тут
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
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, bytesReturned);
}
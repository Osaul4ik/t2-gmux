// driver.c - KMDF driver for Apple gmux access on T2 Mac (MMIO variant).
// Protocol taken from Linux drivers/platform/x86/apple-gmux.c.
//
// Physical address: like Linux (pnp_get_resource(IORESOURCE_MEM, 0)), the base is
// taken from the memory resource the ACPI node ACPI\APP000B hands to the driver
// in EvtDevicePrepareHardware - it is NOT guessed. Also like Linux, 16 bytes are
// mapped even though ACPI declares only 8 (the extra 8 are never claimed by PnP;
// MmMapIoSpaceEx does not go through the resource arbiter). GMUX_FALLBACK_PHYS_BASE
// is used only when the device node has no memory resource at all (root-enumerated
// install, root\GmuxDriver).
//
// Adds, versus the original raw read/write-only version:
//   - GmuxSetDiscretePower(): mirrors gmux_set_discrete_state() - two-write
//     power-rail sequence + settle delay (no ACPI GPE hookup here, so the
//     interrupt-based completion wait Linux uses is approximated with a
//     fixed delay; Linux itself skips the wait when no GPE is found, so
//     this is not worse than that fallback path).
//   - GmuxSwitchDdc(): panel DDC line (port 0x28: 1 = iGPU, 2 = dGPU). Linux
//     gmux_switchto() writes it for EVERY gmux type, so it is written here too. MEASURED on
//     this T2 gmux: port 0x28 reads 0x00 both before and after a write of 1, i.e. the write
//     has no visible effect. Linux's own comment says retina/T2 machines cannot switch the
//     panel's AUX separately, so this is NOT the cause of the black panel on the iGPU.
//   - GmuxSwitchDisplayRoute(): panel mux only (port 0x10).
//   - GmuxSwitchExternalRoute(): external DP mux only (port 0x40). Linux only follows the
//     panel with this mux when no Thunderbolt controller is present; on Thunderbolt Macs
//     (MBP16,1 has one) it forces the external AUX channel to the dGPU (value 3) and never
//     switches it. GmuxSwitchAllRoutes() does the same: the external mux is always forced to
//     the dGPU. Use IOCTL_GMUX_ROUTE_EXTERNAL to move it by hand.
//   - GmuxSwitchTo(): the full sequenced operation - power target up first,
//     route DDC + panel + external, power the now-unused GPU down last. NOTE: the gmux.exe
//     client no longer uses it (rail toggling from Windows hangs the machine, see README);
//     it stays for IOCTL_GMUX_SWITCH_TO. Never call GmuxSwitchDisplayRoute() alone.
//   - Nothing is remembered: no route, power or enable state is stored anywhere, and nothing is
//     applied when the device starts. Every Windows boot begins exactly as the firmware left the
//     hardware. The driver reads no registry values either.

#include <ntddk.h>
#include <wdf.h>
#include "public.h"

// ---- SAFETY SWITCH ----
// 0 (default): the driver NEVER writes the panel route (0x10), DDC (0x28), external mux (0x40)
// or the dGPU power rail (0x50), from IOCTLs or from the raw WRITE8 IOCTL.
// Reads and the backlight (0x70/0x74) still work. Nothing can black the panel.
// Build with /DGMUX_ALLOW_SWITCH=1 to get the old behaviour back.
#ifndef GMUX_ALLOW_SWITCH
#define GMUX_ALLOW_SWITCH 0
#endif

// Plain (non-const) variable on purpose: no constant folding, so no unreachable-code warnings
// (the WDK build uses /W4 /WX).
static BOOLEAN g_AllowSwitch = GMUX_ALLOW_SWITCH;

static BOOLEAN
GmuxPortSwitches(_In_ UCHAR Port)
{
    return (BOOLEAN)(Port == GMUX_PORT_SWITCH_DISPLAY || Port == GMUX_PORT_SWITCH_DDC ||
                     Port == GMUX_PORT_SWITCH_EXTERNAL || Port == GMUX_PORT_DISCRETE_POWER);
}

// ---- Hardware base ----
// Only used for a root-enumerated node that has no ACPI resource. Normal path:
// the address comes from EvtDevicePrepareHardware.
#define GMUX_FALLBACK_PHYS_BASE  0xFE0B0200
#define GMUX_MMIO_SIZE           16   // Linux maps 16 although ACPI declares 8

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
EVT_WDF_DEVICE_PREPARE_HARDWARE EvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE EvtDeviceReleaseHardware;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL EvtIoDeviceControl;

// Prototypes carry the same SAL annotations as the definitions - PREfast (C28301)
// requires the FIRST declaration to be annotated.
static BOOLEAN PollCommandClear(_In_ PUCHAR Base, _In_ ULONG TimeoutMs);
_Success_(return) static BOOLEAN GmuxRead8(_In_ PUCHAR Base, _In_ UCHAR Port, _Out_ PUCHAR Value);
static BOOLEAN GmuxWrite8(_In_ PUCHAR Base, _In_ UCHAR Port, _In_ UCHAR Value);
_Success_(return) static BOOLEAN GmuxRead32(_In_ PUCHAR Base, _In_ UCHAR Port, _Out_ PULONG Value);
static BOOLEAN GmuxWrite32(_In_ PUCHAR Base, _In_ UCHAR Port, _In_ ULONG Value);
_Success_(return) static BOOLEAN GmuxGetBacklight(_In_ PUCHAR Base, _Out_ PULONG Value, _Out_ PULONG Max);
static BOOLEAN GmuxSetBacklight(_In_ PUCHAR Base, _In_ ULONG Value);
static BOOLEAN GmuxSetDiscretePower(_In_ PUCHAR Base, _In_ BOOLEAN PowerOn, _In_ ULONG SettleMs);
static BOOLEAN GmuxSwitchDdc(_In_ PUCHAR Base, _In_ BOOLEAN ToDiscrete);
static BOOLEAN GmuxSwitchDisplayRoute(_In_ PUCHAR Base, _In_ BOOLEAN ToDiscrete);
static BOOLEAN GmuxSwitchExternalRoute(_In_ PUCHAR Base, _In_ BOOLEAN ToDiscrete);
static BOOLEAN GmuxSwitchAllRoutes(_In_ PUCHAR Base, _In_ BOOLEAN ToDiscrete);
static BOOLEAN GmuxSwitchTo(_In_ PUCHAR Base, _In_ BOOLEAN ToDiscrete);
_Success_(return) static BOOLEAN GmuxReadVersion(_In_ PUCHAR Base, _Out_ PUCHAR Major, _Out_ PUCHAR Minor, _Out_ PUCHAR Release);
static BOOLEAN GmuxDetect(_In_ PUCHAR Base);

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
_Success_(return)
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

// ---- Read32 / Write32 (Linux gmux_mmio_read32 / gmux_mmio_write32) ----
// The data window +0x00..+0x03 holds the value BIG-endian (ioread32be / cpu_to_be32).
// Command = 4 (read, len 4) or 0x40|4 (write, len 4).
_Success_(return)
static BOOLEAN
GmuxRead32(_In_ PUCHAR Base, _In_ UCHAR Port, _Out_ PULONG Value)
{
    *Value = 0;
    if (!PollCommandClear(Base, 200)) return FALSE;
    WRITE_REGISTER_UCHAR(Base + OFF_PORT_SELECT, Port);
    WRITE_REGISTER_UCHAR(Base + OFF_COMMAND, 0x04);
    if (!PollCommandClear(Base, 200)) return FALSE;

    *Value = ((ULONG)READ_REGISTER_UCHAR(Base + OFF_DATA + 0) << 24) |
             ((ULONG)READ_REGISTER_UCHAR(Base + OFF_DATA + 1) << 16) |
             ((ULONG)READ_REGISTER_UCHAR(Base + OFF_DATA + 2) << 8)  |
              (ULONG)READ_REGISTER_UCHAR(Base + OFF_DATA + 3);
    return TRUE;
}

static BOOLEAN
GmuxWrite32(_In_ PUCHAR Base, _In_ UCHAR Port, _In_ ULONG Value)
{
    if (!PollCommandClear(Base, 200)) return FALSE;
    WRITE_REGISTER_UCHAR(Base + OFF_DATA + 0, (UCHAR)(Value >> 24));
    WRITE_REGISTER_UCHAR(Base + OFF_DATA + 1, (UCHAR)(Value >> 16));
    WRITE_REGISTER_UCHAR(Base + OFF_DATA + 2, (UCHAR)(Value >> 8));
    WRITE_REGISTER_UCHAR(Base + OFF_DATA + 3, (UCHAR)Value);
    WRITE_REGISTER_UCHAR(Base + OFF_PORT_SELECT, Port);
    WRITE_REGISTER_UCHAR(Base + OFF_COMMAND, 0x44);
    return PollCommandClear(Base, 200);
}

// ---- Panel backlight (Linux gmux_get_brightness / gmux_update_status) ----
// current = port 0x74 & 0xFFFFFF, max = port 0x70. Independent of which GPU drives the panel.
_Success_(return)
static BOOLEAN
GmuxGetBacklight(_In_ PUCHAR Base, _Out_ PULONG Value, _Out_ PULONG Max)
{
    ULONG v = 0, m = 0;

    *Value = 0;
    *Max = 0;
    if (!GmuxRead32(Base, GMUX_PORT_BRIGHTNESS, &v)) return FALSE;
    if (!GmuxRead32(Base, GMUX_PORT_MAX_BRIGHTNESS, &m)) m = 0;
    *Value = v & GMUX_BRIGHTNESS_MASK;
    *Max = m & GMUX_BRIGHTNESS_MASK;
    return TRUE;
}

static BOOLEAN
GmuxSetBacklight(_In_ PUCHAR Base, _In_ ULONG Value)
{
    ULONG cur = 0, max = 0;
    BOOLEAN ok;

    Value &= GMUX_BRIGHTNESS_MASK;
    if (GmuxGetBacklight(Base, &cur, &max) && max != 0 && Value > max) {
        Value = max;
    }
    ok = GmuxWrite32(Base, GMUX_PORT_BRIGHTNESS, Value);
    DbgPrint("[Gmux] Backlight -> 0x%lX (max 0x%lX): %d\n", Value, max, ok);
    return ok;
}

// ---- dGPU power rail sequencing (mirrors gmux_set_discrete_state) ----
static BOOLEAN
GmuxSetDiscretePower(_In_ PUCHAR Base, _In_ BOOLEAN PowerOn, _In_ ULONG SettleMs)
{
    if (!g_AllowSwitch) {
        DbgPrint("[Gmux] Discrete power change BLOCKED (GMUX_ALLOW_SWITCH=0)\n");
        return FALSE;
    }
    BOOLEAN ok = GmuxWrite8(Base, GMUX_PORT_DISCRETE_POWER, 1);
    ok = (BOOLEAN)(GmuxWrite8(Base, GMUX_PORT_DISCRETE_POWER, PowerOn ? 3 : 0) && ok);

    // No ACPI GPE completion signal available here; approximate the
    // interrupt-based wait Linux performs with a fixed settle delay.
    LARGE_INTEGER interval;
    interval.QuadPart = -((LONGLONG)SettleMs * 10000);
    KeDelayExecutionThread(KernelMode, FALSE, &interval);

    DbgPrint("[Gmux] Discrete power %s: %d\n", PowerOn ? "ON" : "OFF", ok);

    // Diagnostics only (NOT cleared here: the ACPI GPE handler may own this register).
    // MEASURED: at idle this reads a constant 0x1D (bits 0, 2, 3, 4) that nothing ever
    // acknowledges - Linux clears it from its GPE handler - so bit 2 is already set and only a
    // CHANGE of the value around a power command is informative.
    {
        UCHAR st = 0;
        if (GmuxRead8(Base, GMUX_PORT_INTERRUPT_STATUS, &st)) {
            DbgPrint("[Gmux] interrupt status after power change = 0x%02X (bit 2 = power done)\n", st);
        } else {
            DbgPrint("[Gmux] interrupt status read failed after power change\n");
        }
    }
    return ok;
}

// ---- Panel DDC/AUX line (port 0x28): 1 = iGPU, 2 = dGPU (NOT the 2/3 of ports 0x10/0x40) ----
static BOOLEAN
GmuxSwitchDdc(_In_ PUCHAR Base, _In_ BOOLEAN ToDiscrete)
{
    if (!g_AllowSwitch) {
        DbgPrint("[Gmux] DDC route BLOCKED (GMUX_ALLOW_SWITCH=0)\n");
        return FALSE;
    }
    BOOLEAN ok = GmuxWrite8(Base, GMUX_PORT_SWITCH_DDC, ToDiscrete ? GMUX_DDC_DGPU : GMUX_DDC_IGD);
    DbgPrint("[Gmux] DDC route -> %s: %d\n", ToDiscrete ? "dGPU" : "iGPU", ok);
    return ok;
}

// ---- Panel route only (mirrors gmux_switchto) ----
static BOOLEAN
GmuxSwitchDisplayRoute(_In_ PUCHAR Base, _In_ BOOLEAN ToDiscrete)
{
    if (!g_AllowSwitch) {
        DbgPrint("[Gmux] Display route BLOCKED (GMUX_ALLOW_SWITCH=0)\n");
        return FALSE;
    }
    BOOLEAN ok = GmuxWrite8(Base, GMUX_PORT_SWITCH_DISPLAY, ToDiscrete ? 3 : 2);
    DbgPrint("[Gmux] Display route -> %s: %d\n", ToDiscrete ? "dGPU" : "iGPU", ok);
    return ok;
}

// ---- External DP mux only (port 0x40; readback on 0x41) ----
static BOOLEAN
GmuxSwitchExternalRoute(_In_ PUCHAR Base, _In_ BOOLEAN ToDiscrete)
{
    if (!g_AllowSwitch) {
        DbgPrint("[Gmux] External route BLOCKED (GMUX_ALLOW_SWITCH=0)\n");
        return FALSE;
    }
    BOOLEAN ok = GmuxWrite8(Base, GMUX_PORT_SWITCH_EXTERNAL, ToDiscrete ? 3 : 2);
    DbgPrint("[Gmux] External route -> %s: %d\n", ToDiscrete ? "dGPU" : "iGPU", ok);
    return ok;
}

// ---- DDC + panel + external, in the same order as Linux gmux_switchto() ----
// The result is the PANEL write. A failed DDC or external write is logged but not fatal: it
// must not stop the caller from powering the right GPU, and models without such a mux may
// simply not acknowledge it.
// External mux: always forced to the dGPU, like Linux does on Thunderbolt Macs (it never follows
// the panel here; IOCTL_GMUX_ROUTE_EXTERNAL moves it explicitly).
static BOOLEAN
GmuxSwitchAllRoutes(_In_ PUCHAR Base, _In_ BOOLEAN ToDiscrete)
{
    BOOLEAN ddcOk = GmuxSwitchDdc(Base, ToDiscrete);
    BOOLEAN ok = GmuxSwitchDisplayRoute(Base, ToDiscrete);

    if (!ddcOk) {
        DbgPrint("[Gmux] WARNING: DDC route write did not complete (panel result: %d)\n", ok);
    }
    if (!GmuxSwitchExternalRoute(Base, TRUE)) {
        DbgPrint("[Gmux] WARNING: external route write did not complete (panel result: %d)\n", ok);
    }
    return ok;
}

// ---- Full sequenced switch: power target up, route, power old one down ----
static BOOLEAN
GmuxSwitchTo(_In_ PUCHAR Base, _In_ BOOLEAN ToDiscrete)
{
    if (ToDiscrete) {
        // Never route the panel to a dGPU whose rail did not come up.
        if (!GmuxSetDiscretePower(Base, TRUE, 250)) {
            return FALSE;
        }
    }

    BOOLEAN ok = GmuxSwitchAllRoutes(Base, ToDiscrete);

    if (!ToDiscrete) {
        // Only power the dGPU down if the panel actually moved to the iGPU.
        if (ok) {
            ok = GmuxSetDiscretePower(Base, FALSE, 250);
        }
    }

    return ok;
}

// ---- Version (port 0x04, 4-byte read) ----
// Linux (T2/MMIO gmux) reads it with ioread32be(): major = v>>24, minor = v>>16,
// release = v>>8, i.e. data bytes 0, 1, 2. Used for logging only.
_Success_(return)
static BOOLEAN
GmuxReadVersion(_In_ PUCHAR Base, _Out_ PUCHAR Major, _Out_ PUCHAR Minor, _Out_ PUCHAR Release)
{
    *Major = 0;
    *Minor = 0;
    *Release = 0;

    if (!PollCommandClear(Base, 200)) return FALSE;
    WRITE_REGISTER_UCHAR(Base + OFF_PORT_SELECT, GMUX_PORT_VERSION_MAJOR);
    WRITE_REGISTER_UCHAR(Base + OFF_COMMAND, 0x04); // read len=4
    if (!PollCommandClear(Base, 200)) return FALSE;

    *Major   = READ_REGISTER_UCHAR(Base + OFF_DATA + 0);
    *Minor   = READ_REGISTER_UCHAR(Base + OFF_DATA + 1);
    *Release = READ_REGISTER_UCHAR(Base + OFF_DATA + 2);
    return TRUE;
}

// ---- Presence check, same as Linux apple_gmux_is_mmio() ----
// The command register reads 0xFF when no gmux is behind the mapping (a live gmux
// resets it to 0x00, or shows 0x01/0x04/0x41/0x44 while a command is in flight).
// The version value is NOT used to accept/reject: Linux only prints it. The old
// "major == 1 or 2" gate is not in Linux and made PrepareHardware fail (Code 10).
static BOOLEAN
GmuxDetect(_In_ PUCHAR Base)
{
    UCHAR cmd = READ_REGISTER_UCHAR(Base + OFF_COMMAND);
    UCHAR major, minor, release;

    DbgPrint("[Gmux] Detect: command register = 0x%02X\n", cmd);
    if (cmd == 0xFF) {
        return FALSE;
    }

    if (GmuxReadVersion(Base, &major, &minor, &release)) {
        DbgPrint("[Gmux] Found gmux version %u.%u.%u\n", major, minor, release);
    } else {
        DbgPrint("[Gmux] Version read timed out (command register stuck) - continuing\n");
    }
    return TRUE;
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
    // Safety net only: EvtDeviceReleaseHardware normally unmaps first.
    if (context->Mapped && context->MappedBase != NULL) {
        MmUnmapIoSpace(context->MappedBase, GMUX_MMIO_SIZE);
        context->MappedBase = NULL;
        context->Mapped = FALSE;
        DbgPrint("[Gmux] MMIO unmapped (cleanup)\n");
    }
}

// ---- Resources: map the MMIO block the ACPI node assigned to us ----
// Mirrors Linux gmux_probe(): res = pnp_get_resource(pnp, IORESOURCE_MEM, 0);
// then a 16-byte mapping starting at res->start.
NTSTATUS
EvtDevicePrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    UNREFERENCED_PARAMETER(ResourcesRaw);

    PDEVICE_CONTEXT context = GetDeviceContext(Device);
    PHYSICAL_ADDRESS physAddr;
    BOOLEAN fromAcpi = FALSE;
    ULONG declaredLen = 0;
    ULONG count = 0;
    ULONG i;

    physAddr.QuadPart = 0;

    if (ResourcesTranslated != NULL) {   // a root-enumerated node may have no resource list
        count = WdfCmResourceListGetCount(ResourcesTranslated);
    }

    for (i = 0; i < count; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR desc =
            WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (desc == NULL) {
            continue;
        }
        if (desc->Type == CmResourceTypeMemory) {
            physAddr = desc->u.Memory.Start;
            declaredLen = desc->u.Memory.Length;
            fromAcpi = TRUE;
            break;   // resource index 0 of type MEM, like Linux
        }
        if (desc->Type == CmResourceTypePort) {
            // Classic/indexed (pre-T2) gmux uses I/O ports; this driver only
            // implements the T2 MMIO protocol.
            DbgPrint("[Gmux] I/O-port gmux resource found - not supported by this driver\n");
        }
    }

    if (!fromAcpi) {
        physAddr.QuadPart = GMUX_FALLBACK_PHYS_BASE;
        DbgPrint("[Gmux] No memory resource from PnP (root-enumerated?), using fallback 0x%llX\n",
                 (ULONGLONG)physAddr.QuadPart);
    } else {
        DbgPrint("[Gmux] ACPI memory resource: base=0x%llX declared_len=%lu (mapping %d)\n",
                 (ULONGLONG)physAddr.QuadPart, declaredLen, GMUX_MMIO_SIZE);
    }

    context->MappedBase = (PUCHAR)MmMapIoSpaceEx(physAddr, GMUX_MMIO_SIZE,
                                                 PAGE_READWRITE | PAGE_NOCACHE);
    if (context->MappedBase == NULL) {
        DbgPrint("[Gmux] MmMapIoSpaceEx failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (!GmuxDetect(context->MappedBase)) {
        DbgPrint("[Gmux] Detect failed - not a gmux at this address\n");
        MmUnmapIoSpace(context->MappedBase, GMUX_MMIO_SIZE);
        context->MappedBase = NULL;
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    context->Mapped = TRUE;
    DbgPrint("[Gmux] MMIO mapped, version check passed\n");

    return STATUS_SUCCESS;
}

NTSTATUS
EvtDeviceReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    PDEVICE_CONTEXT context = GetDeviceContext(Device);
    if (context->Mapped && context->MappedBase != NULL) {
        MmUnmapIoSpace(context->MappedBase, GMUX_MMIO_SIZE);
        context->MappedBase = NULL;
        context->Mapped = FALSE;
        DbgPrint("[Gmux] MMIO unmapped\n");
    }
    return STATUS_SUCCESS;
}

// ---- Device creation ----
NTSTATUS
EvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS status;
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFQUEUE queue;
    PDEVICE_CONTEXT context;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;

    UNREFERENCED_PARAMETER(Driver);

    UNICODE_STRING symbolicLinkName;
    RtlInitUnicodeString(&symbolicLinkName, L"\\DosDevices\\GmuxCtl");

    // WdfDeviceCreateSymbolicLink needs a named device object, and this
    // interface writes raw gmux registers - restrict it to SYSTEM/Administrators.
    UNICODE_STRING deviceName;
    RtlInitUnicodeString(&deviceName, L"\\Device\\GmuxCtl");
    status = WdfDeviceInitAssignName(DeviceInit, &deviceName);
    if (!NT_SUCCESS(status)) return status;

    DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)"); // SYSTEM + Administrators only
    status = WdfDeviceInitAssignSDDLString(DeviceInit, &sddl);
    if (!NT_SUCCESS(status)) return status;

    // Hardware resources (ACPI\APP000B memory range) arrive here, not in EvtDeviceAdd.
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware = EvtDevicePrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware = EvtDeviceReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetCharacteristics(DeviceInit, FILE_DEVICE_SECURE_OPEN, FALSE);

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

    // MMIO mapping happens in EvtDevicePrepareHardware.

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
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    NTSTATUS status = STATUS_SUCCESS;
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PDEVICE_CONTEXT context = GetDeviceContext(device);
    size_t bytesReturned = 0;

    if (!context->Mapped || context->MappedBase == NULL) {
        WdfRequestCompleteWithInformation(Request, STATUS_DEVICE_NOT_READY, 0);
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

        // METHOD_BUFFERED: request and response are the SAME system buffer (Value/Success
        // overlay Port), so copy what the log needs before the response is written.
        UCHAR port = req->Port;
        BOOLEAN okRead = GmuxRead8(context->MappedBase, port, &value);
        resp->Success = okRead;
        resp->Value = value;
        bytesReturned = sizeof(GMUX_READ_RESPONSE);
        DbgPrint("[Gmux] READ8 port=0x%02X value=0x%02X success=%d\n", port, value, okRead);
        break;
    }
    case IOCTL_GMUX_WRITE8: {
        PGMUX_WRITE_REQUEST req = NULL;
        PGMUX_WRITE_RESPONSE resp = NULL;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(GMUX_WRITE_REQUEST), (PVOID*)&req, NULL);
        if (!NT_SUCCESS(status)) break;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(GMUX_WRITE_RESPONSE), (PVOID*)&resp, NULL);
        if (!NT_SUCCESS(status)) break;

        // Same shared buffer as above: Success overlays Port.
        UCHAR port = req->Port;
        UCHAR val = req->Value;
        if (!g_AllowSwitch && GmuxPortSwitches(port)) {
            DbgPrint("[Gmux] WRITE8 port=0x%02X BLOCKED (GMUX_ALLOW_SWITCH=0)\n", port);
            resp->Success = FALSE;
            bytesReturned = sizeof(GMUX_WRITE_RESPONSE);
            break;
        }
        BOOLEAN okWrite = GmuxWrite8(context->MappedBase, port, val);
        resp->Success = okWrite;
        bytesReturned = sizeof(GMUX_WRITE_RESPONSE);
        DbgPrint("[Gmux] WRITE8 port=0x%02X value=0x%02X success=%d\n", port, val, okWrite);
        break;
    }
    case IOCTL_GMUX_SWITCH_TO: {
        PGMUX_SWITCH_REQUEST req = NULL;
        PGMUX_SWITCH_RESPONSE resp = NULL;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(GMUX_SWITCH_REQUEST), (PVOID*)&req, NULL);
        if (!NT_SUCCESS(status)) break;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(GMUX_SWITCH_RESPONSE), (PVOID*)&resp, NULL);
        if (!NT_SUCCESS(status)) break;

        // Same shared buffer: Success overlays TargetIsDiscrete.
        BOOLEAN toDiscrete = (BOOLEAN)(req->TargetIsDiscrete != 0);
        BOOLEAN okSwitch = GmuxSwitchTo(context->MappedBase, toDiscrete);
        resp->Success = okSwitch;
        bytesReturned = sizeof(GMUX_SWITCH_RESPONSE);
        DbgPrint("[Gmux] SWITCH_TO target=%s success=%d\n", toDiscrete ? "dGPU" : "iGPU", okSwitch);
        break;
    }
    case IOCTL_GMUX_ROUTE: {
        PGMUX_SWITCH_REQUEST req = NULL;
        PGMUX_SWITCH_RESPONSE resp = NULL;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(GMUX_SWITCH_REQUEST), (PVOID*)&req, NULL);
        if (!NT_SUCCESS(status)) break;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(GMUX_SWITCH_RESPONSE), (PVOID*)&resp, NULL);
        if (!NT_SUCCESS(status)) break;

        resp->Success = GmuxSwitchAllRoutes(context->MappedBase, req->TargetIsDiscrete);
        bytesReturned = sizeof(GMUX_SWITCH_RESPONSE);
        break;
    }
    case IOCTL_GMUX_ROUTE_EXTERNAL: {
        PGMUX_SWITCH_REQUEST req = NULL;
        PGMUX_SWITCH_RESPONSE resp = NULL;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(GMUX_SWITCH_REQUEST), (PVOID*)&req, NULL);
        if (!NT_SUCCESS(status)) break;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(GMUX_SWITCH_RESPONSE), (PVOID*)&resp, NULL);
        if (!NT_SUCCESS(status)) break;

        resp->Success = GmuxSwitchExternalRoute(context->MappedBase, req->TargetIsDiscrete);
        bytesReturned = sizeof(GMUX_SWITCH_RESPONSE);
        break;
    }
    case IOCTL_GMUX_BL_GET: {
        PGMUX_BL_GET_RESPONSE resp = NULL;
        ULONG value = 0, max = 0;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(GMUX_BL_GET_RESPONSE), (PVOID*)&resp, NULL);
        if (!NT_SUCCESS(status)) break;

        resp->Success = GmuxGetBacklight(context->MappedBase, &value, &max);
        resp->Value = value;
        resp->Max = max;
        bytesReturned = sizeof(GMUX_BL_GET_RESPONSE);
        break;
    }
    case IOCTL_GMUX_BL_SET: {
        PGMUX_BL_SET_REQUEST req = NULL;
        PGMUX_BL_SET_RESPONSE resp = NULL;
        ULONG value;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(GMUX_BL_SET_REQUEST), (PVOID*)&req, NULL);
        if (!NT_SUCCESS(status)) break;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(GMUX_BL_SET_RESPONSE), (PVOID*)&resp, NULL);
        if (!NT_SUCCESS(status)) break;

        // Shared METHOD_BUFFERED buffer: Success overlays Value, copy it first.
        value = req->Value;
        resp->Success = GmuxSetBacklight(context->MappedBase, value);
        bytesReturned = sizeof(GMUX_BL_SET_RESPONSE);
        break;
    }
    case IOCTL_GMUX_POWER: {
        PGMUX_POWER_REQUEST req = NULL;
        PGMUX_POWER_RESPONSE resp = NULL;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(GMUX_POWER_REQUEST), (PVOID*)&req, NULL);
        if (!NT_SUCCESS(status)) break;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(GMUX_POWER_RESPONSE), (PVOID*)&resp, NULL);
        if (!NT_SUCCESS(status)) break;

        resp->Success = GmuxSetDiscretePower(context->MappedBase, req->PowerOn, 250);
        bytesReturned = sizeof(GMUX_POWER_RESPONSE);
        break;
    }
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, bytesReturned);
}
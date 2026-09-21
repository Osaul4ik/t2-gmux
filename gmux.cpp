// gmux.cpp - console client for GmuxCtl (run as Administrator).
//
//   gmux i | -i            full switch to Intel:  route panel -> iGPU, disable Radeon in
//                          PnP, THEN cut the dGPU power rail. Aborts before any power-off
//                          step whose precondition is not verified.
//   gmux d | -d            full switch to Radeon: power rail on, rescan PCI, enable Radeon,
//                          THEN route panel -> dGPU. Never touches the Intel adapter.
//
// Individual steps, for testing one thing at a time:
//   gmux status            gmux version, raw route/power ports, Intel + Radeon PnP state
//   gmux route i|d         panel route only
//   gmux power on|off      dGPU power rail only (WARNING: off while the Radeon driver is
//                          running can TDR/BSOD - use `amd disable` first)
//   gmux amd disable|enable   PnP disable/enable of the Radeon display adapter
//   gmux read <hexport>    raw 8-bit read of a gmux port, e.g. `gmux read 50`

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include "public.h"   // shared IOCTL/struct definitions

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

static const wchar_t* const VEN_AMD   = L"pci\\ven_1002";
static const wchar_t* const VEN_INTEL = L"pci\\ven_8086";

// ---------------------------------------------------------------- driver I/O

static HANDLE OpenGmux()
{
    HANDLE h = CreateFileW(L"\\\\.\\GmuxCtl", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        printf("Cannot open GmuxCtl (error %lu).%s\n", e,
               e == ERROR_ACCESS_DENIED ? " Run as Administrator." : " Is the driver installed and started?");
    }
    return h;
}

static bool Ioctl(HANDLE h, DWORD code, void* in, DWORD inSize, void* out, DWORD outSize)
{
    DWORD bytes = 0;
    if (!DeviceIoControl(h, code, in, inSize, out, outSize, &bytes, NULL)) {
        printf("  ioctl 0x%08lX failed: %lu\n", code, GetLastError());
        return false;
    }
    return true;
}

static bool Read8(HANDLE h, UCHAR port, UCHAR* value)
{
    GMUX_READ_REQUEST req = { port };
    GMUX_READ_RESPONSE resp = { 0, 0 };
    if (!Ioctl(h, IOCTL_GMUX_READ8, &req, sizeof(req), &resp, sizeof(resp))) return false;
    if (!resp.Success) return false;
    *value = resp.Value;
    return true;
}

static bool Route(HANDLE h, bool toDiscrete)
{
    GMUX_SWITCH_REQUEST req = { (UCHAR)(toDiscrete ? 1 : 0) };
    GMUX_SWITCH_RESPONSE resp = { 0 };
    printf("  route panel -> %s ...\n", toDiscrete ? "dGPU (Radeon)" : "iGPU (Intel)");
    return Ioctl(h, IOCTL_GMUX_ROUTE, &req, sizeof(req), &resp, sizeof(resp)) && resp.Success;
}

static bool Power(HANDLE h, bool on)
{
    GMUX_POWER_REQUEST req = { (UCHAR)(on ? 1 : 0) };
    GMUX_POWER_RESPONSE resp = { 0 };
    printf("  dGPU power rail -> %s ...\n", on ? "ON" : "OFF");
    return Ioctl(h, IOCTL_GMUX_POWER, &req, sizeof(req), &resp, sizeof(resp)) && resp.Success;
}

// ------------------------------------------------------------------- PnP side

struct GpuInfo {
    bool    found;
    ULONG   status;
    ULONG   problem;
    wchar_t desc[200];
};

static bool ContainsI(const wchar_t* s, const wchar_t* sub)
{
    size_t n = wcslen(sub);
    for (; *s; ++s) {
        size_t i = 0;
        while (i < n && s[i] && towlower(s[i]) == towlower(sub[i])) ++i;
        if (i == n) return true;
    }
    return false;
}

// Finds the first present PCI *display-class* (&CC_03) device with the given vendor tag.
// Caller must SetupDiDestroyDeviceInfoList(*pSet) on success.
static bool OpenGpu(const wchar_t* venTag, HDEVINFO* pSet, SP_DEVINFO_DATA* pDd)
{
    HDEVINFO set = SetupDiGetClassDevsW(NULL, L"PCI", NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) return false;

    for (DWORD i = 0; ; ++i) {
        SP_DEVINFO_DATA dd;
        ZeroMemory(&dd, sizeof(dd));
        dd.cbSize = sizeof(dd);
        if (!SetupDiEnumDeviceInfo(set, i, &dd)) break;

        wchar_t ids[1024] = { 0 };   // multi-sz; last wchar is reserved so it is always double-terminated
        DWORD sz = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(set, &dd, SPDRP_HARDWAREID, NULL,
                                               (PBYTE)ids, sizeof(ids) - sizeof(wchar_t), &sz)) {
            continue;
        }

        bool venOk = false, classOk = false;
        for (const wchar_t* p = ids; *p; p += wcslen(p) + 1) {
            if (ContainsI(p, venTag))   venOk = true;
            if (ContainsI(p, L"&cc_03")) classOk = true;
        }
        if (venOk && classOk) {
            *pSet = set;
            *pDd = dd;
            return true;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return false;
}

static bool QueryGpu(const wchar_t* venTag, GpuInfo* gi)
{
    ZeroMemory(gi, sizeof(*gi));
    HDEVINFO set;
    SP_DEVINFO_DATA dd;
    if (!OpenGpu(venTag, &set, &dd)) return false;

    gi->found = true;
    DWORD sz = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(set, &dd, SPDRP_DEVICEDESC, NULL,
                                           (PBYTE)gi->desc, sizeof(gi->desc) - sizeof(wchar_t), &sz)) {
        lstrcpynW(gi->desc, L"(unnamed)", 200);
    }
    CM_Get_DevNode_Status(&gi->status, &gi->problem, dd.DevInst, 0);
    SetupDiDestroyDeviceInfoList(set);
    return true;
}

static const wchar_t* Describe(const GpuInfo& g)
{
    if (!g.found)                       return L"not present";
    if (g.problem == CM_PROB_DISABLED)  return L"DISABLED";
    if (g.problem != 0)                 return L"PROBLEM";
    if (g.status & DN_STARTED)          return L"running";
    return L"stopped";
}

static bool GpuRunning(const GpuInfo& g)
{
    return g.found && g.problem == 0 && (g.status & DN_STARTED);
}

static void PrintGpu(const wchar_t* label, const wchar_t* venTag)
{
    GpuInfo g;
    if (!QueryGpu(venTag, &g)) {
        printf("  %-7ls not present\n", label);
        return;
    }
    printf("  %-7ls %ls  [%ls]  status=0x%08lX problem=%lu\n", label, g.desc, Describe(g), g.status, g.problem);
}

// stateChange: DICS_DISABLE or DICS_ENABLE
static bool SetGpuState(const wchar_t* venTag, DWORD stateChange)
{
    HDEVINFO set;
    SP_DEVINFO_DATA dd;
    if (!OpenGpu(venTag, &set, &dd)) {
        printf("  adapter not found in PnP\n");
        return false;
    }

    SP_PROPCHANGE_PARAMS pc;
    ZeroMemory(&pc, sizeof(pc));
    pc.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    pc.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    pc.StateChange = stateChange;
    pc.Scope = DICS_FLAG_GLOBAL;
    pc.HwProfile = 0;

    bool ok = SetupDiSetClassInstallParamsW(set, &dd, &pc.ClassInstallHeader, sizeof(pc)) &&
              SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &dd);
    if (!ok) {
        printf("  PnP %s failed: %lu\n", stateChange == DICS_DISABLE ? "disable" : "enable", GetLastError());
    }
    SetupDiDestroyDeviceInfoList(set);
    return ok;
}

// Polls until the adapter reports DISABLED (wantDisabled) or running (!wantDisabled).
static bool WaitGpu(const wchar_t* venTag, bool wantDisabled, DWORD timeoutMs)
{
    for (DWORD t = 0; t <= timeoutMs; t += 250) {
        GpuInfo g;
        if (QueryGpu(venTag, &g)) {
            if (wantDisabled  && g.problem == CM_PROB_DISABLED) return true;
            if (!wantDisabled && GpuRunning(g))                 return true;
        }
        Sleep(250);
    }
    return false;
}

static void RescanPci()
{
    DEVINST root;
    if (CM_Locate_DevNodeW(&root, NULL, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) {
        CM_Reenumerate_DevNode(root, CM_REENUMERATE_SYNCHRONOUS);
    }
}

// ------------------------------------------------------------------- commands

static void PrintPort(HANDLE h, UCHAR port, const char* name)
{
    UCHAR v;
    if (Read8(h, port, &v)) printf("  port 0x%02X %-24s = 0x%02X\n", port, name, v);
    else                    printf("  port 0x%02X %-24s = <read failed>\n", port, name);
}

static void CmdStatus(HANDLE h)
{
    printf("gmux:\n");
    PrintPort(h, GMUX_PORT_VERSION_MAJOR,      "version major");
    PrintPort(h, GMUX_PORT_SWITCH_DISPLAY,     "switch display (0x10)");
    PrintPort(h, GMUX_PORT_SWITCH_GET_DISPLAY, "get display (0x11)");
    PrintPort(h, GMUX_PORT_DISCRETE_POWER,     "discrete power (0x50)");
    printf("  (raw values - Linux never reads 0x50 back, so compare before/after a command)\n");
    printf("adapters:\n");
    PrintGpu(L"Intel", VEN_INTEL);
    PrintGpu(L"Radeon", VEN_AMD);
}

// Full switch to Intel. Every destructive step is gated on the previous one being verified.
static int CmdToIntel(HANDLE h)
{
    printf("== switch to Intel ==\n");

    GpuInfo intel;
    if (!QueryGpu(VEN_INTEL, &intel) || !GpuRunning(intel)) {
        printf("ABORT: Intel adapter is not running (%ls) - routing the panel to it would leave no display.\n",
               intel.found ? Describe(intel) : L"not present");
        return 1;
    }

    if (!Route(h, false)) { printf("ABORT: route to iGPU failed; nothing was powered off.\n"); return 1; }
    Sleep(1500);

    GpuInfo amd;
    if (QueryGpu(VEN_AMD, &amd)) {
        if (amd.problem != CM_PROB_DISABLED) {
            printf("  disabling Radeon in PnP ...\n");
            if (!SetGpuState(VEN_AMD, DICS_DISABLE) || !WaitGpu(VEN_AMD, true, 8000)) {
                printf("ABORT: Radeon is NOT confirmed disabled; power rail left ON (powering off under a live driver = BSOD risk).\n");
                printf("       Panel is already routed to Intel. Try `gmux amd disable`, then `gmux power off`.\n");
                return 1;
            }
        } else {
            printf("  Radeon already disabled in PnP.\n");
        }
    } else {
        printf("  WARNING: Radeon not present in PnP - continuing (no driver to crash).\n");
    }

    if (!Power(h, false)) { printf("FAILED: dGPU power-off write failed.\n"); return 1; }

    printf("== done ==\n");
    CmdStatus(h);
    return 0;
}

// Full switch to Radeon. Intel is never disabled or powered down.
static int CmdToRadeon(HANDLE h)
{
    printf("== switch to Radeon ==\n");

    if (!Power(h, true)) { printf("ABORT: dGPU power-on failed; panel left where it was.\n"); return 1; }
    Sleep(500);
    RescanPci();
    Sleep(1000);

    GpuInfo amd;
    if (!QueryGpu(VEN_AMD, &amd)) {
        printf("ABORT: Radeon did not reappear after power-on + PCI rescan. Panel NOT routed (still on Intel).\n");
        return 1;
    }
    if (amd.problem == CM_PROB_DISABLED) {
        printf("  enabling Radeon in PnP ...\n");
        if (!SetGpuState(VEN_AMD, DICS_ENABLE)) { printf("ABORT: enabling Radeon failed. Panel NOT routed.\n"); return 1; }
    }
    if (!WaitGpu(VEN_AMD, false, 15000)) {
        printf("ABORT: Radeon is not running after enable. Panel NOT routed (still on Intel).\n");
        return 1;
    }

    if (!Route(h, true)) { printf("FAILED: route to dGPU failed.\n"); return 1; }

    printf("== done ==\n");
    CmdStatus(h);
    return 0;
}

static void Usage()
{
    printf("Usage (Administrator):\n"
           "  gmux i | d                 full switch to Intel / Radeon (-i / -d also accepted)\n"
           "  gmux status                ports + Intel/Radeon PnP state\n"
           "  gmux route i|d             panel route only\n"
           "  gmux power on|off          dGPU power rail only\n"
           "  gmux amd disable|enable    PnP disable/enable of the Radeon\n"
           "  gmux read <hexport>        raw port read, e.g. gmux read 50\n");
}

static const char* Arg(int argc, char** argv, int i)
{
    if (i >= argc) return "";
    const char* a = argv[i];
    while (*a == '-' || *a == '/') ++a;
    return a;
}

int main(int argc, char** argv)
{
    const char* cmd = Arg(argc, argv, 1);
    if (!*cmd || !_stricmp(cmd, "h") || !_stricmp(cmd, "help") || !_stricmp(cmd, "?")) { Usage(); return 1; }

    HANDLE h = OpenGmux();
    if (h == INVALID_HANDLE_VALUE) return 1;

    UCHAR ver = 0;
    if (!Read8(h, GMUX_PORT_VERSION_MAJOR, &ver)) {
        printf("Failed to read gmux version (hardware not present / wrong MMIO address?)\n");
        CloseHandle(h);
        return 1;
    }
    printf("GMUX version: %d (0x%02X)\n", ver, ver);

    int rc = 1;
    const char* a2 = Arg(argc, argv, 2);

    if (!_stricmp(cmd, "i"))            rc = CmdToIntel(h);
    else if (!_stricmp(cmd, "d"))       rc = CmdToRadeon(h);
    else if (!_stricmp(cmd, "status"))  { CmdStatus(h); rc = 0; }
    else if (!_stricmp(cmd, "route") && (*a2 == 'i' || *a2 == 'I' || *a2 == 'd' || *a2 == 'D')) {
        rc = Route(h, *a2 == 'd' || *a2 == 'D') ? 0 : 1;
        CmdStatus(h);
    }
    else if (!_stricmp(cmd, "power") && (!_stricmp(a2, "on") || !_stricmp(a2, "off"))) {
        rc = Power(h, !_stricmp(a2, "on")) ? 0 : 1;
        CmdStatus(h);
    }
    else if (!_stricmp(cmd, "amd") && (!_stricmp(a2, "disable") || !_stricmp(a2, "enable"))) {
        bool disable = !_stricmp(a2, "disable");
        rc = (SetGpuState(VEN_AMD, disable ? DICS_DISABLE : DICS_ENABLE) && WaitGpu(VEN_AMD, disable, 15000)) ? 0 : 1;
        printf(rc == 0 ? "  OK\n" : "  FAILED / not confirmed\n");
        CmdStatus(h);
    }
    else if (!_stricmp(cmd, "read") && *a2) {
        UCHAR port = (UCHAR)strtoul(a2, NULL, 16);
        PrintPort(h, port, "raw");
        rc = 0;
    }
    else Usage();

    CloseHandle(h);
    return rc;
}
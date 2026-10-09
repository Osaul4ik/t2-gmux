// gmux.cpp - console client for GmuxCtl (run as Administrator).
//
//   gmux i | -i [off] [sec]   switch the panel to Intel (DDC + panel mux), restart the Intel adapter so
//                          it re-detects the panel, then wait [sec] (default 20) for a key press. No key
//                          = the panel is routed back to the Radeon automatically (no reboot needed if
//                          the screen stays black). The Radeon stays enabled and POWERED.
//                          With `off` (only after the key press): also disable the Radeon in PnP
//                          (non-persistent) and cut its power rail - EXPERIMENTAL, the way back is a reboot.
//   gmux d | -d [power]    switch the panel back to the Radeon: route + restart the Radeon adapter. Never
//                          touches the power rail. If the Radeon is gone from PnP (rail was cut) it
//                          refuses - reboot instead. `d power` forces the old rail-on + PCI rescan
//                          sequence: EXPERIMENTAL, it hung and powered off this machine.
//   gmux radeon on|off     enable / disable the Radeon in PnP only (non-persistent). No gmux port is written
//                          (no route, DDC, external mux, power rail): the panel stays on whatever GPU it is
//                          on, and the rail is owned by the ACPI power resource of loader mode 6. `status`
//                          shows the Radeon's power=D0 / D3.
//
// Nothing is remembered between boots: every Windows boot starts as the firmware left the hardware.
// `i` / `d` / `radeon` only last until the next reboot.
//
// Individual steps, for testing one thing at a time:
//   gmux trial [sec]       same as `gmux i` without `off`
//   gmux status            gmux ports (panel route decoded from 0x10 bit 0, DDC 0x28, interrupt
//                          status 0x16) + Intel / Radeon PnP state
//   gmux route i|d         DDC + panel mux (external DP mux is always forced to the dGPU, like Linux does
//                          on Thunderbolt Macs; move it with `ext`)
//   gmux ext i|d           external DP mux only (port 0x40) - use when the panel is fine but
//                          an external monitor stays black
//   gmux power on|off      dGPU power rail only (WARNING: off while the Radeon driver is
//                          running can TDR/BSOD; on from Windows hung this machine - see `d power`)
//   gmux amd disable|enable   same as `gmux radeon off|on`
//   gmux read <hexport>    raw 8-bit read of a gmux port, e.g. `gmux read 50`
//   gmux write <hexport> <hexval>   raw 8-bit write, e.g. `gmux write 16 1D`. Port 0x50 is refused
//                          here, use `gmux power`.
//   gmux bl                show panel backlight (gmux port 0x74 / max 0x70)
//   gmux bl <0-100>        set backlight in percent (min raw value is 1, never 0)
//   gmux bl +N | -N        relative change in percent points, e.g. `gmux bl -10`
//   gmux bl raw <value>    set the raw gmux value (decimal or 0x...)
//   gmux bl watch          print changes of gmux port 0x74 while you move the Windows slider
//   gmux efi               READ-ONLY: firmware boot GPU preference (gpu-power-prefs), no driver needed

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <conio.h>
#include "public.h"   // shared IOCTL/struct definitions

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "advapi32.lib")  // OpenProcessToken / AdjustTokenPrivileges / LookupPrivilegeValueW

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

static bool Write8(HANDLE h, UCHAR port, UCHAR value)
{
    GMUX_WRITE_REQUEST req = { port, value };
    GMUX_WRITE_RESPONSE resp = { 0 };
    return Ioctl(h, IOCTL_GMUX_WRITE8, &req, sizeof(req), &resp, sizeof(resp)) && resp.Success;
}

// Panel route the way Linux reads it (gmux_read_switch_state): port 0x10, bit 0 - 1 = dGPU, 0 = iGPU
// (observed: 0x05 = dGPU, 0x04 = iGPU). Port 0x11 is NOT usable for this: it reads 3 for the dGPU but
// 0 - not 2 - for the iGPU on T2.
static bool PanelRoute(HANDLE h, UCHAR* route)
{
    UCHAR raw = 0;
    if (!Read8(h, GMUX_PORT_SWITCH_DISPLAY, &raw)) return false;
    *route = (raw & 1) ? GMUX_ROUTE_DGPU : GMUX_ROUTE_IGD;
    return true;
}

// DDC (0x28) + panel mux (0x10) + external DP mux (0x40) - the driver writes them in Linux order.
static bool Route(HANDLE h, bool toDiscrete)
{
    GMUX_SWITCH_REQUEST req = { (UCHAR)(toDiscrete ? 1 : 0) };
    GMUX_SWITCH_RESPONSE resp = { 0 };
    printf("  route DDC + panel (+ external per driver policy) -> %s ...\n", toDiscrete ? "dGPU (Radeon)" : "iGPU (Intel)");
    return Ioctl(h, IOCTL_GMUX_ROUTE, &req, sizeof(req), &resp, sizeof(resp)) && resp.Success;
}

// External DP mux only. Needs a driver that knows IOCTL_GMUX_ROUTE_EXTERNAL.
static bool RouteExternal(HANDLE h, bool toDiscrete)
{
    GMUX_SWITCH_REQUEST req = { (UCHAR)(toDiscrete ? 1 : 0) };
    GMUX_SWITCH_RESPONSE resp = { 0 };
    printf("  route external DP -> %s ...\n", toDiscrete ? "dGPU (Radeon)" : "iGPU (Intel)");
    if (!Ioctl(h, IOCTL_GMUX_ROUTE_EXTERNAL, &req, sizeof(req), &resp, sizeof(resp))) {
        if (GetLastError() == ERROR_INVALID_FUNCTION) {
            printf("  the installed GmuxDriver.sys is too old for this command - install the new build.\n");
        }
        return false;
    }
    return resp.Success != 0;
}

static bool Power(HANDLE h, bool on)
{
    GMUX_POWER_REQUEST req = { (UCHAR)(on ? 1 : 0) };
    GMUX_POWER_RESPONSE resp = { 0 };
    printf("  dGPU power rail -> %s ...\n", on ? "ON" : "OFF");
    return Ioctl(h, IOCTL_GMUX_POWER, &req, sizeof(req), &resp, sizeof(resp)) && resp.Success;
}

static const char* RouteName(UCHAR r)
{
    return r == GMUX_ROUTE_DGPU ? "dGPU (Radeon)" : r == GMUX_ROUTE_IGD ? "iGPU (Intel)" : "(none)";
}

// Panel backlight: gmux ports 0x74 (current) / 0x70 (max), 32-bit big-endian, handled by the driver.
static bool GetBacklight(HANDLE h, ULONG* value, ULONG* max)
{
    GMUX_BL_GET_RESPONSE resp;
    ZeroMemory(&resp, sizeof(resp));
    if (!Ioctl(h, IOCTL_GMUX_BL_GET, NULL, 0, &resp, sizeof(resp))) {
        if (GetLastError() == ERROR_INVALID_FUNCTION) {
            printf("  the installed GmuxDriver.sys is too old for this command - install the new build.\n");
        }
        return false;
    }
    if (!resp.Success) return false;
    *value = resp.Value;
    *max = resp.Max;
    return true;
}

static bool SetBacklight(HANDLE h, ULONG value)
{
    GMUX_BL_SET_REQUEST req = { value };
    GMUX_BL_SET_RESPONSE resp = { 0 };
    return Ioctl(h, IOCTL_GMUX_BL_SET, &req, sizeof(req), &resp, sizeof(resp)) && resp.Success;
}

static void PrintBacklight(HANDLE h)
{
    ULONG v = 0, m = 0;
    if (!GetBacklight(h, &v, &m)) { printf("  backlight: <read failed>\n"); return; }
    if (m) printf("  backlight = %lu / %lu (%lu%%)   [port 0x74 / 0x70]\n", v, m, (ULONG)(((unsigned long long)v * 100 + m / 2) / m));
    else   printf("  backlight = %lu   [port 0x74; max (0x70) unreadable]\n", v);
}

// Probe: print every change of gmux backlight port 0x74 until a key is pressed. Run it while moving
// the Windows brightness slider (Radeon boot). A change means the stock AMD driver reaches the gmux
// (through ACPI); no change means the panel is dimmed some other way (GPU PWM / DPCD AUX).
static int CmdBacklightWatch(HANDLE h)
{
    ULONG cur = 0, max = 0, last = 0;
    if (!GetBacklight(h, &last, &max)) { printf("ABORT: cannot read the backlight.\n"); return 1; }
    printf("  watching port 0x74 (max 0x70 = %lu), start = %lu. Move the brightness slider; any key stops.\n", max, last);
    ULONG changes = 0;
    while (!_kbhit()) {
        if (GetBacklight(h, &cur, &max) && cur != last) {
            printf("  [%8lu ms] 0x74: %lu -> %lu\n", GetTickCount(), last, cur);
            last = cur;
            ++changes;
        }
        Sleep(100);
    }
    _getch();
    printf("  %lu change(s) seen.\n", changes);
    return 0;
}

// a2/a3 are the RAW argv strings (Arg() would strip the sign of "-10").
static int CmdBacklight(HANDLE h, const char* a2, const char* a3)
{
    ULONG cur = 0, max = 0;
    if (!GetBacklight(h, &cur, &max)) { printf("ABORT: cannot read the backlight.\n"); return 1; }
    if (!*a2) { PrintBacklight(h); return 0; }
    if (!_stricmp(a2, "watch")) return CmdBacklightWatch(h);

    ULONG target;
    if (!_stricmp(a2, "raw")) {
        if (!*a3) { printf("usage: gmux bl raw <value>\n"); return 1; }
        target = (ULONG)strtoul(a3, NULL, 0);
    } else {
        if (!max) { printf("ABORT: max brightness (port 0x70) unreadable - use `gmux bl raw <value>`.\n"); return 1; }
        long long pct;
        if (*a2 == '+' || *a2 == '-') {
            pct = (long long)((unsigned long long)cur * 100 / max) + strtol(a2, NULL, 10);
        } else {
            pct = strtol(a2, NULL, 10);
        }
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        target = (ULONG)((unsigned long long)max * (unsigned long long)pct / 100);
        if (target == 0) target = 1;   // 0 may switch the panel completely off
    }

    printf("  backlight %lu -> %lu ...\n", cur, target);
    if (!SetBacklight(h, target)) { printf("FAILED: backlight write.\n"); return 1; }
    PrintBacklight(h);
    return 0;
}

// ------------------------------------------------------------------- PnP side

struct GpuInfo {
    bool    found;
    ULONG   status;
    ULONG   problem;
    int     powerState;     // Windows device power state: 0 = D0 ... 3 = D3, -1 = unknown
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

    // Most recent device power state kept by the power manager (CM_POWER_DATA). D3 here means the
    // device is idle in D3 (hot or cold - Windows does not say which).
    gi->powerState = -1;
    CM_POWER_DATA pd;
    ULONG pdLen = sizeof(pd);
    ZeroMemory(&pd, sizeof(pd));
    if (CM_Get_DevNode_Registry_PropertyW(dd.DevInst, CM_DRP_DEVICE_POWER_DATA, NULL, &pd, &pdLen, 0) == CR_SUCCESS &&
        pd.PD_MostRecentPowerState >= PowerDeviceD0 && pd.PD_MostRecentPowerState <= PowerDeviceD3) {
        gi->powerState = (int)pd.PD_MostRecentPowerState - (int)PowerDeviceD0;
    }
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
    printf("  %-7ls %ls  [%ls]  status=0x%08lX problem=%lu", label, g.desc, Describe(g), g.status, g.problem);
    if (g.powerState >= 0) printf("  power=D%d", g.powerState);
    printf("\n");
}

static bool PropChange(HDEVINFO set, SP_DEVINFO_DATA* dd, DWORD stateChange)
{
    SP_PROPCHANGE_PARAMS pc;
    ZeroMemory(&pc, sizeof(pc));
    pc.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    pc.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    pc.StateChange = stateChange;
    pc.Scope = DICS_FLAG_GLOBAL;
    pc.HwProfile = 0;

    return SetupDiSetClassInstallParamsW(set, dd, &pc.ClassInstallHeader, sizeof(pc)) &&
           SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, dd);
}

// stateChange: DICS_DISABLE or DICS_ENABLE
//
// Disable is NON-persistent (CM_Disable_DevNode without CM_DISABLE_PERSIST) and there is deliberately no
// fallback to SetupDi DICS_DISABLE: that writes CONFIGFLAG_DISABLED, which survives reboots - and the
// firmware puts the panel on the dGPU at every boot, so a persistently disabled Radeon left the panel
// showing a frozen frame until it was re-enabled by hand in Device Manager. If the non-persistent
// disable fails, the command fails. Enable also clears that persistent flag, in case an older build
// or Device Manager set it.
static bool SetGpuState(const wchar_t* venTag, DWORD stateChange)
{
    HDEVINFO set;
    SP_DEVINFO_DATA dd;
    if (!OpenGpu(venTag, &set, &dd)) {
        printf("  adapter not found in PnP\n");
        return false;
    }

    bool ok = false;
    if (stateChange == DICS_DISABLE) {
        CONFIGRET cr = CM_Disable_DevNode(dd.DevInst, CM_DISABLE_UI_NOT_OK);
        ok = (cr == CR_SUCCESS);
        if (!ok) {
            printf("  CM_Disable_DevNode failed (CR 0x%lX)\n", (unsigned long)cr);
        }
    } else {
        bool cleared = PropChange(set, &dd, DICS_ENABLE);
        CONFIGRET cr = CM_Enable_DevNode(dd.DevInst, 0);
        ok = cleared || cr == CR_SUCCESS;
        if (!ok) {
            printf("  PnP enable failed: %lu (CR 0x%lX)\n", GetLastError(), (unsigned long)cr);
        }
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
            if (wantDisabled  && (g.problem == CM_PROB_DISABLED || !(g.status & DN_STARTED))) return true;
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
    UCHAR route = 0;
    printf("gmux:\n");
    PrintPort(h, GMUX_PORT_VERSION_MAJOR,      "version major");
    PrintPort(h, GMUX_PORT_SWITCH_DISPLAY,     "switch display (0x10)");
    if (PanelRoute(h, &route)) printf("         -> panel route (0x10 bit 0) = %s\n", RouteName(route));
    PrintPort(h, GMUX_PORT_SWITCH_GET_DISPLAY, "get display (0x11)");
    PrintPort(h, GMUX_PORT_SWITCH_DDC,         "ddc (0x28)");
    PrintPort(h, GMUX_PORT_SWITCH_EXTERNAL,    "switch external (0x40)");
    PrintPort(h, GMUX_PORT_SWITCH_GET_EXTERNAL,"get external (0x41)");
    PrintPort(h, GMUX_PORT_DISCRETE_POWER,     "discrete power (0x50)");
    PrintPort(h, GMUX_PORT_INTERRUPT_ENABLE,   "interrupt enable (0x14)");
    PrintPort(h, GMUX_PORT_INTERRUPT_STATUS,   "interrupt status (0x16)");
    printf("  (0x11/0x41 are informational; 0x50 cannot be read back - compare before/after a command;\n"
           "   0x16 sits at a constant 0x1D on this machine because nothing acknowledges it)\n");
    PrintBacklight(h);
    printf("adapters:\n");
    PrintGpu(L"Intel", VEN_INTEL);
    PrintGpu(L"Radeon", VEN_AMD);
}

// Switch the panel to Intel with an automatic way back, optionally followed by cutting the Radeon.
//
// Same reversible test `trial` always was: the Radeon is neither disabled nor powered down while the
// route is unconfirmed, so the revert (route -> dGPU) always has a live dGPU to go back to. Only after
// the user confirmed a readable panel does the optional `off` part run.
static int CmdToIntel(HANDLE h, bool powerOff, DWORD seconds)
{
    printf("== switch to Intel%s (confirm within %lu s) ==\n", powerOff ? " + Radeon off" : "", seconds);

    GpuInfo intel;
    if (!QueryGpu(VEN_INTEL, &intel) || !GpuRunning(intel)) {
        printf("ABORT: Intel adapter is not running (%ls) - routing the panel to it would leave no display.\n",
               intel.found ? Describe(intel) : L"not present");
        return 1;
    }

    GpuInfo amdBefore;
    if (!QueryGpu(VEN_AMD, &amdBefore) || !GpuRunning(amdBefore)) {
        printf("WARNING: the Radeon is not running - if the panel stays black there is no dGPU to fall back to.\n");
    }

    if (!Route(h, false)) { printf("ABORT: route to iGPU failed.\n"); return 1; }
    Sleep(1000);

    // Linux order is "route first, THEN i915 initialises and trains the eDP link". Restarting the Intel
    // adapter after the route approximates that: the driver re-reads the panel (EDID/DPCD) and redoes
    // link training on the newly attached display.
    printf("  restarting Intel adapter so it re-detects the panel ...\n");
    if (SetGpuState(VEN_INTEL, DICS_DISABLE)) WaitGpu(VEN_INTEL, true, 8000);
    if (SetGpuState(VEN_INTEL, DICS_ENABLE))  WaitGpu(VEN_INTEL, false, 15000);

    printf("  If you can read this on the panel, press any key within %lu s to KEEP the iGPU route.\n", seconds);
    printf("  Otherwise the panel returns to the dGPU automatically.\n");

    bool keep = false;
    for (DWORD t = 0; t < seconds * 4; ++t) {
        if (_kbhit()) { _getch(); keep = true; break; }
        Sleep(250);
    }

    if (!keep) {
        printf("  no confirmation - reverting to dGPU ...\n");
        if (!Route(h, true)) { printf("FAILED: revert route write failed - run `gmux d`.\n"); return 1; }
        CmdStatus(h);
        return 2;
    }

    if (!powerOff) {
        printf("  kept: panel stays on Intel until the next reboot (Radeon still enabled and powered).\n");
        CmdStatus(h);
        return 0;
    }

    // ---- optional: take the Radeon down. Every step is gated on the previous one being verified. ----
    printf("  confirmed - EXPERIMENTAL: disabling the Radeon and cutting its power rail.\n");
    printf("  The way back is a REBOOT (raising the rail from Windows hangs this machine).\n");
    GpuInfo amd;
    if (QueryGpu(VEN_AMD, &amd)) {
        if (amd.problem != CM_PROB_DISABLED) {
            printf("  disabling Radeon in PnP (non-persistent) ...\n");
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

// Switch the panel back to the Radeon. The power rail is NOT touched unless powerCycle is set:
// enabling the Radeon worked whenever the card never lost power, and hung the machine (then powered it
// off) whenever the rail had been cycled through gmux port 0x50 from Windows.
static int CmdToRadeon(HANDLE h, bool powerCycle)
{
    printf("== switch to Radeon ==\n");

    GpuInfo amd;
    bool present = QueryGpu(VEN_AMD, &amd);

    if (!present) {
        if (!powerCycle) {
            printf("ABORT: the Radeon is not present in PnP (its power rail is off).\n");
            printf("       Raising the rail from Windows hangs and powers off this machine (observed).\n");
            printf("       Safest: reboot - the firmware powers the Radeon and routes the panel back to it.\n");
            printf("       Experimental, may hang: gmux d power\n");
            return 1;
        }
        printf("  EXPERIMENTAL: rail power-on + PCI rescan - this is the sequence that hung the machine.\n");
        if (!Power(h, true)) { printf("ABORT: dGPU power-on failed; panel left where it was.\n"); return 1; }
        Sleep(500);
        RescanPci();
        Sleep(1000);
        if (!QueryGpu(VEN_AMD, &amd)) {
            printf("ABORT: Radeon did not reappear after power-on + PCI rescan. Panel NOT routed.\n");
            return 1;
        }
    }

    // Idempotency: already routed to a live Radeon - repeating the route write only risks dropping the
    // panel link with nothing left to retrain it.
    UCHAR cur = 0;
    if (PanelRoute(h, &cur) && cur == GMUX_ROUTE_DGPU && GpuRunning(amd)) {
        printf("  already routed to the Radeon and it is running - nothing to do.\n");
        CmdStatus(h);
        return 0;
    }

    if (amd.problem == CM_PROB_DISABLED || !GpuRunning(amd)) {
        printf("  enabling Radeon in PnP ...\n");
        if (!SetGpuState(VEN_AMD, DICS_ENABLE)) { printf("ABORT: enabling Radeon failed. Panel NOT routed.\n"); return 1; }
    }
    if (!WaitGpu(VEN_AMD, false, 15000)) {
        printf("ABORT: Radeon is not running after enable. Panel NOT routed.\n");
        return 1;
    }

    if (!Route(h, true)) { printf("FAILED: route to dGPU failed.\n"); return 1; }

    // A plain route write does not guarantee the eDP link gets retrained on this hardware - bounce the
    // adapter so it re-detects the panel (same idea as `i` does for Intel).
    printf("  restarting Radeon adapter so it re-detects the panel ...\n");
    if (SetGpuState(VEN_AMD, DICS_DISABLE)) WaitGpu(VEN_AMD, true, 8000);
    if (SetGpuState(VEN_AMD, DICS_ENABLE))  WaitGpu(VEN_AMD, false, 15000);

    printf("== done ==\n");
    CmdStatus(h);
    return 0;
}

// Radeon as a second GPU, driven from Windows: `on` enables it in PnP, `off` disables it in PnP
// (non-persistent, see SetGpuState). Nothing is written to gmux - no panel route, DDC, external mux and
// no power rail - so the panel stays exactly on the GPU it is on, and the rail has ONE owner: the ACPI
// power resource of loader mode 6 (acpi patch SSDT_IGPU_PWR.aml). Windows puts the idle, disabled Radeon
// into D3 and the resource cuts the rail when the whole chain (PEG0 > EGP0 > EGP1 > GFX0 / HDAU) is in D3;
// `on` brings it back (the resource powers the rail before the device wakes). Whether Windows really goes
// to D3cold is up to the OS and the driver: `status` shows power=D0 / D3 for the Radeon.
//
// Writing port 0x50 from here (the old `off` / `on` did) while that resource owns the rail would put ACPI
// state and hardware out of step, and powering the card up with port 0x50 alone hung this machine.
// The panel route is read before and after and reported.
static int CmdRadeon(HANDLE h, bool enable)
{
    printf("== Radeon %s (PnP only: gmux is not written, Windows decides about the rail) ==\n", enable ? "ON" : "OFF");

    UCHAR routeBefore = 0;
    bool haveRoute = PanelRoute(h, &routeBefore);
    if (haveRoute) printf("  panel route now: %s\n", RouteName(routeBefore));

    GpuInfo amd;
    if (!QueryGpu(VEN_AMD, &amd)) {
        printf("ABORT: the Radeon is not present in PnP (its rail is off).\n");
        printf("       It cannot be powered up from here. Boot loader mode 6 (Radeon ON at boot) or reboot.\n");
        return 1;
    }

    if (enable) {
        if (GpuRunning(amd)) {
            printf("  Radeon is running.\n");
        } else {
            printf("  enabling Radeon in PnP ...\n");
            if (!SetGpuState(VEN_AMD, DICS_ENABLE) || !WaitGpu(VEN_AMD, false, 15000)) {
                printf("FAILED: Radeon is not running after enable.\n");
                CmdStatus(h);
                return 1;
            }
        }
    } else {
        if (amd.problem == CM_PROB_DISABLED) {
            printf("  Radeon already disabled in PnP.\n");
        } else {
            if (haveRoute && routeBefore == GMUX_ROUTE_DGPU) {
                printf("  WARNING: the panel is routed to the Radeon - it will freeze or go black until\n"
                       "           `gmux radeon on` (or a reboot).\n");
            }
            printf("  disabling Radeon in PnP (non-persistent) ...\n");
            if (!SetGpuState(VEN_AMD, DICS_DISABLE) || !WaitGpu(VEN_AMD, true, 8000)) {
                printf("FAILED: Radeon is not confirmed disabled.\n");
                CmdStatus(h);
                return 1;
            }
        }
    }

    UCHAR routeAfter = 0;
    if (haveRoute && PanelRoute(h, &routeAfter)) {
        if (routeAfter == routeBefore) printf("  panel route unchanged: %s\n", RouteName(routeAfter));
        else printf("  WARNING: panel route changed to %s - something else moved the mux.\n", RouteName(routeAfter));
    }

    // The power manager needs a moment to take the idle device down to D3.
    if (!enable) Sleep(2000);
    printf("== done ==\n");
    CmdStatus(h);
    return 0;
}

// READ-ONLY look at the EFI variable the Mac firmware uses to pick the boot GPU
// (gpu-power-prefs-fa4ce28d-b62f-4c99-9cc3-6815686e30f9; first data byte 1 = iGPU, 0 = dGPU - see the
// apple-gmux.c documentation and 0xbb/gpu-switch). At boot the firmware itself routes the panel and
// gives the chosen GPU a framebuffer, which is the only iGPU path known to work on T2 (see README).
// This tool never writes it: use gpu-switch for that, and read the README first.
static int CmdEfi()
{
    HANDLE tok = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        TOKEN_PRIVILEGES tp;
        ZeroMemory(&tp, sizeof(tp));
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (LookupPrivilegeValueW(NULL, L"SeSystemEnvironmentPrivilege", &tp.Privileges[0].Luid)) {
            AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
        }
        CloseHandle(tok);
    }

    typedef DWORD (WINAPI *PFN_GetFwVarEx)(LPCWSTR, LPCWSTR, PVOID, DWORD, PDWORD);
    FARPROC proc = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetFirmwareEnvironmentVariableExW");
    PFN_GetFwVarEx fn = reinterpret_cast<PFN_GetFwVarEx>(reinterpret_cast<void*>(proc));
    if (!fn) { printf("GetFirmwareEnvironmentVariableExW is not available on this Windows.\n"); return 1; }

    BYTE buf[64] = { 0 };
    DWORD attr = 0;
    DWORD n = fn(L"gpu-power-prefs", L"{FA4CE28D-B62F-4C99-9CC3-6815686E30F9}", buf, sizeof(buf), &attr);
    if (n == 0) {
        DWORD e = GetLastError();
        if (e == ERROR_ENVVAR_NOT_FOUND) {
            printf("gpu-power-prefs: not set (the firmware uses its default: dGPU).\n");
            return 0;
        }
        printf("gpu-power-prefs: cannot read (error %lu)%s\n", e,
               e == ERROR_INVALID_FUNCTION ? " - Windows is not booted in UEFI mode." :
               e == ERROR_PRIVILEGE_NOT_HELD ? " - run as Administrator." : ".");
        return 1;
    }
    printf("gpu-power-prefs: %lu byte(s), attributes 0x%lX, data:", n, attr);
    for (DWORD i = 0; i < n; ++i) printf(" %02X", buf[i]);
    printf("\n  first byte %02X -> firmware boot GPU: %s\n", buf[0],
           buf[0] == 1 ? "iGPU (Intel)" : buf[0] == 0 ? "dGPU (Radeon)" : "(unrecognised)");
    return 0;
}

static void Usage()
{
    printf("Usage (Administrator):\n"
           "  gmux i [off] [sec]         panel -> Intel with auto-revert unless a key is pressed within\n"
           "                             [sec] (default 20); `off` then also cuts the Radeon (experimental)\n"
           "  gmux d [power]             panel -> Radeon (never touches the power rail unless `power`)\n"
           "                             (-i / -d also accepted)\n"
           "  gmux radeon on|off         PnP enable/disable of the Radeon only: no gmux port is written, the\n"
           "                             rail belongs to the ACPI power resource of loader mode 6 (Windows D3)\n"
           "                             not persistent (i, d and radeon last only until the next reboot)\n"
           "  gmux status                ports + Intel/Radeon PnP state\n"
           "  gmux trial [sec]           same as `gmux i`\n"
           "  gmux route i|d             DDC + panel mux\n"
           "  gmux ext i|d               external DP mux only\n"
           "  gmux power on|off          dGPU power rail only (dangerous, see README)\n"
           "  gmux amd disable|enable    same as `gmux radeon off|on`\n"
           "  gmux read <hexport>        raw port read, e.g. gmux read 50\n"
           "  gmux write <hexport> <hexval>   raw port write, e.g. gmux write 16 1D\n"
           "  gmux bl [<0-100>|+N|-N|raw <v>]   panel backlight via gmux (no arg = show)\n"
           "  gmux efi                   READ-ONLY: show the firmware's boot GPU preference\n"
           "                             (gpu-power-prefs EFI variable); no driver needed\n");
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

    if (!_stricmp(cmd, "efi")) return CmdEfi();   // needs no driver

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
    const char* a3 = Arg(argc, argv, 3);

    if (!_stricmp(cmd, "i") || !_stricmp(cmd, "trial")) {
        bool off = false;
        DWORD sec = 20;
        for (int k = 2; k < argc; ++k) {
            const char* a = Arg(argc, argv, k);
            if (!_stricmp(a, "off")) off = true;
            else if (*a) { DWORD v = (DWORD)strtoul(a, NULL, 10); if (v >= 5 && v <= 300) sec = v; }
        }
        if (!_stricmp(cmd, "trial")) off = false;
        rc = CmdToIntel(h, off, sec);
    }
    else if (!_stricmp(cmd, "d"))       rc = CmdToRadeon(h, !_stricmp(a2, "power"));
    else if (!_stricmp(cmd, "status"))  { CmdStatus(h); rc = 0; }
    else if (!_stricmp(cmd, "route") && (*a2 == 'i' || *a2 == 'I' || *a2 == 'd' || *a2 == 'D')) {
        rc = Route(h, *a2 == 'd' || *a2 == 'D') ? 0 : 1;
        CmdStatus(h);
    }
    else if (!_stricmp(cmd, "ext") && (*a2 == 'i' || *a2 == 'I' || *a2 == 'd' || *a2 == 'D')) {
        rc = RouteExternal(h, *a2 == 'd' || *a2 == 'D') ? 0 : 1;
        CmdStatus(h);
    }
    else if (!_stricmp(cmd, "power") && (!_stricmp(a2, "on") || !_stricmp(a2, "off"))) {
        if (!_stricmp(a2, "on")) {
            printf("WARNING: raising the rail from Windows and then enabling the Radeon hung this machine.\n"
                   "         Do NOT enable the Radeon afterwards unless you are prepared for a power-off.\n");
        }
        rc = Power(h, !_stricmp(a2, "on")) ? 0 : 1;
        CmdStatus(h);
    }
    else if ((!_stricmp(cmd, "radeon") || !_stricmp(cmd, "amd")) &&
             (!_stricmp(a2, "on") || !_stricmp(a2, "enable") || !_stricmp(a2, "off") || !_stricmp(a2, "disable"))) {
        rc = CmdRadeon(h, !_stricmp(a2, "on") || !_stricmp(a2, "enable"));
    }
    else if (!_stricmp(cmd, "bl") || !_stricmp(cmd, "backlight")) {
        rc = CmdBacklight(h, argc > 2 ? argv[2] : "", argc > 3 ? argv[3] : "");
    }
    else if (!_stricmp(cmd, "read") && *a2) {
        UCHAR port = (UCHAR)strtoul(a2, NULL, 16);
        PrintPort(h, port, "raw");
        rc = 0;
    }
    else if (!_stricmp(cmd, "write") && *a2 && *a3) {
        UCHAR port = (UCHAR)strtoul(a2, NULL, 16);
        UCHAR val  = (UCHAR)strtoul(a3, NULL, 16);
        if (port == GMUX_PORT_DISCRETE_POWER) {
            printf("Refusing a raw write to port 0x50 - use `gmux power on|off` (it does the 2-write sequence).\n");
        } else {
            PrintPort(h, port, "before");
            printf("  raw write port 0x%02X <- 0x%02X ...\n", port, val);
            rc = Write8(h, port, val) ? 0 : 1;
            PrintPort(h, port, "after");
        }
    }
    else Usage();

    CloseHandle(h);
    return rc;
}
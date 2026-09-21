// gmux.cpp - console client for GmuxCtl.
// Uses IOCTL_GMUX_SWITCH_TO (sequenced power + display route), not a raw
// display-route-only write - that path is what produced the black screen.

#include <windows.h>
#include <stdio.h>
#include <devioctl.h>
#include "public.h"   // shared IOCTL/struct definitions

HANDLE OpenGmux() {
    return CreateFile(L"\\\\.\\GmuxCtl", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
}

BOOL Read8(HANDLE h, UCHAR port, PUCHAR value) {
    GMUX_READ_REQUEST req = { port };
    GMUX_READ_RESPONSE resp = { 0 };
    DWORD bytes;
    if (!DeviceIoControl(h, IOCTL_GMUX_READ8, &req, sizeof(req), &resp, sizeof(resp), &bytes, NULL)) {
        printf("DeviceIoControl READ8 failed: %lu\n", GetLastError());
        return FALSE;
    }
    if (resp.Success) *value = resp.Value;
    return resp.Success;
}

BOOL SwitchTo(HANDLE h, BOOL toDiscrete) {
    GMUX_SWITCH_REQUEST req = { (UCHAR)(toDiscrete ? 1 : 0) };
    GMUX_SWITCH_RESPONSE resp = { 0 };
    DWORD bytes;
    if (!DeviceIoControl(h, IOCTL_GMUX_SWITCH_TO, &req, sizeof(req), &resp, sizeof(resp), &bytes, NULL)) {
        printf("DeviceIoControl SWITCH_TO failed: %lu\n", GetLastError());
        return FALSE;
    }
    return resp.Success;
}

int main(int argc, char** argv) {
    if (argc != 2 || (argv[1][0] != 'i' && argv[1][0] != 'd')) {
        printf("Usage: gmux.exe i|d\n");
        printf("  i = switch to iGPU (power dGPU rails down after routing)\n");
        printf("  d = switch to dGPU (power dGPU rails up before routing)\n");
        return 1;
    }

    HANDLE h = OpenGmux();
    if (h == INVALID_HANDLE_VALUE) {
        printf("Cannot open GmuxCtl (%lu). Check driver is installed and loaded.\n", GetLastError());
        return 1;
    }

    UCHAR ver;
    if (Read8(h, GMUX_PORT_VERSION_MAJOR, &ver)) {
        printf("GMUX version: %d (0x%02X)\n", ver, ver);
    } else {
        printf("Failed to read version (hardware not present?)\n");
        CloseHandle(h);
        return 1;
    }

    BOOL toDiscrete = (argv[1][0] == 'd');
    printf("Switching to %s ...\n", toDiscrete ? "dGPU" : "iGPU");

    BOOL ok = SwitchTo(h, toDiscrete);
    printf(ok ? "Switch reported success.\n" : "Switch reported FAILURE.\n");

    CloseHandle(h);
    return ok ? 0 : 1;
}
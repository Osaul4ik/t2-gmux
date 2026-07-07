// gmux_app.c – консольний клієнт для GmuxCtl
#include <windows.h>
#include <stdio.h>
#include <devioctl.h>
#include "public.h"   // ваш файл з IOCTL та структурами

HANDLE OpenGmux() {
    return CreateFile(L"\\\\.\\GmuxCtl", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
}

BOOL Read8(HANDLE h, UCHAR port, PUCHAR value) {
    GMUX_READ_REQUEST req = { port };
    GMUX_READ_RESPONSE resp = { 0 };
    DWORD bytes;
    if (!DeviceIoControl(h, IOCTL_GMUX_READ8, &req, sizeof(req), &resp, sizeof(resp), &bytes, NULL)) {
        printf("DeviceIoControl READ8 failed: %d\n", GetLastError());
        return FALSE;
    }
    if (resp.Success) *value = resp.Value;
    return resp.Success;
}

BOOL Write8(HANDLE h, UCHAR port, UCHAR value) {
    GMUX_WRITE_REQUEST req = { port, value };
    GMUX_WRITE_RESPONSE resp = { 0 };
    DWORD bytes;
    if (!DeviceIoControl(h, IOCTL_GMUX_WRITE8, &req, sizeof(req), &resp, sizeof(resp), &bytes, NULL)) {
        printf("DeviceIoControl WRITE8 failed: %d\n", GetLastError());
        return FALSE;
    }
    return resp.Success;
}

int main() {
    HANDLE h = OpenGmux();
    if (h == INVALID_HANDLE_VALUE) {
        printf("Cannot open GmuxCtl. Check driver is installed.\n");
        return 1;
    }

    // ---- Читання версії ----
    UCHAR ver;
    if (Read8(h, GMUX_PORT_VERSION_MAJOR, &ver)) {
        printf("GMUX version: %d (0x%02X)\n", ver, ver);
    } else {
        printf("Failed to read version (maybe hardware not present)\n");
        CloseHandle(h);
        return 1;
    }

    // ---- Читання поточного стану перемикача ----
    UCHAR state;
    if (Read8(h, GMUX_PORT_SWITCH_DISPLAY, &state)) {
        printf("Current display switch state: %d (0x%02X) – %s\n",
               state, state, (state % 2 == 0) ? "iGPU" : "dGPU");
    }

    // ---- Потужність dGPU ----
    UCHAR power;
    if (Read8(h, GMUX_PORT_DISCRETE_POWER, &power)) {
        printf("Discrete GPU power: 0x%02X (%s)\n", power, power ? "ON" : "OFF");
    }

    printf("\n============================================\n");
    printf("WARNING: Writing to port 0x10 can switch display.\n");
    printf("On MacBookPro16,1 reverse switching may NOT work.\n");
    printf("Value 2 = force iGPU, 3 = force dGPU (typical).\n");
    printf("Do you want to write value 2 (force iGPU)? (y/n): ");

    char c = getchar();
    if (c == 'y' || c == 'Y') {
        if (Write8(h, GMUX_PORT_SWITCH_DISPLAY, 2)) {
            printf("Write successful. Check display.\n");
        } else {
            printf("Write failed.\n");
        }
    } else {
        printf("No write performed.\n");
    }

    CloseHandle(h);
    return 0;
}
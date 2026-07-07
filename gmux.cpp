// gmux_app.cpp
//
// Usermode тест-застосунок для GmuxDriver.sys.
// Відкриває \\.\GmuxCtl і шле IOCTL_GMUX_READ8 / IOCTL_GMUX_WRITE8.
//
// Компіляція (mingw-w64 або cl):
//   g++ gmux_app.cpp -o gmux_app.exe
//
// Запуск: від імені адміністратора, ПІСЛЯ встановлення драйвера.

#include <windows.h>
#include <cstdio>
#include <cstring>
#include "public.h"

static HANDLE OpenGmuxDevice()
{
    HANDLE h = CreateFileW(
        L"\\\\.\\GmuxCtl",
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (h == INVALID_HANDLE_VALUE)
    {
        printf("Не вдалося відкрити \\\\.\\GmuxCtl (код помилки: %lu). "
               "Драйвер встановлено і сервіс запущено?\n", GetLastError());
    }
    return h;
}

static bool GmuxRead8(HANDLE h, UCHAR port, UCHAR* value)
{
    GMUX_READ_REQUEST req{ port };
    GMUX_READ_RESPONSE resp{};
    DWORD bytesReturned = 0;

    BOOL ok = DeviceIoControl(
        h, IOCTL_GMUX_READ8,
        &req, sizeof(req),
        &resp, sizeof(resp),
        &bytesReturned, NULL);

    if (!ok) return false;
    *value = resp.Value;
    return resp.Success != FALSE;
}

static bool GmuxWrite8(HANDLE h, UCHAR port, UCHAR value)
{
    GMUX_WRITE_REQUEST req{ port, value };
    GMUX_WRITE_RESPONSE resp{};
    DWORD bytesReturned = 0;

    BOOL ok = DeviceIoControl(
        h, IOCTL_GMUX_WRITE8,
        &req, sizeof(req),
        &resp, sizeof(resp),
        &bytesReturned, NULL);

    if (!ok) return false;
    return resp.Success != FALSE;
}

int main(int argc, char** argv)
{
    HANDLE h = OpenGmuxDevice();
    if (h == INVALID_HANDLE_VALUE) return 1;

    UCHAR val = 0;

    if (GmuxRead8(h, GMUX_PORT_VERSION_MAJOR, &val))
        printf("GMUX_PORT_VERSION_MAJOR = 0x%02X\n", val);
    else
        printf("Не вдалося прочитати VERSION_MAJOR (handshake не спрацював).\n");

    if (GmuxRead8(h, GMUX_PORT_SWITCH_DISPLAY, &val))
        printf("GMUX_PORT_SWITCH_DISPLAY (поточне) = 0x%02X (%s)\n",
               val, (val % 2 == 0) ? "iGPU" : "dGPU");
    else
        printf("Не вдалося прочитати SWITCH_DISPLAY.\n");

    if (argc > 1 && strcmp(argv[1], "--force-igpu") == 0)
    {
        printf("Записую SWITCH_DISPLAY = 2 (форсувати iGPU)...\n");
        if (GmuxWrite8(h, GMUX_PORT_SWITCH_DISPLAY, 2))
            printf("Запис виконано.\n");
        else
            printf("Запис не вдався / timeout.\n");
    }
    else
    {
        printf("\n(запусти з --force-igpu щоб спробувати примусово перемкнути на iGPU)\n");
    }

    CloseHandle(h);
    return 0;
}
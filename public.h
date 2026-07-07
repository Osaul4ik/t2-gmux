// public.h
// Спільні визначення IOCTL між GmuxDriver.sys та usermode-застосунком (gmux_app.cpp).
// Цей файл включається в обидва проєкти.

#pragma once

#define IOCTL_GMUX_READ8  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GMUX_WRITE8 CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _GMUX_READ_REQUEST {
    UCHAR Port;
} GMUX_READ_REQUEST, *PGMUX_READ_REQUEST;

typedef struct _GMUX_READ_RESPONSE {
    UCHAR   Value;
    BOOLEAN Success;
} GMUX_READ_RESPONSE, *PGMUX_READ_RESPONSE;

typedef struct _GMUX_WRITE_REQUEST {
    UCHAR Port;
    UCHAR Value;
} GMUX_WRITE_REQUEST, *PGMUX_WRITE_REQUEST;

typedef struct _GMUX_WRITE_RESPONSE {
    BOOLEAN Success;
} GMUX_WRITE_RESPONSE, *PGMUX_WRITE_RESPONSE;

// Номери "портів" gmux (з Linux apple-gmux.h)
#define GMUX_PORT_VERSION_MAJOR  0x04
#define GMUX_PORT_SWITCH_DISPLAY 0x10
#define GMUX_PORT_DISCRETE_POWER 0x50
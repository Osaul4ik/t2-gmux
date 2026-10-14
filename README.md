# t2-gmux
# DO NOT USE! IF YOU THINK YOU CAN, NO, YOU CAN'T.
### THIS IS FOR THOSE WHO KNOW WHAT THEY'RE DOING.

KMDF driver (`GmuxDriver.sys`) + console client (`gmux.exe`) for the Apple gmux on a T2 MacBook Pro
under Windows (MMIO gmux, ACPI\APP000B). Protocol taken from Linux `drivers/platform/x86/apple-gmux.c`.

## Typical use (Administrator)

    gmux status            ports, decoded panel route, Intel/Radeon state
    gmux i                 panel -> Intel, auto-reverts to the Radeon unless a key is pressed in 20 s
    gmux d                 panel -> Radeon
    gmux radeon on|off     enable / disable the Radeon in PnP only (no gmux port is written)

Nothing is remembered between boots: every Windows boot starts as the firmware left the hardware, so
`i`, `d` and `radeon` only last until the next reboot.

`gmux radeon on|off` never writes a gmux port (no panel route, DDC, external mux or power rail): it is a
non-persistent PnP enable / disable, so the panel stays on the GPU it is on. The power rail has one owner:
the ACPI power resource of loader mode 6 (`SSDT_IGPU_PWR.aml`, repo `t2-gmux-uefi`, branch `dGPU`). With that
patch Windows puts the disabled, idle Radeon into D3 and the resource cuts the rail; `gmux radeon on` (or
enabling it in Device Manager) brings it back. `gmux status` shows `power=D0` / `D3` for the Radeon and for every node of its PCI chain (the rail can drop only when all of them are D3; port 0x50 reads 3 while the rail is on). Without
that patch `radeon off` only disables the adapter and the rail stays on. If the Radeon is gone from PnP the
command refuses: it cannot be powered up from Windows (that hung this machine), reboot or use mode 6.

`gmux i` keeps the Radeon enabled and powered, so the revert always has something to go back to.
`gmux i off` additionally disables the Radeon (non-persistent) and cuts its rail - experimental, the
way back is a reboot.

## What is known about this hardware

Measured on a MacBookPro16,1 (Radeon Pro 5300M + UHD 630):

- The firmware puts the panel on the dGPU at every boot and powers the Radeon (port 0x50 reads 3).
- Panel route readback is port **0x10 bit 0** (1 = dGPU, 0 = iGPU), like Linux (0x05 = dGPU, 0x04 = iGPU).
  Port 0x11 reads 3 / 0 (never 2). Port 0x41 is **not** a route indicator: it read 0x00 at boot and 0x10
  after any write to 0x40, with the panel on the dGPU.
- A route write is acknowledged and changes 0x10, and the Intel adapter restarts cleanly, but the panel
  stays black: the mux moves, the iGPU still does not drive the panel. The auto-revert works and the
  Radeon is untouched, so `gmux i` is safe to experiment with.
- DDC (0x28) reads 0x00 before and after a write of 1, so the write is a no-op on this gmux. It is still
  written because Linux does. It is **not** the cause of the black panel (Linux itself says retina/T2
  machines cannot switch the panel's AUX separately and "need eDP pre-calibration").
- Interrupt status 0x16 sits at a constant 0x1D; nothing acknowledges it (Linux clears it in its GPE
  handler), so "power done" cannot be read from it.
- External mux 0x40: Linux never switches it on Thunderbolt Macs (forces 3 = dGPU). The driver does the
  same: `route` / `i` / `d` always leave it on the dGPU; `gmux ext i|d` moves it by hand.
- Enabling the Radeon works if the card never lost power. After the rail was cycled through port 0x50
  from Windows (`gmux power on` / `gmux d power`), enabling it freezes the CPU (no BSOD) and the laptop
  powers off - the "system freeze, then sudden shutdown (CPU CATERR)" the t2linux wiki also describes for
  this AMD GPU. Working theory: Windows does not know the card lost power (PnP disable is not D3cold).

- Radeon rail off since boot (port 0x50 reads 0, Radeon absent from PnP), AMD driver installed: `gmux radeon on`
  (rail on + PCI rescan) hung the machine. With the AMD driver removed there was no hang, but the Radeon
  showed up as Microsoft Basic Display Adapter with problem 31 (`CM_PROB_FAILED_ADD`). Working theory: the
  firmware never initialised the card this boot, so the AMD driver cannot start it. The rail should be
  raised before the OS (UEFI stage), not from Windows.

## Radeon power from Windows

Apple's own AML (ssdt7, table id `PEG0GFX0`) already contains the correct power-up/power-down sequence for the
Radeon: `GFX0.PWRD` -> `PUPD` writes gmux port 0x50, parks/enables the lanes, retrains the PCIe link and saves/
restores the Navi bridge bus numbers (`SRBN`). Writing port 0x50 alone (what `gmux power on` and the old
`radeon on` did) skips all of that, which is the likely root cause of the hang and of code 12 (hypothesis).
Loader mode 6 of `t2-gmux-uefi` (branch `dGPU`) ships an ACPI patch that wraps `PWRD` in a power resource for
the whole Radeon chain, plus a `_WAK` that puts the panel back on the iGPU after sleep. It is untested on
hardware; see `docs/ACPI_PATCH_GUIDE_MODE6.md` and `docs/TECHNICAL.md` there.

## Why runtime switching to the iGPU is not expected to work (and what does)

- The T2 Linux patch that adds `apple_gmux.force_igd` says it exists because runtime switching does not
  work: for that "both gpus need to be able to probe the eDP panel". It works only because the route is
  switched at gmux probe time, before the iGPU driver's first probe of the panel
  (`vga_switcheroo_client_probe_defer` holds the GPU drivers back until then). Routing after the Intel
  driver is already up and restarting it is a runtime switch.
- The apple-gmux documentation says the boot GPU is chosen by the firmware from the EFI variable
  `gpu-power-prefs-fa4ce28d-b62f-4c99-9cc3-6815686e30f9` (first data byte 1 = iGPU, 0 = dGPU): the firmware
  routes the panel and the external port and allocates a framebuffer for that GPU.
- The t2linux hybrid-graphics page says for Windows: the iGPU only works if no driver is installed for it,
  and to switch with 0xbb/gpu-switch. So an Intel display driver may be the blocker even after a
  firmware-level switch. That is a community statement, not verified here.
- `gmux efi` reads that variable (read-only). gpu-switch writes it (`integrated.bat`, needs UEFI-mode
  Windows, Administrator and a reboot). Its tested-hardware list has no T2 model, and a wrong value can
  leave the panel black after the reboot. If that happens: connect remotely and run `dedicated.bat`, or
  reset NVRAM (Option-Command-P-R at power-on; this also resets EFI boot settings).

## Recovery

- An older build stored a boot default in `Parameters\DefaultRoute` under the service key above. This
  build ignores it; remove it with
  `reg delete HKLM\SYSTEM\CurrentControlSet\Services\GmuxDriver\Parameters /v DefaultRoute /f`.
- Radeon missing after `gmux i off`: reboot. Do not use `gmux d power` unless you accept a power-off.
- `gmux radeon off` (alias `gmux amd disable`) is not persistent. If an older build left the Radeon disabled,
  `gmux radeon on` (or Device Manager) clears it.

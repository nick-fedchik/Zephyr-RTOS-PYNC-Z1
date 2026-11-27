# ADR 0006: Zynq-7000 hwinfo reset cause and `sys_reboot()`

- **Status:** Accepted (implemented, verified on hardware; see "Open items")
- **Issue:** [#11 `hwinfo` reset cause + `sys_reboot()` via SLCR](https://github.com/nick-fedchik/Zephyr-RTOS-PYNC-Z1/issues/11)
- **Zephyr version:** `v4.5.0`
- **Platform:** Digilent PYNQ-Z1 (Xilinx Zynq-7000 `XC7Z020`, dual Cortex-A9, 512 MiB DDR3)
- **Delivered as:** `patches/0008-hwinfo-zynq7000-reset-cause-and-reboot.patch`
- **References:** Xilinx UG585 (chapter 26, SLCR: `REBOOT_STATUS`, `PSS_RST_CTRL`,
  `SLCR_LOCK`/`SLCR_UNLOCK`), Linux `7.3.6` `drivers/watchdog/cadence_wdt.c`

## Context

Zephyr `v4.5.0` has no `hwinfo` driver and no `sys_arch_reboot()` for the Zynq-7000.
`kernel reboot cold` therefore ended in the generic fallback (halt), and the only
way to learn the reset cause was the application-level `wdt cause` command from
ADR 0003. Issue #11 asks for the standard `hwinfo` API and a working reboot.

The SLCR `REBOOT_STATUS` register (`0xF8000258`) records the last reset source in
bits 22:16 (POR 22, SRST_B 21, DBG 20, SLC soft reset 19, AWDT1 18, AWDT0 17,
SWDT 16) and a software scratch field in bits 31:24. Its reset value is
`0x00400000`; the flags are sticky across every reset except POR.

Constraints are as in ADR 0001-0005: no Vivado, numbered upstream patches with
disjoint hunks, nothing lab-specific in the repository.

## Decisions

### D1. A new `hwinfo_zynq7000.c` driver selected by the SoC family
`CONFIG_HWINFO_ZYNQ7000` defaults to `y` on `SOC_FAMILY_XILINX_ZYNQ7000` and selects
`HWINFO_HAS_DRIVER`. The SLCR base address comes from the `slcr` devicetree node.

### D2. Mapping of reset sources to `hwinfo` flags
POR to `RESET_POR`, SRST_B to `RESET_PIN`, DBG to `RESET_DEBUG`, SLC soft reset to
`RESET_SOFTWARE`, and AWDT1/AWDT0/SWDT to `RESET_WATCHDOG`. `hwinfo_get_supported_reset_cause()`
reports exactly these five.

### D3. `hwinfo_clear_reset_cause()` unlocks the SLCR and preserves the scratch field
The SLCR is unlocked (`0xDF0D`) only if `LOCKSTA` says it is locked, bits 22:16 are
cleared with bits 31:24 kept, then the SLCR is locked again (`0x767B`). The call
returns `-EIO` if flags remain set after the write.

### D4. `sys_arch_reboot()` in `soc/xlnx/zynq7000/common/reboot.c`
It locks IRQs, unlocks the SLCR, writes `PSS_RST_CTRL` bit 0 and spins until the
reset happens. It is a separate file added to `soc/xlnx/zynq7000/CMakeLists.txt`
under `CONFIG_REBOOT`, so no hunk touches `soc.c` (already modified by patches
0002 and 0006). The reset goes through BootROM, so with `JP4=SD` U-Boot and the
TFTP netboot start again.

### D5. The application clears the cause at boot
Because the flags are sticky, `main()` prints `Reset cause: ...` and then calls
`hwinfo_clear_reset_cause()`. Each boot reports its own cause. The `wdt cause`
command and its direct SLCR access were removed from the test app.

## Consequences

### Positive
- Standard `hwinfo reset_cause get|supported|clear` shell works.
- `kernel reboot cold` and `sys_reboot()` work and return to the netboot flow.
- The watchdog test no longer needs raw SLCR/syscon access.

### Negative
- `SOFTWARE` cannot distinguish `sys_reboot()` from other writes to `PSS_RST_CTRL`.
- The three watchdogs (AWDT0/1, SWDT) all map to `RESET_WATCHDOG`.
- A boot stage that clears `REBOOT_STATUS` first (e.g. a custom FSBL) hides the cause.

## Verification

On a live PYNQ-Z1 (standalone, TFTP netboot):

| Action | Boot log |
|--------|----------|
| Power cycle | `Reset cause: 0x08 POR` |
| `kernel reboot cold` | `Reset cause: 0x02 SOFTWARE`, board returns to U-Boot/Zephyr without SRST |
| `wdt start 500` + `wdt hang` | `Reset cause: 0x10 WATCHDOG` |
| `hwinfo reset_cause supported` | pin, software, power-on reset, watchdog, debug |

All eight patches apply to a fresh `v4.5.0` and reverse-check on the working tree;
the standalone image and `./amp/build_amp.sh` both build.

## Open items

- `hwinfo devid` is not provided: the PS has no unique device ID (the PL DNA needs
  a bitstream and DevCfg access).
- The SRST_B (`RESET_PIN`) and DBG (`RESET_DEBUG`) causes are mapped from the UG585
  bit definitions but were not part of the recorded hardware test matrix.

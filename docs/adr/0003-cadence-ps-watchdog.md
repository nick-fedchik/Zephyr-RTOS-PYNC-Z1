# ADR 0003: Cadence PS watchdog driver (`wdt_cdns`)

- **Status:** Accepted (implemented, verified on hardware; see "Open items")
- **Issue:** [#1 `[P3][Easy]` Cadence PS Watchdog driver](https://github.com/nick-fedchik/Zephyr-RTOS-PYNC-Z1/issues/1)
- **Zephyr version:** `v4.5.0`
- **Platform:** Digilent PYNQ-Z1 (Xilinx Zynq-7000 `XC7Z020`)
- **Delivered as:** `patches/0005-drivers-watchdog-add-cadence-wdt.patch`
- **References:** Xilinx UG585 (chapter 8, System Watchdog Timer; SLCR `REBOOT_STATUS`),
  Linux `7.3.6` `drivers/watchdog/cadence_wdt.c`

## Context

The Zynq-7000 PS has a system watchdog timer (SWDT, Cadence WDT, compatible
`cdns,wdt-r1p2`) at `0xF8005000`. Zephyr `v4.5.0` only has watchdogs for the PL
AXI timebase core and for Versal/ZynqMP, so standalone Zephyr on the PYNQ-Z1 has no
hardware watchdog. The peripheral has four registers (ZMR, CCR, RESTART, SR).

Constraints are the same as in ADR 0001 and ADR 0002: no Vivado, all upstream
changes as numbered patches with disjoint hunks, nothing lab-specific in the
repository.

## Decisions

### D1. Watchdog API, one channel

The driver is `drivers/watchdog/wdt_cdns.c` (`DT_DRV_COMPAT cdns_wdt_r1p2`) and
implements `wdt_install_timeout`, `wdt_setup`, `wdt_feed` and `wdt_disable`. The
hardware has a single timeout, so a second `wdt_install_timeout()` returns
`-ENOMEM`, an install after `wdt_setup()` returns `-EBUSY`, and `wdt_disable()`
uninstalls the timeout as required by the Zephyr API documentation. Window mode is
not available in the hardware (`window.min` must be 0, otherwise `-EINVAL`).

### D2. Expiry actions

- `WDT_FLAG_RESET_SOC` sets `ZMR.RSTEN`: expiry resets the processing system.
- `WDT_FLAG_RESET_NONE` sets `ZMR.IRQEN` and calls the installed callback from the
  interrupt (`GIC_SPI 9`, edge). The callback can feed the watchdog.
- `WDT_FLAG_RESET_CPU_CORE` returns `-ENOTSUP`.
- `WDT_FLAG_RESET_SOC` together with a callback returns `-ENOTSUP`. The reset and
  the interrupt are raised together, so a "pre-reset" callback cannot run. The
  stock Zephyr watchdog sample handles this (`Callback support rejected, continuing anyway`).

The reset pulse length is 16 PCLK cycles (`RSTLEN`), as in Linux.

### D3. Timeout calculation

The counter is loaded from a 12-bit reload value `CRV` (units of 4096 counter
clocks) and clocked by `clock / prescaler`. Linux fixes the prescaler per clock
rate; this driver picks the finest of /64, /512 and /4096 whose `CRV` covers the
requested time, so short timeouts keep a good resolution:

    units = ceil(timeout_ms * clock / 1000 / (4096 * prescaler));  CRV = units - 1

(the counter expires after `CRV + 1` units, same convention as Linux). At
108.33 MHz the supported range is roughly 2.4 ms (finest step) to 10 min.
Timeouts above the range return `-EINVAL`.

### D4. Clock rate comes from the devicetree

There is no clock controller driver for the Zynq-7000 PS in Zephyr. After reset
the SWDT runs from `CPU_1x` (`SLCR.WDT_CLK_SEL = 0`), which is `650 MHz / 6 =
108333333 Hz` on the PYNQ-Z1 with the PYNQ U-Boot clock setup (confirmed from the
Linux clock tree: `swdt = 108333333`, `WDT_CLK_SEL = 0`). The binding has a
required `clock-frequency` property, and `pynq_z1.dts` sets it next to the other
fixed board clocks (`arch_timer`, `uart0`).

### D5. DT node, binding, MMU

`dts/bindings/watchdog/cdns,wdt-r1p2.yaml` (compatible as in Linux, requires `reg`,
`interrupts`, `clock-frequency`). The node `watchdog0: watchdog@f8005000` is inside
`soc {}` of `zynq7000.dtsi` (after `uart1`, away from the hunks of 0003/0004),
`status = "disabled"`. `pynq_z1.dts` enables it and adds the `watchdog0` alias used
by the Zephyr sample. `soc.c` maps the register page when the node is enabled
(hunk between the `mpcore` and `ocm` regions).

### D6. AMP build leaves the watchdog to Linux

In the Linux+Zephyr AMP configuration Linux owns the SWDT (`cdns-wdt`), so
`amp/zephyr_cpu1/boards/pynq_z1.overlay` disables `watchdog0`, like UART0 and GEM0.

### D7. Initial state and Kconfig

Init writes `ZMR` with the access key and no enable bit, so the timer is stopped
whatever the boot loader left behind. `WDT_CDNS` is `default y` with
`depends on DT_HAS_CDNS_WDT_R1P2_ENABLED`, in `drivers/watchdog/Kconfig.cdns`
(sourced and built in the sorted blocks).

### D8. Test application and reset cause

`src/wdt_test.c` adds the `wdt` shell command: `start <ms> [reset|irq]`, `feed`,
`stop`, `run <ms> <seconds>` (feed at `ms/4`), `hang` (stop feeding, print the time
since the last feed every 20 ms) and `cause [clear]`. `cause` reads the SLCR
`REBOOT_STATUS` register, which is persistent through all resets except power-on
and where the ROM sets bit 16 for an SWDT reset (bit 19 for SLC soft reset, 18 for
APU watchdog 1). Clearing bits 23:16 before the test makes the cause unambiguous.
`prj.conf` enables `CONFIG_WATCHDOG=y`.

## Consequences

**Positive**

- A real hardware watchdog for standalone Zephyr: expiry resets the whole PS and
  the board boots again from the boot loader.
- The stock `samples/drivers/watchdog` builds and runs for `-b pynq_z1`.

**Negative / trade-offs**

- The clock rate is a devicetree constant. A different PS clock configuration needs
  a changed `clock-frequency` (and `SLCR.WDT_CLK_SEL` is not touched).
- ~~No `hwinfo` reset-cause support yet~~ Resolved by ADR 0006 (`hwinfo reset_cause`; `wdt cause` was removed).
- `WDT_OPT_PAUSE_*` options are ignored (the SWDT keeps running while halted).
- The patch stack (0001-0005) must be rebased on every Zephyr bump.

## Verification (live PYNQ-Z1, standalone SMP, netboot)

Reset-on-timeout, measured with `wdt start <ms>` followed by `wdt hang`. The "last
print" is the last `hang: +N ms` line before the U-Boot banner, printed every 20 ms,
so the real expiry is within 20 ms after it. `REBOOT_STATUS` was cleared before each
run and read after the reboot (`0x00010000` = SWDT reset only).

| Timeout | Last print | Deviation | `REBOOT_STATUS` after reset |
|---------|-----------|-----------|-----------------------------|
| 100 ms | 95 ms | -5 % | `0x00010000` |
| 500 ms | 496 ms | -1 % | `0x00010000` |
| 1000 ms | 999 ms | 0 % | `0x00010000` |
| 5000 ms | 5000 ms | 0 % | `0x00010000` |
| 10000 ms | 10004 ms | 0 % | `0x00010000` |
| 60000 ms | 59993 ms | 0 % | `0x00010000` |

After each reset the board went through the boot ROM, U-Boot and the TFTP netboot
and returned to the Zephyr shell without any manual intervention.

Other checks:

- Feeding: `wdt run 1000 5` and `wdt run 1000 3` (feed every 250 ms) finished and
  disabled the watchdog without a reset.
- Soak: `wdt run 2000 600` fed a 2000 ms watchdog every 500 ms for 10 minutes; the
  uptime stayed continuous (128 s to 728 s) and `REBOOT_STATUS` stayed `0` (no reset).
- Interrupt mode: `wdt start 1000 irq` called the callback once per second (it
  feeds the watchdog), `wdt stop` ended it.
- Stock `samples/drivers/watchdog` built for `pynq_z1` without warnings and ran
  on the board: "Feeding watchdog..." five times, then "Waiting for reset..." and a
  hardware reset (the sample re-runs after each reboot).
- `amp/build_amp.sh` still builds (the watchdog is disabled in that build), and
  patches 0001-0005 apply cleanly to a fresh `v4.5.0` tree.

## Open items / follow-ups

- ~~`hwinfo` reset-cause support~~ Done in ADR 0006.
- A `sys_reboot()` implementation for the SoC (`kernel reboot cold` currently ends
  in "Failed to reboot: spinning endlessly"), possibly through `SLCR.PSS_RST_CTRL`.
- Derive the clock rate from the real PS clock configuration instead of a DT constant.

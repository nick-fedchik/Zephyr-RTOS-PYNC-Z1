# ADR 0002: Zynq-7000 XADC sensor driver (`zynq7000_xadc`)

- **Status:** Accepted (implemented, verified on hardware; see "Open items")
- **Issue:** [#5 `[P2][Easy]` Zynq-7000 XADC driver](https://github.com/nick-fedchik/Zephyr-RTOS-PYNC-Z1/issues/5)
- **Zephyr version:** `v4.5.0`
- **Platform:** Digilent PYNQ-Z1 (Xilinx Zynq-7000 `XC7Z020`)
- **Delivered as:** `patches/0004-drivers-sensor-add-zynq7000-xadc.patch`
- **References:** Xilinx UG585 (chapter 30, XADC interface), UG480 (7 Series XADC),
  Linux `7.3.6` `drivers/iio/adc/xilinx-xadc-core.c`

## Context

The Zynq-7000 PS reaches the on-chip XADC through the XADC interface (XADCIF) at
`0xF8007100`, which sits in the DevCfg register page. Zephyr `v4.5.0` has no
driver for it. The goal is to read the die temperature and the internal supply
monitors from standalone Zephyr and to compare them with Linux `xilinx-xadc` on
the same board.

Constraints are the same as in ADR 0001: no Vivado, all upstream changes as
numbered patches with hunks disjoint from the earlier ones, nothing
lab-specific in the repository.

## Decisions

### D1. Sensor API, not ADC API

The driver is a `sensor` driver (`drivers/sensor/xlnx/zynq7000_xadc.c`,
`DT_DRV_COMPAT xlnx_zynq_xadc_1_00_a`). The issue asks for `SENSOR_CHAN_DIE_TEMP`
and a `sensor`-style verification, and `SENSOR_CHAN_DIE_TEMP` is the natural API
for the temperature. The supply rails use driver-specific channels defined in
`include/zephyr/drivers/sensor/zynq7000_xadc.h` (`SENSOR_CHAN_PRIV_START` based):
`SENSOR_CHAN_XADC_VCCINT`, `_VCCAUX`, `_VCCBRAM`, `_VCCPINT`, `_VCCPAUX`,
`_VCCO_DDR`. Values are returned in degrees Celsius and volts.

*Alternatives:* the ADC API (generic, but the temperature then needs an
application-side conversion and the API gives no unit semantics); both APIs from
one node (a device can only expose one API struct).

### D2. Compatible, binding and DT node follow Linux

Compatible `xlnx,zynq-xadc-1.00.a` (same as the Linux binding), node
`xadc@f8007100` with `reg = <0xf8007100 0x20>` and `interrupts = <GIC_SPI 7 ...>`,
`status = "disabled"` in `zynq7000.dtsi`, enabled in `pynq_z1.dts`. The node is
inside `soc {}` and, to keep patch hunks disjoint from 0003, it is inserted before
`sdhc0` rather than next to `devcfg`. A `syscon = <&slcr>` phandle is required
(see D4).

### D3. Polled FIFO protocol, mutex-protected

A register read is: drain the data FIFO, push `READ(reg)` and `NOP` into the
command FIFO, wait until the data FIFO holds two words (the first answers the
`READ` slot, the second carries the value), take the second. The wait is a
`k_busy_wait()` loop with a 5 ms timeout (`-ETIMEDOUT`). All interrupts are
masked in `INTMSK`. `sample_fetch` reads all seven channels under a mutex and
`channel_get` converts the latched values, so a snapshot is consistent.

Interrupts (`GIC_SPI 7`) are not used in v1, same policy as ADR 0001 D2.

### D4. Initialisation and clocking

Init resets the interface (`MCTL.RESET` pulse), clears and masks interrupts and
sets `CFG = ENABLE | REDGE | WEDGE | TCKRATE_DIV16 | IGAP(20)`. The XADCIF is
clocked by the PCAP clock, so the driver makes sure `SLCR.PCAP_CLK_CTRL.CLKACT` is
set through the `syscon` API (U-Boot/FSBL normally leave it on; Linux sets it too).

The TCK divider is fixed at /16 (at most 12.5 MHz with a 200 MHz PCAP clock,
limit 50 MHz). Linux derives the divider from `clk_get_rate()`; Zephyr has no
clock handle for the PCAP clock yet, and the sample rate is irrelevant for
monitoring, so the slowest divider is the robust choice.

### D5. The XADC runs in its default sequencer mode

The driver does not change the sequencer configuration. In the default mode the
XADC continuously converts temperature, VCCINT, VCCAUX, VCCBRAM, and on Zynq
also VCCPINT, VCCPAUX and VCCO_DDR, which is exactly the set the driver exposes.
VAUX/VP-VN external inputs (PYNQ-Z1 Arduino analog pins) would need the
sequencer and input-mode registers and are left to a follow-up.

### D6. Conversion

Using the 12-bit result (`reg >> 4`):

- temperature: `T[C] = raw * 503.975 / 4096 - 273.15`
- supply rails: `V = raw * 3.0 / 4096`

These are the same transfer functions as in Linux (`temp_scale = 503975`,
offset, `3000 / 2^12`). Calculation is done in 64-bit micro-units with
`sensor_value_from_micro()`.

### D7. MMU mapping

The XADCIF page (`0xF8007000`) is already mapped when `devcfg` is enabled
(ADR 0001). When only `xadc` is enabled, `soc.c` maps it itself. The block is
guarded by `xadc` enabled and `devcfg` not enabled, so the same page is never
mapped twice. The hunk is appended after the existing
`DT_FOREACH_STATUS_OKAY(xlnx_xps_gpio_1_00_a, ...)` line, away from the hunks of
0002 and 0003 (ADR 0001 D9).

### D8. Kconfig / CMake placement

New vendor directory `drivers/sensor/xlnx/` with its own `CMakeLists.txt` and
`Kconfig`, registered in `drivers/sensor/{CMakeLists.txt,Kconfig}` in the sorted
blocks (`zephyr-keep-sorted`). `ZYNQ7000_XADC` is `default y` with
`depends on DT_HAS_XLNX_ZYNQ_XADC_1_00_A_ENABLED && SYSCON` (`depends on`, not
`select`, per ADR 0001 D8).

### D9. Test application

`src/xadc_test.c` adds `xadc read`, which fetches a sample and prints all seven
channels. `prj.conf` enables `CONFIG_SENSOR=y`.

## Consequences

**Positive**

- Die temperature and six supply rails readable from Zephyr with the standard
  `sensor_sample_fetch()` / `sensor_channel_get()` calls.
- Works alone or together with the DevCfg driver (tested with the PL loaded).

**Negative / trade-offs**

- Driver-specific channel IDs are not understood by generic tooling such as the
  `sensor` shell (it knows only the common channels); the app shell command is
  used instead.
- No alarms/thresholds, no triggers, no external (VAUX) inputs, no calibration
  settings.
- Reads busy-wait for the FIFO transfer (not measured; bounded by a 5 ms timeout per register).
- The patch stack (0001-0004) must be rebased on every Zephyr bump.

## Verification (live PYNQ-Z1, standalone SMP, netboot)

Linux reference: mainline `7.3.6` `xilinx-xadc` on the same board, PL configured by
U-Boot with `base.bin`, read through sysfs. Linux values are
`(raw + offset) * scale`.

| Quantity | Zephyr `xadc read` | Linux `xilinx-xadc` | Delta |
|----------|-------------------|---------------------|-------|
| Die temperature, PL active, settled (about 150-210 s uptime) | 59.18 C | 59.4-59.9 C (raw 2698-2706) | under 1 C |
| Die temperature, PL not configured, about 25 s after boot | 53.3-53.6 C | not comparable (PL configured in the Linux run) | - |
| VCCINT | 1.021 V | 1.018-1.019 V (raw 1386-1392) | about 2 mV |
| VCCAUX | 1.7996 V | 1.799 V (raw 2455-2458) | under 1 mV |
| VCCBRAM / VCCPINT / VCCPAUX | 1.021 / 1.020 / 1.799 V | not sampled on Linux | - |
| VCCO_DDR | 1.4956 V | 1.484-1.495 V (raw 2022-2041) | about 10 mV (noise) |

Notes:

- The temperature depends strongly on the system state (PL configured or not,
  uptime). A first comparison at about 50 s uptime differed by 1.6 C (Zephyr
  58.9 C vs Linux 57.3 C) while both were still warming up; after soaking for
  the same time the difference was under 1 C. Comparisons must therefore be done
  with the same PL state and soak time.
- Zephyr results were stable across consecutive reads (temperature changes by
  tenths of a degree, rails by one LSB of 0.73 mV).
- `amp/build_amp.sh` still builds, and patches 0001-0004 apply cleanly to a fresh
  `v4.5.0` tree.

## Open items / follow-ups

- ~~Interrupt-driven reads and alarm/over-temperature thresholds.~~ Done, see
  [ADR 0007](0007-zynq7000-xadc-aux-inputs-and-alarms.md).
- ~~External analog inputs (VAUX, VP/VN; PYNQ-Z1 Arduino A0-A5) through the
  sequencer and input-mode registers.~~ Done, see ADR 0007.
- ~~Derive the TCK divider from the real PCAP clock rate.~~ Done, see ADR 0007 (D6).
- Teach the generic `sensor` shell about the driver-specific channels or move
  to chan_spec/decoder support.

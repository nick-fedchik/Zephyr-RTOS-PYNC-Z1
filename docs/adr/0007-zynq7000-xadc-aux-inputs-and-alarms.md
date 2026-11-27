# ADR 0007: Zynq-7000 XADC external inputs, alarms and interrupts

- **Status:** Accepted (implemented, verified on hardware; see "Open items")
- **Issue:** [#10 XADC external analog inputs (VAUX / Arduino A0-A5), alarms and interrupts](https://github.com/nick-fedchik/Zephyr-RTOS-PYNC-Z1/issues/10)
- **Zephyr version:** `v4.5.0`
- **Platform:** Digilent PYNQ-Z1 (Xilinx Zynq-7000 `XC7Z020`)
- **Delivered as:** `patches/0009-drivers-sensor-zynq7000-xadc-external-inputs-and-alarms.patch`
  (a delta on top of `0004`)
- **References:** Xilinx UG585 (chapter 30, XADC interface), UG480 (7 Series XADC),
  Digilent PYNQ-Z1 reference manual, Linux `7.3.6`
  `drivers/iio/adc/xilinx-xadc-core.c`, `xilinx-xadc-events.c`, `xilinx-xadc.h`

## Context

ADR 0002 delivered a polled driver for the default XADC sequence (die temperature and
six supply rails). It left out the external analog inputs (VAUX0-15, VP/VN), the
alarm and over-temperature thresholds, interrupts, and it used a fixed, conservative
TCK divider. Issue #10 asks for these, with the PYNQ-Z1 Arduino analog pins A0-A5 as
the practical test target.

## Decisions

### D1. Delivered as a delta patch, `setup_and_build.sh` applies the stack

The driver files are created by patch `0004`, so the extension is a separate patch
`0009` that modifies them. A delta keeps `0004` untouched and the history readable.
The old rule "a patch that reverse-checks is already applied" breaks for such a
stack: once `0009` is applied, `0004` no longer reverse-checks. `setup_and_build.sh`
now finds the topmost patch that reverse-checks and applies only the later ones. All
nine patches also apply, in order, to a fresh `v4.5.0` tree.

### D2. External inputs are selected in the devicetree

`xlnx,channels` lists the enabled inputs (0-15 = VAUX, 16 = VP/VN) and
`xlnx,bipolar-channels` the ones that are bipolar (they must be a subset; both are
checked by `BUILD_ASSERT`). The driver builds the sequencer from them:

- SEQ0 always holds calibration, the six supplies and the die temperature, plus VP/VN
  when enabled; SEQ1 holds the VAUX bits.
- If both VAUX halves (0-7 and 8-15) are used, simultaneous sampling is selected, as
  Linux does (ADC B converts VAUXn+8 together with ADC A converting VAUXn).
  Otherwise ADC B is powered down.
- If `xlnx,channels` is absent the sequencer is not touched, so the ADR 0002 behaviour
  is unchanged.

The board DTS enables `<1 9 6 15 5 13>` (Arduino A0-A5, see D8).

### D3. Sensor API: channels, thresholds, triggers

- New channels `SENSOR_CHAN_XADC_VPVN`, `SENSOR_CHAN_XADC_VAUX(n)` (n = 0..15) in
  `<zephyr/drivers/sensor/zynq7000_xadc.h>`. `sensor_channel_get()` returns volts at
  the XADC pin: `raw12 * 1 V / 4096`, sign-extended for bipolar inputs. This is the
  Linux transfer function for the auxiliary inputs.
- `sensor_attr_set()/get()` with `SENSOR_ATTR_UPPER_THRESH` and
  `SENSOR_ATTR_LOWER_THRESH` for `SENSOR_CHAN_DIE_TEMP` and the six supply channels.
- `sensor_trigger_set()` with `SENSOR_TRIG_THRESHOLD`: a handler arms the alarm, NULL
  disarms it. Re-setting the trigger of an armed alarm only replaces the handler.

### D4. The die-temperature alarm is the over-temperature (OT) alarm

As in Linux, the `DIE_TEMP` thresholds map to the OT alarm (registers `0x53`/`0x57`
with the low nibble `0x3`, otherwise the hardware falls back to 125 C). The lower
threshold is the OT hysteresis: the alarm releases below it. The standard
temperature alarm (ALM0) is unused. The supply channels use their own alarm; a
supply alarm asserts above the upper or below the lower threshold.

### D5. Interrupt design: mask while active, re-arm from a timer

The XADC alarms are level signals. Leaving them unmasked would storm the CPU while
the condition lasts, so:

1. The ISR (SPI 7) reads `INTSTS`, masks the active alarms, clears `INTSTS`, queues
   the event and schedules `unmask_work` after 500 ms. The `XADCIF` alarm bit order
   differs from the 7-series one (OT is bit 7), converted by a helper.
2. Handlers run from the system work queue, not in interrupt context.
3. `unmask_work` reads `MSTS[7:0]`, unmasks the alarms that are no longer active and
   reschedules itself while any alarm is still masked.

Result: exactly one event per excursion, and the alarm re-arms by itself.
`trigger_set()` masks the alarm while it switches the hardware, waits 2 ms (several
sequencer passes) so the alarm status is fresh, and only then unmasks it. It may
sleep and must be called from thread context.

### D6. TCK divider from the real PCAP clock

The driver reads `PCAP_CLK_CTRL` (source, divisor) and the PLL feedback divider from
the SLCR and multiplies by `ps-clk-frequency` (50 MHz on the PYNQ-Z1, default
33.33 MHz) to get the PCAP rate. The TCK divider follows the Linux rule (TCK at most
50 MHz); the sample rate is limited to 150 kSPS through the `CONF2` divider. The
PYNQ-Z1 result is PCAP 200 MHz and DIV4 (50 MHz). If the clock cannot be derived,
the slowest TCK (DIV16) is used.

### D7. The PL configuration resets the XADC registers

Configuring the PL reloads the XADC DRP registers (sequencer, thresholds, alarm
enables) from the bitstream, so everything the driver programmed is lost. Before the
PL is configured, the threshold registers read as 0. The new
`zynq7000_xadc_reconfigure()` re-runs the hardware setup and leaves every alarm
disarmed. The `pl load` shell command calls it after a successful `fpga_load()`.
Applications that load a bitstream must call it and then re-arm their triggers.

### D8. The Arduino analog inputs need a configured PL

The VAUX pads of the PYNQ-Z1 Arduino analog pins are only usable once the PL is
configured. With an unconfigured PL every input floats and reads a small
sequence-dependent ramp (3-70 mV), whatever is connected to the pin. After
`pl load` the inputs are clean. The pin to channel mapping was confirmed on the
board by touching 3V3 to each pin:

| Pin | A0 | A1 | A2 | A3 | A4 | A5 |
|-----|----|----|----|----|----|----|
| XADC input | VAUX1 | VAUX9 | VAUX6 | VAUX15 | VAUX5 | VAUX13 |

The board scales 0-3.3 V to 0-1 V. `xadc ain` prints the pin voltage and a board
voltage using the nominal factor 3.3; the real divider tolerance is not calibrated.

## Consequences

Positive:

- External inputs, alarms and interrupts follow the Linux register-level behaviour
  while using the standard sensor API.
- One event per excursion without an interrupt storm.
- The ADR 0002 internal-only configuration is unchanged.

Negative:

- The XADC settings are lost when the PL is configured and must be restored with
  `zynq7000_xadc_reconfigure()`.
- `sensor_trigger_set()` sleeps for 2 ms and cannot be used from interrupt context.
- Alarm handlers run in the system work queue with up to 500 ms of re-arm latency.
- The Arduino analog pins need a PL bitstream (for example `base.bin`).

## Verification

Hardware: PYNQ-Z1, standalone Zephyr over TFTP, `PL_BITSTREAM=base.bin`
(preloaded to `0x05000000`, programmed with `pl load 0x05000000 4045564`).

| Check | Result |
|-------|--------|
| Fresh `v4.5.0` plus patches `0001`-`0009` | all apply in order |
| `./setup_and_build.sh`, `./amp/build_amp.sh` | both build |
| Regression, `xadc read` | die temp 43-57 C, VCCINT 1.013 V, VCCAUX 1.79 V, VCCBRAM 1.014 V, VCCPINT 1.014 V, VCCPAUX 1.79 V, VCCO_DDR 1.488 V (as ADR 0002) |
| TCK divider (`CFG` register) | `0x80003114`: TCKRATE = 1 (PCAP/4 = 50 MHz) |
| 3V3 on A0, GND on A1, A2-A5 open, PL configured | A0 0.997 V (board about 3.29 V), A1 1.5 mV, A2-A5 1.5-2.9 mV |
| 3V3 touched on each pin | A0 0.9954 V (VAUX1), A1 0.9886 V (VAUX9), A2 0.9888 V (VAUX6), A3 0.9917 V (VAUX15), A4 0.9905 V (VAUX5), A5 0.9897 V (VAUX13) |
| PL not configured | all inputs float (3-70 mV ramp), see D8 |
| `xadc alarm ot 40 30` at about 55 C | one `XADC ALARM DIE_TEMP` event, no storm while the alarm stays active |
| `xadc alarm ot 70 60` afterwards | no event, alarm clears and the interrupt is unmasked again |
| `xadc alarm ot 40 30` again | exactly one more event |
| `xadc alarm vcc int 900 800` (VCCINT 1.01 V) | one `XADC ALARM VCCINT` event |
| `xadc alarm vcc aux 1500 1400` (VCCAUX 1.80 V) while the OT alarm is active | one `XADC ALARM VCCAUX` event |
| `xadc alarm off` | alarms disarmed, no further events |
| `pl load` | `XADC reconfigured: 0 (alarms disarmed)`, aux inputs readable |

The Linux comparison required by the issue was not run: Linux cannot be booted on
the board while the Linux+AMP netboot restore is postponed. The driver uses the
Linux transfer function (`raw12 * 1000 mV / 4096`, 3 V full scale for the supplies),
and the 3V3 reference above is the independent check.

Two bugs were found and fixed during the tests: re-arming an active alarm reported
it again, and a status left over from the previous thresholds raised a false event.

## Open items / follow-ups

- Not tested on hardware: bipolar inputs and VP/VN (no signal wired).
- Calibrate the board divider ratio with a measured reference.
- Compare the readings with Linux `xilinx-xadc` once the Linux netboot is restored.
- Cache thresholds and arm state inside the driver so that
  `zynq7000_xadc_reconfigure()` can restore them.

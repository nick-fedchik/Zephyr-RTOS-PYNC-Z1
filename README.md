# Zephyr-RTOS-PYNC-Z1

Zephyr RTOS (`v4.5.0`) on a **Digilent PYNQ-Z1** (Xilinx Zynq-7000 `XC7Z020`,
dual Cortex-A9 + Artix-7 FPGA) without installing AMD Vivado or Vitis.

Verified on hardware:
- **Native out-of-tree HWMv2 board definition** (`boards/digilent/pynq_z1/`):
  UART0 console (`MIO14`/`MIO15`, `LVCMOS33`, 100 MHz), 325 MHz ARM global
  timer (`arch_timer`), 511 MiB DDR3 (`sram0` at `0x00100000`), and 1G Ethernet
  (`gem0`, `phyaddr 0`, `rgmii-id`).
- **Networking & diagnostics**: DHCPv4 client, `net` shell, `zperf` TCP/UDP
  benchmark server, and optional two-stage **TFTP netboot** (`boot.scr` ->
  `netboot.scr` -> `zephyr.bin`) with automatic SD fallback.
- **Dual-core Cortex-A9 SMP (`CONFIG_SMP=y`, 2 CPUs)**: SCU + `ACTLR.SMP|FW`
  coherency, `pm_cpu_on()` via `0xFFFFFFF0 + sev`, and `smp info` / `smp test`
  shell commands.
- **AMP / OpenAMP (`amp/`)**: **Linux** (stock PYNQ `6.6.10` or custom
  Mainline `7.3.6 PREEMPT_RT`) on CPU0 + **Zephyr 4.5.0** on CPU1
  (`0x1e000000`, `VBAR`, `CONFIG_GIC_SAFE_CONFIG=y`) with `remoteproc0/trace0`
  logging, bidirectional **RPMsg** (`/dev/rpmsg0` over GICv1 SGI 14/15,
  ~100–140 µs RTT), FPGA PL AXI GPIO control, 1 kHz sub-microsecond jitter
  (`< 1 µs`), and independent 1 kHz `FAILSAFE` supervisor.
- **Minimal Mainline Linux `7.3.6` (`linux/`)**: `SMP PREEMPT_RT` `zImage`
  (`5.3 MiB`, all required drivers built-in including `zynq-fpga`, `rpmsg`,
  `XADC`, `Cadence Watchdog`, `USB Host/Serial`, `OF_OVERLAY`, and ported
  `zynq_remoteproc.c`).
- **Embedded Linux `initramfs` (`buildroot/`)**: Buildroot `2026.08.2` minimal
  `rootfs.cpio.uboot` (`3.37 MiB`, `14 MiB` RAM used, `5.58 s` full boot to
  Dropbear SSH + Zephyr CPU1 + `/dev/rpmsg0`, `cyclictest` 1 kHz max `39 µs`,
  native C `/usr/bin/rpmsg-cli`).
- **Upstream Zephyr `4.5.0` patches (`patches/`)**: reproducible fix for a
  GICv1/v2 inter-core SGI drop bug in `isr_wrapper.S` (upstream `#122600` /
  `PR #123040`, documented in
  [`docs/zephyr-gic-v1-v2-sgi-bug.md`](docs/zephyr-gic-v1-v2-sgi-bug.md)) and
  Zynq-7000 SMP/AMP SoC support (`0002`) and a Zynq-7000 DevCfg FPGA Manager
  driver that programs the PL from Zephyr through PCAP DMA (`0003`, see
  [`docs/adr/0001-zynq7000-devcfg-fpga-manager.md`](docs/adr/0001-zynq7000-devcfg-fpga-manager.md)),
  and a Zynq-7000 XADC sensor driver for die temperature and supply rails
  (`0004`, see [`docs/adr/0002-zynq7000-xadc-sensor.md`](docs/adr/0002-zynq7000-xadc-sensor.md)),
  and a Cadence PS watchdog driver for the Zynq-7000 SWDT (`0005`, see
  [`docs/adr/0003-cadence-ps-watchdog.md`](docs/adr/0003-cadence-ps-watchdog.md)),
  and a Zynq-7000 PS QSPI controller driver that exposes the on-board NOR flash
  through the standard `flash` API (`0006`, see
  [`docs/adr/0004-zynq7000-qspi-nor-flash.md`](docs/adr/0004-zynq7000-qspi-nor-flash.md)),
  and an interrupt-driven ARM PL330 PS DMA driver for memory-to-memory transfers
  (`0007`, see [`docs/adr/0005-zynq7000-ps-dma-pl330.md`](docs/adr/0005-zynq7000-ps-dma-pl330.md)),
  and a `hwinfo` reset-cause driver plus `sys_reboot()` through the SLCR
  (`0008`, see [`docs/adr/0006-zynq7000-hwinfo-reset-cause-and-reboot.md`](docs/adr/0006-zynq7000-hwinfo-reset-cause-and-reboot.md)),
  and an extension of the XADC driver with the external analog inputs
  (Arduino A0-A5), threshold alarms and interrupts (`0009`, see
  [`docs/adr/0007-zynq7000-xadc-aux-inputs-and-alarms.md`](docs/adr/0007-zynq7000-xadc-aux-inputs-and-alarms.md)).

> **Scope note:** This repository does not synthesize an FPGA bitstream or
> `BOOT.BIN`. Instead, it reuses the verified `BOOT.BIN` (FSBL + U-Boot) from
> the official PYNQ-Z1 SD image for DDR3 and MIO initialization.

## Repository layout

- `boards/digilent/pynq_z1/` — native Zephyr HWMv2 board definition
- `src/` — main app (`main.c`), SMP bring-up & shell (`zynq7000_smp.c`), and
  single-core CPU1 bare-metal wake-up/SGI test (`cpu1_cmd.c`, `cpu1_trampoline.S`)
- `amp/` — Linux DT overlay (`openamp.dts`, including `&gem0` `phy-handle` fix),
  Zephyr CPU1 AMP/OpenAMP firmware (`amp/zephyr_cpu1/`), and `amp/build_amp.sh`
- `linux/` — minimal Mainline Linux `7.3.6` `PREEMPT_RT` config
  (`pynq_z1_min_defconfig`), ported `zynq_remoteproc.c`, and `build_linux.sh`
- `buildroot/` — Buildroot `2026.08.2` `initramfs` config (`pynq_z1_defconfig`),
  C `/dev/rpmsg0` client (`rpmsg-cli.c`), `S90zephyr-amp` autostart init script,
  and `build_buildroot.sh`
- `patches/` — patches auto-applied to `zephyr-ws/zephyr` by `setup_and_build.sh`
- `docs/` — technical memos (including `docs/zephyr-gic-v1-v2-sgi-bug.md`) and
  architecture decision records (`docs/adr/`)
- `setup_and_build.sh` — self-contained local workspace, SDK `1.0.1`, build,
  and boot-script staging
- `ROADMAP.md` — project milestones and verification status

## Quick start (automated build)

Install Debian/Ubuntu prerequisites:

```sh
sudo apt install -y git cmake ninja-build gperf ccache device-tree-compiler \
	wget xz-utils unzip u-boot-tools mtools python3-venv picocom
```

Run the self-contained setup and build (keeps `.venv/`, `.tools/`, `zephyr-ws/`,
`build/`, and `sd_card_files/` inside the repository):

```sh
./setup_and_build.sh
```

By default, `setup_and_build.sh` generates a **standalone SD-card `boot.scr`**
with no hardcoded network addresses. Useful options:

```sh
# Enable two-stage TFTP netboot (U-Boot uses DHCP unless BOARD_IP is set):
./setup_and_build.sh --tftp-server 192.168.1.10

# Reuse a local PYNQ-Z1 image archive or skip downloading BOOT.BIN:
./setup_and_build.sh --pynq-image /path/to/pynq_z1_v3.1.1.zip
./setup_and_build.sh --skip-pynq-image

# Full list of options and environment overrides:
./setup_and_build.sh --help
```

Persistent machine-specific overrides (`TFTP_SERVER_IP`, `BOARD_IP`,
`BOARD_NETMASK`, `BOARD_GW`, `BOARD_MAC`, `TFTP_PREFIX`, `TFTP_DIR`) can be
placed in a git-ignored `local.conf` file in the repository root.

> Note: `boards/digilent/pynq_z1/pynq_z1.dts` sets a default locally
> administered `local-mac-address` on `&gem0` because PYNQ-Z1 has no MAC EEPROM;
> adjust it if you run multiple boards on the same LAN.

## Flashing & booting standalone Zephyr (2-core SMP)

1. Copy `BOOT.BIN`, `boot.scr`, and `zephyr.bin` from `sd_card_files/` to the
   root of a FAT32 microSD card (keep a backup of your original PYNQ card).
2. Set `JP4` to **`SD`**, connect micro-USB to **`PROG UART`**, and open the
   UART0 console (`115200 8N1`, typically `/dev/ttyUSB1`):
   ```sh
   sudo picocom -b 115200 /dev/ttyUSB1
   ```
3. Power on the board. `boot.scr` loads `zephyr.bin` to `LOAD_ADDR`
   (`0x00100000`), disables caches (`dcache off; icache off`), and jumps to the
   ELF `ENTRY_ADDR` via `go` (`bootm` / `uImage` is not needed).
4. In the Zephyr shell (`uart:~$`), try `smp info`, `smp test`, `net iface`, or
   `zperf tcp download 5001`.

## Loading the FPGA bitstream from Zephyr (DevCfg / PCAP)

Zephyr can program the PL itself with the generic `fpga` API. The driver
accepts a raw, byte-swapped `.bin` bitstream (starts with `66 55 99 aa`) that
is already in DDR. For netboot, let U-Boot preload it over TFTP (the file must
be in `TFTP_DIR/TFTP_PREFIX`; lab settings stay in `local.conf`):

```sh
PL_BITSTREAM=base.bin ./setup_and_build.sh   # optional: PL_BITSTREAM_ADDR=0x05000000
```

After `SRST`, U-Boot prints the exact command to run in the Zephyr shell:

```
uart:~$ pl status                           # INACTIVE
uart:~$ pl load 0x05000000 4045564          # programs the PL in ~50 ms
uart:~$ pl status                           # ACTIVE (PL DONE)
uart:~$ pl led 5                            # LD0-LD3 via PL AXI GPIO
```

`fpga load devcfg@f8007000 <addr> <size>`, `fpga get_status` and `fpga reset`
work as well. The staging window (`CONFIG_ZYNQ7000_FPGA_STAGING_ADDR/SIZE`) is
MMU-mapped by the SoC code; use addresses inside it.

## Reading die temperature and supply voltages (XADC)

The XADC sensor driver is enabled for the `pynq_z1` board. In the Zephyr shell:

```
uart:~$ xadc read
DIE_TEMP  59.183123 C
VCCINT    1.020996 V
VCCAUX    1.799560 V
...
```

Applications use `sensor_sample_fetch()` / `sensor_channel_get()` with
`SENSOR_CHAN_DIE_TEMP` and the `SENSOR_CHAN_XADC_*` channels from
`<zephyr/drivers/sensor/zynq7000_xadc.h>`.

## Hardware watchdog (Cadence PS SWDT)

The watchdog driver is enabled for the standalone `pynq_z1` build (it is
disabled in the AMP build, where Linux owns the SWDT). Applications use the
standard `wdt_install_timeout()`, `wdt_setup()` and `wdt_feed()` calls with
`WDT_FLAG_RESET_SOC` (reset the PS on expiry) or `WDT_FLAG_RESET_NONE` (callback
from the interrupt). The stock `samples/drivers/watchdog` builds for this board.
Shell test commands:

```
uart:~$ wdt run 2000 60      # feed a 2 s watchdog for 60 s, no reset
uart:~$ wdt start 5000       # arm a 5 s watchdog
uart:~$ wdt hang             # stop feeding; the board resets after ~5 s
uart:~$ wdt irqs             # number of watchdog interrupts seen
```

The reset cause is read with the generic `hwinfo` shell and is also printed at
boot (`Reset cause: 0x10 WATCHDOG`); see "Reset cause and reboot" below.

## Reset cause and reboot

The standalone build reports why the SoC was last reset through the generic
`hwinfo` API (`CONFIG_HWINFO_ZYNQ7000`, SLCR `REBOOT_STATUS`) and implements
`sys_reboot()` with the SLCR `PSS_RST_CTRL` soft reset. `main()` prints and clears
the cause at every boot, because the flags are sticky except across power-on reset:

```
Reset cause: 0x08 POR          # after a power cycle
Reset cause: 0x02 SOFTWARE     # after `kernel reboot cold`
Reset cause: 0x10 WATCHDOG     # after `wdt start 500` + `wdt hang`
uart:~$ hwinfo reset_cause get
uart:~$ hwinfo reset_cause supported
```

After `kernel reboot cold` the board goes through BootROM and U-Boot again, so the
TFTP netboot starts normally.

## XADC external inputs and alarms

The XADC driver also reads the six Arduino analog pins and raises threshold
alarms through the standard `sensor` API. `xlnx,channels` in the board DTS selects
the inputs: A0-A5 are VAUX1, 9, 6, 15, 5 and 13 (unipolar, 0-3.3 V on the pin is
scaled to 0-1 V at the XADC). The inputs only work after the PL is configured,
for example with `PL_BITSTREAM=base.bin ./setup_and_build.sh` and `pl load`.
Never apply more than 3.3 V.

```
uart:~$ pl load 0x05000000 4045564
uart:~$ xadc ain                              # A0-A5: pin volts and board mV
uart:~$ xadc aux                              # every enabled VAUX channel
uart:~$ xadc alarm ot 70 60                   # over-temperature upper/lower (C)
uart:~$ xadc alarm vcc int 1100 900           # int|aux|bram|pint|paux|ddr, mV
uart:~$ xadc alarm status                     # thresholds, event counts, XADCIF regs
uart:~$ xadc alarm off
```

An alarm that stays active is reported once and re-arms after it clears. Loading a
bitstream resets the XADC registers, so `pl load` calls
`zynq7000_xadc_reconfigure()` and disarms all alarms; arm them again afterwards.

## QSPI NOR flash

The standalone `pynq_z1` build drives the on-board 16 MiB Winbond `W25Q128JV`
through the Zynq-7000 PS QSPI controller (polled, single-lane I/O mode) and the
generic `jedec,spi-nor` driver, so the standard `flash` shell commands work. The
flash holds a boot image, so the board devicetree splits it into a read-only
`qspi-data` partition (first 15 MiB) and a 1 MiB `qspi-scratch` partition at
`0xF00000`: erase and write only inside the scratch partition.

```
uart:~$ qspi id                       # JEDEC ID, geometry, scratch partition
uart:~$ qspi selftest 128             # erase/program/verify 128 KiB of scratch
uart:~$ flash partitions
uart:~$ flash read qspi_flash 0xf00000 40
uart:~$ flash write qspi_flash 0xf00000 0x11223344
uart:~$ flash erase qspi_flash 0xf00000 0x10000
```

The QSPI node is disabled in the AMP build.

## PS DMA (PL330)

The standalone `pynq_z1` build includes an interrupt-driven driver for the PS
DMA controller (`dmac_s`, `0xF8003000`) that implements the Zephyr `dma` API for
memory-to-memory transfers: 8 channels, up to 4 chained blocks per channel, any
length and alignment, completion callback, `dma_stop()` and `dma_get_status()`.
The driver cleans and invalidates the L1 data cache around each transfer (the L2
cache is off on this board). Peripheral DMA, scatter/gather and cyclic mode are not
implemented.

```
uart:~$ dma info
uart:~$ dma selftest 1024     # 1 MiB copy, byte-exact check, comparison with memcpy
uart:~$ dma align             # unaligned source/destination/length sweep
uart:~$ dma multi             # all 8 channels at once
uart:~$ dma chain             # three chained blocks
uart:~$ dma fill              # fixed source address
uart:~$ dma stop              # abort mid-transfer, then reuse the channel
uart:~$ dma neg               # invalid configurations are rejected
```

The DMA node is disabled in the AMP build (Linux owns the controller there). The
stock Zephyr `tests/drivers/dma/loop_transfer` suite passes with
`zephyr,user { dma-test-devs = <&dmac_s>; };` in an overlay.

## Building AMP / OpenAMP (PYNQ Linux on CPU0 + Zephyr on CPU1)

After running `./setup_and_build.sh` once (to initialize `zephyr-ws/` with
`libmetal` and `open-amp` and apply `patches/`), build the AMP artifacts:

```sh
./amp/build_amp.sh
```

This produces `amp/build/openamp.dtbo` (plus merged `amp/build/pynq-amp.dtb`
if `sd_backup/image.ub` is present) and `amp/build/zephyr_amp.elf`. Boot stock
PYNQ `v3.1` Linux (`6.6.10-xilinx-v2024.1`, which already ships with
`zynq_remoteproc`, `virtio_rpmsg_bus`, and `rpmsg_char`) with `pynq-amp.dtb`,
then on the board:

```sh
sudo cp /tmp/zephyr_amp.elf /lib/firmware/zephyr_amp.elf
sudo modprobe zynq_remoteproc virtio_rpmsg_bus rpmsg_ctrl rpmsg_char
echo zephyr_amp.elf | sudo tee /sys/class/remoteproc/remoteproc0/firmware
echo start | sudo tee /sys/class/remoteproc/remoteproc0/state
sudo cat /sys/kernel/debug/remoteproc/remoteproc0/trace0
ls -la /dev/rpmsg0
```

## Building minimal Mainline Linux 7.3.6 (`PREEMPT_RT`)

Requires `gcc-arm-linux-gnueabihf` (`sudo apt install -y gcc-arm-linux-gnueabihf`).
Downloads Mainline Linux `7.3.6` into git-ignored `.tools/linux/`, installs
`linux/pynq_z1_min_defconfig` and the ported `linux/zynq_remoteproc.c`, and
compiles `zImage` (~5.3 MiB, all drivers built-in):

```sh
./linux/build_linux.sh
# Optional: stage zImage-7.3.6 to a TFTP directory (or set TFTP_DIR in local.conf)
./linux/build_linux.sh --tftp-dir /srv/tftp/pynq1
```

## Building minimal Embedded Linux `initramfs` (Buildroot)

Downloads Buildroot `2026.08.2` into git-ignored `.tools/buildroot/` (using a
pre-built Bootlin `armv7-eabihf` external toolchain), cross-compiles
`buildroot/rpmsg-cli.c`, bundles `amp/build/zephyr_amp.elf` + `S90zephyr-amp`
+ your `~/.ssh/id_*.pub` key (if present), and builds `rootfs.cpio.uboot`
(~3.37 MiB):

```sh
./buildroot/build_buildroot.sh
# Optional: stage rootfs.cpio.uboot to a TFTP directory
./buildroot/build_buildroot.sh --tftp-dir /srv/tftp/pynq1
```

Booting `base.bin` (`fpga load 0`) + `zImage-7.3.6` + `pynq-amp.dtb` +
`rootfs.cpio.uboot` via U-Boot `bootz` brings up the complete Linux `7.3.6 RT`
+ Dropbear SSH + Zephyr `4.5.0` CPU1 + `/dev/rpmsg0` stack in ~5.6 seconds
using ~14 MiB of RAM.

## Manual Zephyr build

From an initialized Zephyr `v4.5.0` workspace with Zephyr SDK `1.0.1` and
`patches/` applied:

```sh
west build -b pynq_z1 /path/to/Zephyr-RTOS-PYNC-Z1 -d /path/to/build -- \
	-DBOARD_ROOT=/path/to/Zephyr-RTOS-PYNC-Z1
```

## License

GPL-3.0 (see [`LICENSE`](LICENSE)); Zephyr board/source files carry `SPDX-License-Identifier: Apache-2.0`.

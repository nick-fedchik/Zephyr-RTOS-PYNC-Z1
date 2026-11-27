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
  [`docs/adr/0001-zynq7000-devcfg-fpga-manager.md`](docs/adr/0001-zynq7000-devcfg-fpga-manager.md)).

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

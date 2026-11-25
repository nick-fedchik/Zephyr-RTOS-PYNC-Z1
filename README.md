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
- **AMP / OpenAMP (`amp/`)**: stock **PYNQ Linux 6.6.10** on CPU0 + **Zephyr
  v4.5.0** on CPU1 (`0x1e000000`, `VBAR`, `CONFIG_GIC_SAFE_CONFIG=y`) with
  `remoteproc0/trace0` logging and bidirectional **RPMsg** (`/dev/rpmsg0` over
  GICv1 SGI 14/15, ~140 µs RTT).
- **Upstream Zephyr `v4.5.0` patches (`patches/`)**: reproducible fix for a
  GICv1/v2 inter-core SGI drop bug in `isr_wrapper.S` (documented in
  [`docs/zephyr-gic-v1-v2-sgi-bug.md`](docs/zephyr-gic-v1-v2-sgi-bug.md)) and
  Zynq-7000 SMP/AMP SoC support.

> **Scope note:** This repository does not synthesize an FPGA bitstream or
> `BOOT.BIN`. Instead, it reuses the verified `BOOT.BIN` (FSBL + U-Boot) from
> the official PYNQ-Z1 SD image for DDR3 and MIO initialization.

## Repository layout

- `boards/digilent/pynq_z1/` — native Zephyr HWMv2 board definition
- `src/` — main app (`main.c`), SMP bring-up & shell (`zynq7000_smp.c`), and
  single-core CPU1 bare-metal wake-up/SGI test (`cpu1_cmd.c`, `cpu1_trampoline.S`)
- `amp/` — Linux DT overlay (`openamp.dts`), Zephyr CPU1 AMP/OpenAMP firmware
  (`amp/zephyr_cpu1/`), and build script (`amp/build_amp.sh`)
- `patches/` — patches auto-applied to `zephyr-ws/zephyr` by `setup_and_build.sh`
- `docs/` — technical memos (including `docs/zephyr-gic-v1-v2-sgi-bug.md`)
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

## Manual Zephyr build

From an initialized Zephyr `v4.5.0` workspace with Zephyr SDK `1.0.1` and
`patches/` applied:

```sh
west build -b pynq_z1 /path/to/Zephyr-RTOS-PYNC-Z1 -d /path/to/build -- \
	-DBOARD_ROOT=/path/to/Zephyr-RTOS-PYNC-Z1
```

## License

GPL-3.0 (see [`LICENSE`](LICENSE)); Zephyr board/source files carry `SPDX-License-Identifier: Apache-2.0`.

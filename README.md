# ZephyrOS-PYNC-Z1

Zephyr RTOS (`v4.5.0`) bring-up on a Digilent PYNQ-Z1 (Xilinx Zynq-7000 `XC7Z020`)
without installing Vivado or Vitis. It provides a native out-of-tree HWMv2 board
definition (`boards/digilent/pynq_z1/`) with UART0 (`MIO14`/`MIO15`), the
325 MHz ARM global timer, and 1G Ethernet (`gem0`) with a fixed locally
administered MAC address and DHCPv4 (`hostname = "pynq1"`).

**This does not synthesize an FPGA bitstream or `BOOT.BIN`.** Instead, it reuses
the verified `BOOT.BIN` (FSBL + U-Boot) from the official PYNQ-Z1 SD image for
DDR3 and MIO initialization.

## Automated local setup and build

`./setup_and_build.sh` sets up a self-contained Zephyr workspace (`.venv/`,
`.tools/`, `zephyr-ws/`), installs the minimal Zephyr SDK `1.0.1`
(`arm-zephyr-eabi`), builds `zephyr.bin` for the native `pynq_z1` board
(`-DBOARD_ROOT=...`), extracts `BOOT.BIN` from the official PYNQ-Z1 image,
and stages `sd_card_files/` with `boot.scr` (using `LOAD_ADDR` for `fatload`
and `ENTRY_ADDR` for `go`):

```sh
./setup_and_build.sh
# or reuse a local PYNQ-Z1 archive/image, or skip image download:
./setup_and_build.sh --pynq-image /path/to/pynq_z1_v3.1.1.zip
./setup_and_build.sh --skip-pynq-image
```

## Build the Zephyr image manually

Install the Zephyr SDK with the ARM toolchain and initialize a Zephyr workspace
following the [Zephyr getting-started guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html).
From that workspace, build this application:

```sh
west build -b pynq_z1 /path/to/ZephyrOS-PYNC-Z1 -d /path/to/build -- \
	-DBOARD_ROOT=/path/to/ZephyrOS-PYNC-Z1
```

The raw image is `/path/to/build/zephyr/zephyr.bin`. The board definition
(`boards/digilent/pynq_z1/`) configures UART0 (`MIO14`/`MIO15`, `LVCMOS33`,
100 MHz clock) via `pinctrl_uart0_default`, sets `&arch_timer` to 325 MHz,
reserves the low 1 MiB of DDR3 (`sram0` at `0x00100000`), and enables `&gem0`
with `local-mac-address = [9e 5c 1a 8a bf 53]`.

## Reuse a PYNQ boot image

Download the official PYNQ-Z1 image for the board and mount its boot partition
read-only. For example, `losetup` can expose the image partitions:

```sh
loopdev=$(sudo losetup --find --show --read-only --partscan PYNQ-Z1.img)
lsblk "$loopdev"
sudo mount -o ro "${loopdev}p1" /mnt
```

Copy the image's `BOOT.BIN` (and any separate U-Boot payload required by that
image) to a FAT32 SD card. This reuses the board's known-good first-stage
initialization instead of substituting ZedBoard/Zybo DDR settings. Do not
assume every PYNQ release has the same partition layout or boot files; inspect
the mounted image first. Detach the loop device after unmounting:

```sh
sudo umount /mnt
sudo losetup --detach "$loopdev"
```

The alternative of copying `ps7_init_gpl.c` from PYNQ into
`board/xilinx/zynq/custom_hw_platform/` is not a verified upstream U-Boot
procedure. That directory is not present in the current Xilinx U-Boot tree,
and PYNQ v3.0.1's `boards/Pynq-Z1/base` contains a Vivado Tcl design rather
than a ready-to-copy `ps7_init_gpl.c`. Do not rely on that recipe without
finding a specific maintained patch or build configuration.

## Boot handoff

Once the reused PYNQ image has initialized the board and reached U-Boot, copy
`BOOT.BIN`, `zephyr.bin`, and the generated `boot.scr` from `sd_card_files/` to
the FAT32 boot partition. Because `zephyr.bin` is a flat binary linked at
`LOAD_ADDR` (`0x00100000`) while the ELF entry point (`ENTRY_ADDR`) may sit at
an offset within the image, `boot.scr` loads to `LOAD_ADDR` and jumps to
`ENTRY_ADDR`:

```text
fatload mmc 0:1 <LOAD_ADDR> zephyr.bin
dcache off
icache off
go <ENTRY_ADDR>
```

Use `go` for the raw Zephyr binary. `bootm` is the Linux boot path and a Linux
`uImage` wrapper is not required.

## Boot image and FPGA

No PYNQ-Z1 `BOOT.BIN` is included or generated here. Bootgen packages boot
partitions; it does not generate the board-specific DDR initialization needed
by the first-stage loader. The open-source [Bootgen](https://github.com/Xilinx/bootgen)
README currently requires OpenSSL 3.5 or newer to build it.

The CPU-only serial demo does not need an FPGA bitstream. Loading a `.bit`
file into the PL is a separate, board- and U-Boot-configuration-specific step.

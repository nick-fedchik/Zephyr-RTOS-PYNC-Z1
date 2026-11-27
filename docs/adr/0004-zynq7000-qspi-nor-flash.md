# ADR 0004: Zynq-7000 PS QSPI controller and NOR flash

- **Status:** Accepted (implemented, verified on hardware; see "Open items")
- **Issue:** [#4 `[P4][Medium]` Zynq-7000 PS QSPI NOR Flash driver](https://github.com/nick-fedchik/Zephyr-RTOS-PYNC-Z1/issues/4)
- **Zephyr version:** `v4.5.0`
- **Platform:** Digilent PYNQ-Z1 (Xilinx Zynq-7000 `XC7Z020`), 16 MiB Winbond QSPI NOR
- **Delivered as:** `patches/0006-drivers-spi-add-zynq7000-qspi.patch`
- **References:** Xilinx UG585 (chapter 12, Quad-SPI Flash Controller), Linux `7.3.6`
  `drivers/spi/spi-zynq-qspi.c`

## Context

The Zynq-7000 PS has a Quad-SPI controller (`xlnx,zynq-qspi-1.0`, `0xE000D000`)
that talks to the 16 MiB serial NOR flash on the PYNQ-Z1. Zephyr `v4.5.0` has no
driver for it. The existing QSPI/SPI drivers target other register layouts
(`spi_xlnx_zynqmp_gqspi`, `flash_cadence_qspi_nor`, `spi_xlnx_axi_quadspi`, the
Cadence `spi_cdns` used for the PS SPI0/1 blocks).

The board boots from the SD card (`JP4 = SD`), but the QSPI flash is not empty: a
read of offset 0 shows a Zynq boot image header (`66 55 99 aa` at `0x20`, `XNLX`
at `0x24`). The flash content must therefore be treated as valuable.

Constraints are the same as in ADR 0001-0003: no Vivado, upstream changes as
numbered patches with disjoint hunks, nothing lab-specific in the repository.

## Decisions

### D1. A SPI controller driver plus the generic `jedec,spi-nor` driver

Two architectures were considered:

1. A custom `flash` driver that talks to the controller directly (like
   `flash_cadence_qspi_nor`).
2. A Zephyr `spi` controller driver for the QSPI block and the existing generic
   `spi_nor` flash driver on top.

Option 2 is used. `spi_nor` already implements JEDEC ID/SFDP discovery, the
erase/page-program/read command set, write-enable and busy polling, page layout
and the `flash` API used by the `flash` shell, `flash_area`, MCUboot and file
systems. The controller driver only has to move bytes. The flash description lives
in the devicetree (`compatible = "jedec,spi-nor"`), so a different chip needs no
code change.

### D2. I/O mode, polled, single lane

`drivers/spi/spi_xlnx_zynq_qspi.c` (`DT_DRV_COMPAT xlnx_zynq_qspi_1_0`) uses the
controller's I/O (FIFO) transfer mode, as the mainline Linux driver does, and
polls the status register (no interrupts, accepted for the first version). The
Linear Addressing (memory-mapped) mode that the boot ROM and U-Boot may have left
enabled is switched off at init (`LQSPI_CFG = 0`). The wire protocol is 1-1-1 only
(`SPI_LINES_SINGLE`); dual/quad lines are a follow-up. Requests for peripheral
mode, LSB first, loopback, active-high CS, GPIO CS, word sizes other than 8 bits
or chip select other than 0 return `-ENOTSUP`.

### D3. Transfer algorithm

A `transceive` call asserts CS (manual chip select), then streams
`max(tx_len, rx_len)` bytes. Every byte clocked out also clocks one byte in, so TX
and RX are both consumed in lock-step through the `spi_context` helpers (skip and
NULL buffers as used by `spi_nor` are handled by the context).

- Full words: up to 32 words (128 bytes, below the 63-word FIFO depth) are written to
  `TXD0` (4 bytes), then the same number of words is read from `RXD`.
- A tail of 1-3 bytes uses `TXD1/2/3` (1/2/3 bytes). Those registers must only be
  used with an empty TX FIFO, which holds because every burst is fully drained
  first. The received bytes of a short word sit in the upper bytes of the RX word.
- The RX threshold is set to 1 so `RX_NOT_EMPTY` means "at least one word" (the
  Linux driver leaves it at 32 because it is interrupt driven); the TX threshold is 1.
- All waits use `WAIT_FOR()` with a 100 ms timeout and return `-ETIMEDOUT`.
- The controller is configured as in Linux: master, manual CS, 32-bit FIFO width,
  flash memory interface mode (`IFMODE`), auto start.
- CS is released at the end of the transfer unless `SPI_HOLD_ON_CS` is set (and in
  `release()`).

### D4. Clock rate comes from the devicetree

As for the watchdog (ADR 0003), Zephyr has no clock controller for the Zynq PS.
The binding requires `clock-frequency` (the LQSPI reference clock). The PYNQ
U-Boot setup programs `SLCR.LQSPI_CLK_CTRL (0xF800014C) = 0x501`, which is
`IO PLL 1000 MHz / 5 = 200 MHz`; the board DTS sets 200 MHz and `qspi id` prints
the live SLCR value next to the DT value so a mismatch is visible. The SPI clock
is `ref / (2 << n)` for `n = 0..7`, the smallest divider not above
`spi-max-frequency`. The board uses `spi-max-frequency = 50 MHz` (divider 4), well
within the Winbond limit for fast read. The clock gate (`APER_CLK_CTRL.LQSPI_CPU_1XCLKACT`)
and the MIO1-6 pin multiplexing are left as the boot firmware set them, the same
approach as the UART.

### D5. Flash description and partitions

`pynq_z1.dts` enables `&qspi` and adds `qspi_flash: flash@0` (`jedec,spi-nor`,
`jedec-id = [ef 70 18]`, 128 Mbit). The chip is a Winbond `W25Q128JV` (`EF 70 18`,
read from the board; the issue text assumed a Spansion part). With
`CONFIG_SPI_NOR_SFDP_RUNTIME=y` the geometry comes from the chip's SFDP tables
(`spi_nor` reports 64 KiB erase sectors, 256-byte pages).

Two fixed partitions protect the content:

| Partition | Offset | Size | Purpose |
|-----------|--------|------|---------|
| `qspi-data` (`read-only`) | `0x000000` | 15 MiB | Left untouched (boot image) |
| `qspi-scratch` | `0xF00000` | 1 MiB | Only region the tests erase or program |

The `flash` shell addresses the raw device, so the protection is a convention
for tests, enforced in the `qspi selftest` command (it refuses to leave the scratch
partition) and visible in the `flash partitions` output.

### D6. DT node, binding, MMU

`dts/bindings/spi/xlnx,zynq-qspi-1.0.yaml` (compatible as in Linux). The node
`qspi: spi@e000d000` is inside `soc {}` of `zynq7000.dtsi` (before `psgpio`, away
from the hunks of earlier patches), `status = "disabled"`, interrupt `GIC_SPI 19`
(unused while polling). `soc.c` maps the register page when the node is enabled;
the entry is the first element of `mmu_regions[]` because every other spot is
already covered by the context of patches 0002-0004.

### D7. AMP build

The QSPI node is disabled in `amp/zephyr_cpu1/boards/pynq_z1.overlay`. Linux in the
AMP image has no MTD/QSPI support, but the controller stays out of the AMP firmware
so that Linux can own it later.

### D8. Test application

`src/qspi_test.c` adds the `qspi` shell command:

- `qspi id`: JEDEC ID, flash size, erase sector size, SLCR clock check, scratch
  partition.
- `qspi selftest [kib]` (default 64, max 256): erase, check blank, program a
  pseudo-random pattern, read back and compare, check 90 short reads at odd
  offsets and lengths (1..10 bytes, offsets 0..8), print timings, erase again.

`prj.conf` enables `CONFIG_FLASH`, `CONFIG_FLASH_SHELL`, `CONFIG_SPI`,
`CONFIG_SPI_NOR`, `CONFIG_SPI_NOR_SFDP_RUNTIME`, `CONFIG_FLASH_PAGE_LAYOUT` and
`CONFIG_FLASH_JESD216_API`.

## Consequences

**Positive**

- Standard Zephyr `flash` API and shell on the PYNQ-Z1 QSPI flash, usable by
  `flash_area`, MCUboot or a file system later.
- The generic `spi_nor` driver means no flash command logic is duplicated.

**Negative / trade-offs**

- Polled, single-lane I/O mode: reads reach about 3.5-5.8 MiB/s and programming about
  0.45 MiB/s, below the capability of the flash (quad, linear mode).
- The reference clock is a devicetree constant.
- Only chip select 0 (one flash) is supported.
- The patch stack (0001-0006) must be rebased on every Zephyr bump.

## Verification (live PYNQ-Z1, standalone SMP, netboot)

- Device init: `qspi` and `qspi_flash` are `READY` in `device list`.
- JEDEC ID: `qspi id` prints `ef 70 18`, 16777216 bytes, 64 KiB erase sectors and
  `LQSPI_CLK_CTRL 0x00000501` -> 200000000 Hz (DT 200000000 Hz).
- `flash read qspi_flash 0 40` reads the boot image header (`66 55 99 aa 58 4e 4c 58`
  at `0x20`), unchanged after all tests.
- `flash erase`, `flash write` and `flash read` on the scratch partition: a write
  that crosses a 256-byte page boundary (`0xf000f8`) reads back correctly and the
  erase returns the sector to `0xff`. `flash test qspi_flash 0xf00000 0x1000 20`
  (erase, write, verify) passed.
- `qspi selftest`: 1, 64, 100 and 256 KiB pass; ten consecutive runs of 128 KiB pass
  (erase 501 ms, program 256-308 ms, read 22-36 ms, i.e. 0.41-0.49 MiB/s program and
  3.5-5.7 MiB/s read).
- `amp/build_amp.sh` still builds (the node is disabled there) and patches
  0001-0006 apply cleanly to a fresh `v4.5.0` tree.

## Open items / follow-ups

- Interrupt-driven transfers and dual/quad (1-1-4) reads using the controller's
  linear read mode or `READ_1_1_4`.
- Hybrid-sector layouts and 4 KiB erase (`spi_nor` currently reports 64 KiB pages).
- Second chip select / dual-memory mode.
- Derive the reference clock from the real PS clock configuration.
- Boot the PS from QSPI (`JP4 = QSPI`) with a Zephyr image in the flash.

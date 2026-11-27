# ADR 0008: Loading the PL bitstream from a FAT microSD card

- **Status:** Accepted (implemented, verified on hardware; see "Open items")
- **Issue:** [#7 Load the PL bitstream (`base.bin`) from microSD FAT or QSPI flash directly in Zephyr](https://github.com/nick-fedchik/Zephyr-RTOS-PYNC-Z1/issues/7)
- **Zephyr version:** `v4.5.0`
- **Platform:** Digilent PYNQ-Z1 (Xilinx Zynq-7000 `XC7Z020`)
- **Delivered as:** `patches/0010-drivers-sdhc-xlnx-zynq7000-sdhci-support.patch`
  (fixes to the stock `drivers/sdhc/xlnx_sdhc.c` plus an MMU region), together with
  application, board and `prj.conf` changes
- **References:** Xilinx UG585 (chapter 13, SD/SDIO controller; chapter 25, SLCR),
  SD Host Controller Simplified Specification 3.0, Linux `7.3.6`
  `drivers/mmc/host/sdhci.c` and `sdhci-of-arasan.c`, ADR 0001 (DevCfg FPGA Manager)

## Context

ADR 0001 programs the PL from a bitstream that already sits in DDR; for netboot, U-Boot
preloads it over TFTP. Issue #7 asks Zephyr to fetch `base.bin` by itself from the
microSD card (FAT) so that no TFTP preload or Linux is needed.

Zephyr `v4.5.0` ships an SDHC driver, `drivers/sdhc/xlnx_sdhc.c` (compatible
`xlnx,versal-8.9a`), written for the Versal SD/eMMC controller. The Zynq-7000 `sdhc0`
node (`0xE0100000`, SPI 24) in `zynq7000.dtsi` is disabled and this driver was never
run on a Zynq. The Zynq-7000 controller is a plain SDHCI 2.0/3.0 core, so most of the
driver applies, but bring-up on the board exposed six differences, listed in D2.

## Decisions

### D1. Use the Zephyr SDHC + disk-access + FatFs stack, mounted on demand

`sdhc0` is enabled with a `zephyr,sdmmc-disk` child node (`disk-name = "SD"`), and the
application mounts `FATFS` at `/SD:` through `fs_mount()`. The new shell command

```
pl loadfile [path]        # default /SD:/base.bin
```

mounts the card, validates the file, reads it into the existing staging window
(`CONFIG_ZYNQ7000_FPGA_STAGING_ADDR`, 0x05000000, 16 MiB) in 256 KiB chunks, unmounts
the card and then calls the same `pl_program()` helper as `pl load` (`fpga_load()` plus
the XADC re-configuration from ADR 0007). Reusing the staging window keeps the
cache-maintenance and MMU assumptions of ADR 0001 unchanged.

Mounting and unmounting per load keeps the card idle between loads and means a card can
be swapped without reboot. The FAT volume is mounted **read-only**
(`CONFIG_FS_FATFS_READ_ONLY=y`, `CONFIG_FS_FATFS_MOUNT_MKFS=n`): a bootloader-style
command must never write to or format the PYNQ boot card.

### D2. Fixes to the stock driver (patch 0010)

| # | Symptom on Zynq-7000 | Cause | Fix |
|---|----------------------|-------|-----|
| 1 | `No response from card` on CMD0/CMD8 although the card answers | Interrupt enable registers at `0x34`/`0x38` stayed 0: **16-bit stores to the paired normal/error status, status-enable and signal-enable registers are dropped** on this core (32-bit stores work, as in Linux) | `XLNX_SDHC_WRITE_INTR()`: always write the normal (low) and error (high) half-words as one 32-bit access, in the host reset, the polled paths, the clear-all helper and the ISR |
| 2 | `Card does not support CMD8`, ACMD41 never completes | Capabilities `[31:30]` report an *embedded* slot, so the driver rejects R7 and APP_CMD as for eMMC | Force `slot_type = SD` when `CONFIG_SOC_FAMILY_XILINX_ZYNQ7000` |
| 3 | `Data transfer timeout` on the first data command (ACMD51) | Driver programs 64-bit ADMA2 (`host_ctrl1 = 0x18`, 12-byte descriptors); the core has no 64-bit system bus support (capabilities bit 28 = 0) | Use 32-bit ADMA2 (`host_ctrl1 = 0x10`, 8-byte descriptors) on Zynq-7000 |
| 4 | Data abort right after `Setting bus clock to 50000000` | `set_timing` writes `itap_dly`/`otap_dly`, which belong to the Versal SD wrapper and lie about 60 KiB beyond the controller window | Skip the tap-delay programming on Zynq-7000 |
| 5 | Data abort on the very first register access | The flat MMU map has no entry for `0xE0100000`, and `DEVICE_MMIO_MAP` is a no-op there | `soc.c` maps the `sdhc0` register page (strongly ordered) when the node is `okay` |
| 6 | Clock divisor wrong (200 MHz assumed) | `zynq7000.dtsi` gives the SDHCI reference clock as 200 MHz; the boot firmware actually sets `SDIO_CLK_CTRL` = `0x1401` (IO PLL 1000 MHz / 20) | `pynq_z1.dts` overrides `&sdhci_ref_clk { clock-frequency = <DT_FREQ_M(50)>; }` |

All changes in the driver are guarded by `CONFIG_SOC_FAMILY_XILINX_ZYNQ7000` (or
`IS_ENABLED()` for the tap delays so the helpers stay referenced), so Versal behaviour
is unchanged. Items 1-4 are bugs or assumptions of the upstream driver on a non-Versal
core and could be upstreamed with a proper DT/capability based selection.

### D3. MIO mux, card-detect and SDIO clock are inherited from the boot firmware

The Zephyr driver has no pinctrl support. `sdio0_2_grp` (MIO 40-45), the card-detect/write
-protect routing and the SDIO reference clock are configured by the FSBL in the stock
PYNQ `BOOT.BIN`, which this project always reuses. Zephyr does not touch SLCR for them.
This matches the project scope note (no Vivado/Vitis; `BOOT.BIN` provides MIO and DDR).
It is why the SDIO clock must be described in the DTS (D2, item 6) rather than set.

### D4. Validate the file before touching the PL

`pl loadfile` refuses, with a distinct message and without modifying the PL: a card that
cannot be mounted, a missing file (`-ENOENT`), a directory, a file of 0 bytes, a file
larger than `CONFIG_ZYNQ7000_FPGA_STAGING_SIZE`, and a short read. The PL is configured
only after the complete file has been read.

### D5. `fatfs` joins the west project filter

The FatFs library lives in the `fatfs` west module. `setup_and_build.sh` adds `+fatfs`
to `manifest.project-filter` (`-.*,+cmsis,+picolibc,+libmetal,+open-amp,+fatfs`), so a
fresh checkout fetches it. No other modules are needed.

### D6. AMP firmware keeps `sdhc0` disabled

The CPU1 AMP image (`amp/zephyr_cpu1`) disables `&sdhc0` in its overlay: Linux owns the
card there and the image does not need a file system.

### D7. QSPI flash source is deferred

The issue lists microSD *or* QSPI. The QSPI driver exists (ADR 0004), but a bitstream
stored in flash needs a layout/partitioning decision (offset, header, erase handling).
The SD path was chosen as the first, safer source; a `pl loadflash` command can reuse
`pl_program()` later.

## Consequences

**Positive**
- Zephyr programs the PL from `/SD:/base.bin` with no TFTP preload and no Linux.
- Bounded, explicit error handling; the boot card is never written or formatted.
- The upstream SDHC driver gains a working Zynq-7000 path guarded by a SoC Kconfig.

**Negative**
- Read throughput is low: 4,045,564 bytes in about 2.5 s (~1.6 MB/s). The driver runs the
  controller conservatively and uses polling for most waits; adequate for a one-shot
  bitstream load, not for bulk storage.
- The ADMA2 and `itap/otap` changes are Zynq-specific forks of upstream code.
- The FatFs and disk stack add to the RAM image (the image still uses under 1 % of DDR).
- Depends on FSBL pin/clock setup (D3); a different first-stage loader must provide it.

## Verification

Board: PYNQ-Z1, `JP4=SD`, standalone Zephyr via TFTP netboot, `base.bin` (4,045,564 bytes)
copied to the FAT partition of the PYNQ card (boot files untouched), no TFTP preload.

| Check | Result |
|-------|--------|
| Card init | CMD8 SDHC, ACMD41 after 1 attempt, 121,536,512 blocks (58 GiB), SCR spec 8 |
| `pl loadfile` (3 consecutive cold boots) | `Read 4045564 bytes in 2516 ms`, PL programmed in 47.9-51.0 ms, `fpga_get_status()=1` |
| `pl status` | `ACTIVE (PL DONE)` |
| `pl led 10` | LD0-LD3 `DATA=0xa TRI=0xf`, LEDs lit (read back through the PL AXI GPIO) |
| Missing file | `cannot stat /SD:/nope.bin (-2): file not found` |
| 16 MiB + 1 byte file | `must be 1..16777216 (staging window)`, PL untouched |
| Empty file | `/SD:/empty.bin is 0 bytes: must be 1..16777216` |
| Directory | `/SD:/dir.bin is not a regular file` |
| Regression | `xadc read` and `hwinfo reset_cause` unchanged; stock netboot still works |
| Fresh `v4.5.0` | patches `0001`-`0010` apply in order |
| Builds | `./setup_and_build.sh` and `./amp/build_amp.sh` succeed |

## Open items

- No-card and unformatted-card behaviour was not tested on the board (the card stayed
  inserted); the mount error path (`-5`) was observed only during bring-up.
- Throughput tuning: interrupt-driven waits, 4-bit/high-speed negotiation check, larger
  ADMA bursts.
- QSPI source (D7) and a `pl loadfile` that selects among several files.
- Upstream the Zynq-7000 fixes (D2) with a capability/DT based selection.

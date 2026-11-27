# ADR 0001: Zynq-7000 DevCfg FPGA Manager driver (`fpga_zynq7000`)

- **Status:** Accepted (implemented, verified on hardware including visual LED confirmation; see "Open items")
- **Issue:** [#2 `[P1][Easy]` Zynq-7000 DevCfg FPGA Manager](https://github.com/nick-fedchik/Zephyr-RTOS-PYNC-Z1/issues/2)
- **Zephyr version:** `v4.5.0`
- **Platform:** Digilent PYNQ-Z1 (Xilinx Zynq-7000 `XC7Z020`, dual Cortex-A9)
- **Delivered as:** `patches/0003-drivers-fpga-add-zynq7000-devcfg-fpga-manager.patch`
- **Register-level references:** Xilinx UG585 (Zynq-7000 TRM, chapters 6 and 7),
  Linux `7.3.6` `drivers/fpga/zynq-fpga.c`

## Context

Today the PL (Artix-7 fabric) is configured by U-Boot (`fpga load 0`) or by
Linux (`zynq-fpga`) before Zephyr starts. Zephyr `v4.5.0` has no driver for the
Zynq-7000 Device Configuration interface (DevCfg, `0xF8007000`), so a Zephyr
application cannot program or reprogram the PL on its own. The project goal is a
Vivado-free workflow, so Zephyr must be able to load a prebuilt raw `.bin`
bitstream (PYNQ `base.bin`) through the PCAP, using Zephyr's generic
`fpga_driver_api` (`fpga_load()`, `fpga_get_status()`, `fpga_reset()`,
`fpga_get_info()`) and the existing `fpga` shell commands.

Constraints:

- No Vivado/Vitis; only the pinned SDK and `west` workspace under `zephyr-ws/`.
- All upstream Zephyr changes are carried as numbered patches in `patches/` and
  applied by `setup_and_build.sh`.
- The same Zephyr tree is used for standalone SMP (`0x00100000` load address)
  and the Linux+Zephyr AMP build (`0x1e000000`).
- Lab network settings live in git-ignored `local.conf`; nothing lab-specific
  may be committed.

## Decisions

### D1. Driver structure follows upstream conventions

New files (all inside the patch):

| File | Purpose |
|------|---------|
| `drivers/fpga/fpga_zynq7000.c` | Driver (`DT_DRV_COMPAT xlnx_zynq_devcfg_1_0`) |
| `drivers/fpga/Kconfig.zynq7000` | `ZYNQ7000_FPGA` and staging window options |
| `dts/bindings/fpga/xlnx,zynq-devcfg-1.0.yaml` | Binding (`reg`, `interrupts`, `syscon`) |
| `dts/arm/xilinx/zynq7000.dtsi` | `devcfg@f8007000` node, `status = "disabled"` |

The node is placed **inside** `soc {}` (a root-level node has no interrupt
parent). The board enables it with `&devcfg { status = "okay"; };` in
`pynq_z1.dts`. The binding already declares the `interrupts` property
(`GIC_SPI 8`) so interrupt mode can be added later without a DT change.

### D2. Polling, not interrupt-driven, in v1

The load runs from a thread, is mutex-protected, and busy-polls `INT_STS`.
Measured duration for the 4 MiB `base.bin` is about 50 ms, which is acceptable.
This was agreed with the project owner for the first version.

*Alternatives:* IRQ + semaphore (`D_P_DONE` / error interrupts). Better for
latency-sensitive systems but needs more test surface. Tracked as a follow-up.

### D3. SLCR access through `syscon`

The driver needs `FPGA_RST_CTRL` (`0x240`), `LVL_SHFTR_EN` (`0x900`) and
`PCAP_CLK_CTRL` (`0x168`) in the SLCR. It uses the `syscon` API through a
`syscon = <&slcr>` phandle. The SoC code (`soc_reset_hook`) already unlocks the
SLCR at boot, so the driver does not touch `SLCR_UNLOCK`.

*Alternatives:* map the SLCR registers directly in the driver (duplicates the
mapping and breaks the AMP build where patch 0002 already maps it); a dedicated
SLCR driver (out of scope).

### D4. Load sequence (mirrors Linux `zynq-fpga.c`)

1. Validate input: address 8-byte aligned, non-zero size multiple of 4, sync
   word `66 55 99 aa` within the first 128 bytes (byte-swapped `.bin` format;
   `-EINVAL` otherwise).
2. Assert `FPGA_RST_CTRL = 0xf`, `LVL_SHFTR_EN = 0` then `0xa`.
3. Pulse `PCFG_PROG_B` and poll `STATUS.PCFG_INIT` through high, low, high.
4. Select PCAP (`CTRL.PCAP_PR | PCAP_MODE`), require an empty DMA queue, clear
   `MCTRL.PCAP_LPBK`, mask and clear all interrupts.
5. Flush the D-cache for the source buffer, start DMA
   (`DMA_SRC_ADDR | 1`, `DMA_DST_ADDR = 0xffffffff`, `DMA_SRC_LEN = size / 4`,
   `DMA_DEST_LEN = 0`).
6. Poll `INT_STS.D_P_DONE`; abort on any error bit (`0x00f0c860`); then poll
   `PCFG_DONE`.
7. Clear `PCAP_PR`, enable level shifters (`0xf`), release `FPGA_RST_CTRL`.

On any failure the driver returns a negative errno and leaves the PL in reset.

### D5. `get_status` semantics

`FPGA_STATUS_ACTIVE` is reported from the latched `INT_STS.PCFG_DONE` bit (as
Linux does), otherwise `FPGA_STATUS_INACTIVE`. `fpga_reset()` pulses
`PCFG_PROG_B` and therefore returns the status to INACTIVE.
`get_status` is read-only and not mutex-protected; a concurrent `load` may be
observed mid-way as INACTIVE.

### D6. Bitstream staging window in DDR + MMU mapping

Cortex-A in Zephyr maps only the kernel image by default. A DDR buffer that holds
the bitstream (4 MiB for `base.bin`) must be added to `mmu_regions`, otherwise
the first access gives a 1st-level translation fault (observed: Data Abort at
`0x05000000`). Decision:

- `ZYNQ7000_FPGA_STAGING_ADDR` (default `0x05000000`) and
  `ZYNQ7000_FPGA_STAGING_SIZE` (default `0x01000000`, 16 MiB) are Kconfig options.
- `soc.c` maps that window as normal cacheable memory in 1 MiB sections
  (`pl_bitstream`), guarded by `CONFIG_ZYNQ7000_FPGA`.
- The DevCfg registers are mapped (`devcfg`) when the DT node is enabled.
- The driver flushes the data cache before starting the DMA, so any cacheable
  source buffer is correct. The address is not restricted to the window; the
  window is only a convenient, already-mapped default.

### D7. Bitstream delivery: U-Boot preload over TFTP (no microSD changes)

`setup_and_build.sh` gets two options, `PL_BITSTREAM` (file name under
`TFTP_PREFIX`) and `PL_BITSTREAM_ADDR` (default `0x05000000`). When set, the
generated `netboot.scr` runs `tftpboot ${PL_BITSTREAM_ADDR} <prefix>/<file>`
before jumping to Zephyr and prints `pl load <addr> 0x${filesize}`, so the
test is a copy-paste in the shell. Lab IPs stay in `local.conf`.

*Alternatives:* embed the bitstream into the Zephyr image (4 MiB inflation, slower
netboot, complicates licensing/updates); read from SD/FAT in Zephyr (needs the
SDHC driver, issue-sized work of its own).

### D8. Kconfig uses `depends on`, not `select`

`ZYNQ7000_FPGA` has `depends on DT_HAS_XLNX_ZYNQ_DEVCFG_1_0_ENABLED && SYSCON &&
CACHE_MANAGEMENT`. A `select SYSCON` created a Kconfig dependency loop because
the SoC already selects it.

### D9. Patch stacking: keep hunks disjoint

`setup_and_build.sh` decides whether each patch is already applied with a
per-patch `git apply --reverse --check`. Patch 0003 touches the same file as 0002
(`soc/xlnx/zynq7000/common/soc.c`), so its hunks must not overlap 0002's context
or that check fails for the earlier patch. The 0003 hunks were placed after the
`ocm` MMU entry. Rule for future patches: **regenerate each patch against a tree
that has only the preceding patches applied** (we use a scratch `git worktree`),
then verify `git apply --reverse --check` for 0001, 0002 and 0003 on the working
tree.

### D10. Standalone vs AMP in `soc.c`

In the standalone build nothing else maps the SLCR or the PL AXI GPIO; in the AMP
build patch 0002 already does. Following the 0002 convention, 0003 adds the
`slcr` and `pynq_base_axi_gpio` (`0x41200000`, `0x60000`) regions only when
`DT_REG_ADDR(zephyr_sram) == 0x00100000` (standalone).

### D11. Test application (`src/pl_test.c`)

A `pl` shell command group is built only when `CONFIG_ZYNQ7000_FPGA=y`:
`pl load <addr> <size>`, `pl status`, `pl led [0-15]` (AXI GPIO `0x41250000`),
`pl read <addr>` (restricted to `0x41200000..0x4125ffff` so that a typo can never
hit an unconfigured AXI window, which hangs the bus). `prj.conf` enables
`CONFIG_FPGA` and `CONFIG_FPGA_SHELL`.

## Consequences

**Positive**

- Zephyr can (re)program the PL without U-Boot or Linux; works with the generic
  FPGA API and shell.
- The same patch series still applies to a clean tree and is idempotent.
- No lab-specific data in the repository.

**Negative / trade-offs**

- Only raw byte-swapped `.bin` is accepted (no `.bit`, no byte-swapping, no
  compression, no encrypted/authenticated streams).
- 16 MiB of DDR is reserved for staging when the driver is enabled.
- The load blocks the calling thread for the entire PCAP transfer (~50 ms).
- Reconfiguring the PL while AXI peripherals are in use is the application's
  responsibility.
- The upstream patch stack (0001-0003) must be rebased on every Zephyr bump.

## Verification (live PYNQ-Z1, standalone SMP, netboot, `base.bin` 4,045,564 bytes)

| Check | Result |
|-------|--------|
| `pl status` before load | `INACTIVE` |
| `pl load 0x05000000 4045564` | PL programmed in ~51 ms (50976, 50067, 50984 us), `fpga_get_status()=1` |
| `pl status` after load | `ACTIVE (PL DONE)` |
| LED GPIO `0x41250000` (`pl led 3/5/6/10/0xf`) | DATA write and read-back match, `TRI=0xf` |
| `pl led 0xf` | LD0-LD3 physically lit (visual confirmation by the project owner) |
| `pl read 0x41200000` / `0x41250000` | No AXI bus hang |
| Reload without reset, `fpga reset`, then `pl load` | Works; status returns to `INACTIVE` after reset |
| `fpga load devcfg@f8007000 0x05000000 4045564` / `fpga get_status` | Works, status `1` |
| Misaligned address (`0x05000004`) | `-EINVAL` |
| No sync word (zero-filled buffer) | `-EINVAL` |
| Size not multiple of 4 (`4045563`) | `-EINVAL` |

## Open items / follow-ups

- Interrupt-driven mode (D2).
- Partial reconfiguration and encrypted bitstreams.
- Behaviour when the PL was already configured by U-Boot/Linux before Zephyr.
- Read the bitstream from SD/FAT or QSPI (depends on issue #4 and an SDHC driver).
- Rebuild/verify the AMP firmware (`amp/build_amp.sh`) with the patch stack
  (`CONFIG_FPGA` is off there, so no change is expected).

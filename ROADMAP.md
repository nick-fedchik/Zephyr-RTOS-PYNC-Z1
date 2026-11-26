# Roadmap — Zephyr RTOS on Digilent PYNQ-Z1

This roadmap tracks the bring-up and R&D milestones for running Zephyr RTOS (`v4.5.0`) on the Digilent PYNQ-Z1 (Xilinx Zynq-7000 `XC7Z020`) without Vivado/Vitis.

## Phase 1: Core Bring-up (Completed)

- [x] Self-contained local workspace and build script (`setup_and_build.sh`) using Zephyr `v4.5.0`, Zephyr SDK `1.0.1` (`arm-zephyr-eabi`), and minimal `west` module filter (`-.*,+cmsis,+picolibc`).
- [x] No-Vivado boot flow reusing the verified PYNQ `v3.1` `BOOT.BIN` (FSBL + U-Boot) and generating `boot.scr` with separate `LOAD_ADDR` (`0x00100000`) and `ENTRY_ADDR`.
- [x] Serial console bring-up on `uart0` (`MIO14`/`MIO15`, `LVCMOS33`, 100 MHz, 115200 baud) with explicit `pinctrl_uart0_default` and 325 MHz ARM global timer (`arch_timer`).
- [x] Verified boot banner and application output on hardware via `picocom`.

## Phase 2: Networking & Diagnostics (Completed)

- [x] Enable 1G Ethernet (`gem0`, `phyaddr 0`, `rgmii-id`) with a fixed locally administered MAC (`9e:5c:1a:8a:bf:53`), DHCPv4 client, and `net` shell.
- [x] Verify 1000 Mb full-duplex link, DHCPv4 lease acquisition, and ICMP ping (~1.1 ms) on hardware.
- [x] Add network throughput benchmarking (`zperf`), `NET_STATISTICS`, and mDNS responder (`pynq1.local`) to `prj.conf`.
- [x] Verify native `pynq_z1` build (`hostname=pynq1`) and `zperf` TCP benchmark on hardware (`21.4 Mbps` baseline with default 16 KB TCP window).
- [ ] *(Optional)* Tune TCP/GEM buffers (`CONFIG_SPEED_OPTIMIZATIONS`, larger TCP window / `NET_BUF_DATA_SIZE`) and check mDNS multicast RX filtering on `eth_xlnx_gem`.

## Phase 3: Native `pynq_z1` Board Port (In Progress)

- [x] Replace the `zedboard` + overlay workaround with an out-of-tree HWMv2 board definition under `boards/digilent/pynq_z1/` (`board.yml`, `Kconfig.pynq_z1`, `Kconfig.defconfig`, `pynq_z1_defconfig`, `pynq_z1.dts`, `pynq_z1-pinctrl.dtsi`) to build directly via `west build -b pynq_z1`.
- [x] Verify the native `pynq_z1` build on hardware (UART0 + 1G Ethernet/DHCPv4 + `zperf`).
- [ ] Verify additional PS peripherals (GPIO, SDHC0, QSPI, I2C, USB) against the PYNQ-Z1 hardware preset.

## Phase 4: Dual-Core & Heterogeneous Computing (Completed)

- [x] Set up two-stage TFTP netboot (`boot.scr` -> `pynq1/netboot.scr` -> `pynq1/zephyr.bin`) with SD fallback for rapid iteration via `SRST` (`960 KiB/s` verified on hardware).
- [x] Run a minimal CPU1 wake-up test from Zephyr (`0xFFFFFFF0` + `sev` on Zynq-7000): verified on hardware (`magic=0x43505531`, `mpidr=0x80000001`, `core=1`, `CPU1 is ALIVE`, plus cooperative `cpu1 stop` WFE park and resume).
- [x] Verify bi-directional CPU0 ↔ CPU1 IPC via `__nocache` shared memory and GICv1 SGI #7 (`cpu1 ping`: **7 µs** RTT from WFE park, **16 µs** while running), fixing an upstream Zephyr `v4.5.0` GICv1/v2 `GICC_IAR` CPUID-masking bug in `isr_wrapper.S` (`patches/0001-arch-arm-cortex_a_r-mask-gic-v1-v2-intid-in-isr-wrapper.patch`, documented in `docs/zephyr-gic-v1-v2-sgi-bug.md`).
- [x] **AMP / OpenAMP (Linux on CPU0 + real-time Zephyr on CPU1):**
  - [x] Inspect PYNQ `v3.1` Linux FIT (`image.ub`, `Linux 6.6.10-xilinx-v2024.1`): kernel already ships with `CONFIG_REMOTEPROC=y`, `CONFIG_ZYNQ_REMOTEPROC=m`, `CONFIG_RPMSG_VIRTIO=m`, `CONFIG_RPMSG_CHAR=m`, and `CONFIG_RPMSG_CTRL=m`! Stock `fdt-0` lacks `remoteproc`/`reserved-memory`, so AMP only needs a DT overlay (`openamp.dtbo`) — no Linux kernel rebuild required.
  - [x] Reserve dedicated DDR via `reserved-memory` (`0x1e000000`, 16 MiB), relocate Zephyr's vector table via `VBAR` (`0x1e000000`) with MMU mapping (`vbar_vectors`), enable `CONFIG_GIC_SAFE_CONFIG=y` + `ACTLR.SMP|FW`, and verify live **Linux 6.6.10 (CPU0) + Zephyr v4.5.0 (CPU1)** coexistence via `zynq_remoteproc` and `/sys/kernel/debug/remoteproc/remoteproc0/trace0`.
  - [x] Add bidirectional RPMsg channel (`open-amp` + `libmetal` over GICv1 SGI 14/15, endpoint `"rpmsg-raw"` -> Linux `/dev/rpmsg0`): verified live echo round-trip from Python on PYNQ Linux to Zephyr CPU1 at **137–179 µs** RTT!
- [x] **2-core SMP on Cortex-A9 (`CONFIG_SMP=y`, `CONFIG_MP_MAX_NUM_CPUS=2`):** verified on hardware (`smp info` scheduled on CPU1 `MPIDR=0x80000001`; `smp test` ran pinned workers concurrently on CPU0 and CPU1 with parallel wall time matching max core time). Reproducible via `patches/0002-soc-xlnx-zynq7000-enable-smp-coherency-and-ipi.patch` + `src/zynq7000_smp.c` (`pm_cpu_on`) + `CONFIG_FPU=n` workaround for `CONFIG_USE_SWITCH` + `CONFIG_FPU_SHARING` mismatch in `thread.c`/`exc.S`.

## Phase 5: FPGA (PL) & Multi-Board Lab (In Progress)

- [x] Drive FPGA PL peripherals from Zephyr CPU1 without Vivado: map PYNQ `base.bin` AXI GPIO (`0x41200000..0x4125ffff`: `LD0-LD3`, RGB `LD4-LD5`, `BTN0-BTN3`, `SW0-SW1`) and control them from Python on Linux CPU0 over `/dev/rpmsg0` (`led`, `rgb`, `blink`, `read`) at **127–133 µs** RTT.
- [ ] Measure real-time PL interrupt latency/jitter (Linux userspace vs Zephyr CPU1).
- [ ] Multi-architecture Zephyr lab: link STM32 (SPI/I2C/ADC sensing) and ESP32 (Wi-Fi/BLE) with PYNQ-Z1 acting as a 1G Ethernet gateway and real-time processor.
- [x] Publish Part 1 technical article (`DOU/DOU-Zephyr-RTOS-on-PYNQ-Z1-without-Vivado-UA.md`) and draft Part 2 (`AMP/OpenAMP + FPGA PL`).

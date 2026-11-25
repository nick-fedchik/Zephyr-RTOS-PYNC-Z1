# Roadmap — Zephyr RTOS on Digilent PYNQ-Z1

This roadmap tracks the bring-up and R&D milestones for running Zephyr RTOS (`v4.5.0`) on the Digilent PYNQ-Z1 (Xilinx Zynq-7000 `XC7Z020`) without Vivado/Vitis.

## Phase 1: Core Bring-up (Completed)

- [x] Self-contained local workspace and build script (`setup_and_build.sh`) using Zephyr `v4.5.0`, Zephyr SDK `1.0.1` (`arm-zephyr-eabi`), and minimal `west` module filter (`-.*,+cmsis,+picolibc`).
- [x] No-Vivado boot flow reusing the verified PYNQ `v3.1` `BOOT.BIN` (FSBL + U-Boot) and generating `boot.scr` with separate `LOAD_ADDR` (`0x00100000`) and `ENTRY_ADDR`.
- [x] Serial console bring-up on `uart0` (`MIO14`/`MIO15`, `LVCMOS33`, 100 MHz, 115200 baud) with explicit `pinctrl_uart0_default` and 325 MHz ARM global timer (`arch_timer`).
- [x] Verified boot banner and application output on hardware via `picocom`.

## Phase 2: Networking & Diagnostics (In Progress)

- [x] Enable 1G Ethernet (`gem0`, `phyaddr 0`, `rgmii-id`) with a fixed locally administered MAC (`9e:5c:1a:8a:bf:53`), DHCPv4 client, and `net` shell.
- [x] Verify 1000 Mb full-duplex link, DHCPv4 lease acquisition, and ICMP ping (~1.1 ms) on hardware.
- [ ] Flash and verify the rebuilt image with `CONFIG_NET_HOSTNAME="pynq1"` on microSD.
- [ ] Add network throughput benchmarking (`zperf`) and lightweight services (mDNS `pynq1.local`, HTTP/CoAP/MQTT).

## Phase 3: Native `pynq_z1` Board Port (In Progress)

- [x] Replace the `zedboard` + overlay workaround with an out-of-tree HWMv2 board definition under `boards/digilent/pynq_z1/` (`board.yml`, `Kconfig.pynq_z1`, `Kconfig.defconfig`, `pynq_z1_defconfig`, `pynq_z1.dts`, `pynq_z1-pinctrl.dtsi`) to build directly via `west build -b pynq_z1`.
- [ ] Verify the native `pynq_z1` build on hardware (UART0 + 1G Ethernet/DHCPv4).
- [ ] Verify additional PS peripherals (GPIO, SDHC0, QSPI, I2C, USB) against the PYNQ-Z1 hardware preset.

## Phase 4: Dual-Core & Heterogeneous Computing (AMP / SMP)

- [ ] Run a minimal CPU1 wake-up test from Zephyr (`0xFFFFFFF0` + `sev` on Zynq-7000).
- [ ] **AMP / OpenAMP (Linux on CPU0 + real-time Zephyr on CPU1):**
  - [ ] Inspect PYNQ Linux (`image.ub`) for `zynq_remoteproc` and `rpmsg` support.
  - [ ] Reserve dedicated DDR via `reserved-memory` and relocate Zephyr's vector table away from `0x00000000`.
  - [ ] Implement GIC SGI-based IPM/mailbox signaling for Zynq-7000 (`ipm_xlnx_ipi` targets ZynqMP) and run `openamp_rsc_table` (`/dev/ttyRPMSG0`).
- [ ] *(Optional R&D)* Explore SMP on dual Cortex-A9 (`CONFIG_MP_MAX_NUM_CPUS=2`, SCU coherency, and secondary core bring-up).

## Phase 5: FPGA (PL) & Multi-Board Lab

- [ ] Load a prebuilt `.bit` bitstream from U-Boot (`fpga loadb`) before jumping to Zephyr and access AXI peripherals / DMA from a Zephyr driver.
- [ ] Multi-architecture Zephyr lab: link STM32 (SPI/I2C/ADC sensing) and ESP32 (Wi-Fi/BLE) with PYNQ-Z1 acting as a 1G Ethernet gateway and real-time processor.
- [ ] Publish a technical article documenting the bring-up, pitfalls (`pinctrl`, `LOAD_ADDR` vs `ENTRY_ADDR`, SDK `1.0.1`), and AMP architecture.

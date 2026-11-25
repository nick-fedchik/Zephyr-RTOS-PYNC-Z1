# Technical Memo: Upstream Zephyr v4.5.0 GICv1/v2 SGI Drop Bug (`isr_wrapper.S`)

- **Date:** 2026-11-25
- **Zephyr version:** `v4.5.0` (commit `41d10b597304`)
- **Platform verified on:** Digilent PYNQ-Z1 (Xilinx Zynq-7000 `XC7Z020`, Dual Cortex-A9, GICv1)
- **Affected files:**
  - `arch/arm/core/cortex_a_r/isr_wrapper.S` (both `#ifndef CONFIG_USE_SWITCH` and `#else` / `CONFIG_USE_SWITCH` paths)
  - `drivers/interrupt_controller/intc_gic.c` (`arm_gic_get_active()` / `arm_gic_eoi()`)
- **Local fix patch:** `patches/0001-arch-arm-cortex_a_r-mask-gic-v1-v2-intid-in-isr-wrapper.patch`

## Summary

On AArch32 Cortex-A/R systems using GICv1 or GICv2 (`CONFIG_GIC_V1` / `CONFIG_GIC_V2`), any Software Generated Interrupt (SGI, INTID `0..15`) raised by a **non-zero CPU** (e.g., CPU1 → CPU0) is silently dropped as a spurious interrupt by Zephyr's `_isr_wrapper`.

This breaks:
- Inter-core GIC SGI signaling in AMP/OpenAMP setups (e.g., CPU1 bare-metal/RTOS or Linux → CPU0 Zephyr).
- SMP scheduling IPIs (`SGI_SCHED_IPI = 0`) sent from secondary cores (`CPUID >= 1`) to CPU0 on GICv1/v2 platforms.

## Root Cause

Per the ARM GICv1/v2 Architecture Specification (Section 4.4.4 `GICC_IAR` and Section 4.4.5 `GICC_EOIR`):
- Bits `[9:0]` of `GICC_IAR` contain the **Interrupt ID (`INTID`)**.
- Bits `[12:10]` of `GICC_IAR` contain the **source `CPUID`** for SGIs (`0..7`).
- Software **must** write the full unmodified `GICC_IAR` value (including `CPUID`) back to `GICC_EOIR`.

In Zephyr `v4.5.0`:
1. `arm_gic_get_active()` (`drivers/interrupt_controller/intc_gic.c`) correctly returns the full raw `GICC_IAR` register so that `arm_gic_eoi()` can write it back intact.
2. When **CPU1** raises **SGI #7** to **CPU0** (writing `0x00010007` to `GICD_SGIR` at `0xF8F01F00`), CPU0 reads:
   - `CPUID = 1` (`1 << 10 = 0x400`)
   - `INTID = 7` (`0x007`)
   - `GICC_IAR = 0x00000407` (**1031** decimal).
3. In `arch/arm/core/cortex_a_r/isr_wrapper.S` (in **both** the non-`CONFIG_USE_SWITCH` and `CONFIG_USE_SWITCH` implementations), `r0` is pushed onto the stack for `arm_gic_eoi()`, and then compared **unmasked** against `CONFIG_NUM_IRQS` (96 on Zynq-7000):

```asm
	bl arm_gic_get_active
	push {r0, r1}
	...
	mov r1, #CONFIG_NUM_IRQS
	cmp r0, r1
	bge spurious_continue
```

Because `1031 >= 96`, `_isr_wrapper` branches directly to `spurious_continue`, skips `_sw_isr_table[7]`, and still calls `arm_gic_eoi(0x407)`. As a result, CPU0 does not hang — the interrupt is acknowledged and cleared, while the registered Zephyr ISR never executes.

*(Note: SGIs raised by CPU0 to itself have `CPUID = 0`, so `GICC_IAR == INTID` and the bug is masked in single-core self-SGI tests.)*

## Fix

Mask `r0` to the 10-bit `INTID` (`0..1023`) **after** `push {r0, r1}` in both `_isr_wrapper` paths when `CONFIG_GIC_V1` or `CONFIG_GIC_V2` is enabled:

```asm
	push {r0, r1}

#if defined(CONFIG_GIC_V1) || defined(CONFIG_GIC_V2)
	/*
	 * GICC_IAR carries CPUID in bits [12:10] for SGIs from another core.
	 * Keep the full IAR on the stack for arm_gic_eoi(), but mask r0 to
	 * the 10-bit INTID before bounds-checking and _sw_isr_table lookup.
	 */
	ubfx r0, r0, #0, #10
#endif
```

This guarantees:
1. The stack copy passed to `arm_gic_eoi()` retains the full `CPUID | INTID` required by GICv1/v2.
2. `CONFIG_ARM_TRACK_ACTIVE_IRQ`, the `CONFIG_NUM_IRQS` bounds check, and `_sw_isr_table` indexing all receive the clean 10-bit `INTID`.

## Hardware Verification (Digilent PYNQ-Z1)

Reproducer: `src/cpu1_cmd.c` + `src/cpu1_trampoline.S` (`cpu1 start` followed by `cpu1 ping <N>`).

### Before patch

CPU1 processed the shared-memory request (`rsp_seq` matched `req_seq`) and wrote `0x00010007` to `GICD_SGIR` (`0xF8F01F00`), but CPU0's `cpu1_sgi_isr` never fired:

```text
uart:~$ cpu1 ping 42
Timeout waiting for SGI#7 from CPU1 (req_seq=1 rsp_seq=1)
uart:~$ cpu1 ping 123
Timeout waiting for SGI#7 from CPU1 (req_seq=2 rsp_seq=2)
```

### After patch

CPU0's `cpu1_sgi_isr` fired immediately on SGI #7 with microsecond round-trip latency:

```text
uart:~$ cpu1 ping 42
CPU1 SGI#7 reply: 42^2 = 1764 (seq=1, sgi_irqs=1, rtt=16 us)
uart:~$ cpu1 stop
cpu1_shm @ 0x147000: magic=0x43505531 mpidr=0x80000001 (core=1) heartbeat=49 state=STOPPED sgi_irqs=1
CPU1 is PARKED (WFE)
uart:~$ cpu1 ping 123
CPU1 SGI#7 reply: 123^2 = 15129 (seq=2, sgi_irqs=2, rtt=7 us)
```

## Notes for Upstream Submission

- Verify `ubfx` availability across all AArch32 Cortex-A/R targets using GICv1/v2 (ARMv7-A/R supports `ubfx` in both ARM and Thumb-2; if ARMv5/ARMv6 compatibility is ever needed, `bic r0, r0, #0x1c00` or `ldr r1, =0x3ff; and r0, r0, r1` is an equivalent alternative).
- Both `#ifndef CONFIG_USE_SWITCH` and `#else` blocks in `arch/arm/core/cortex_a_r/isr_wrapper.S` must be patched together.

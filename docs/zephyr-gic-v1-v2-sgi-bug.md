# Technical Memo: Upstream Zephyr v4.5.0 GICv1/v2 SGI Drop Bug (`arch/arm/core/cortex_a_r/isr_wrapper.S`)

- **Date:** 2026-11-25
- **Zephyr version:** `v4.5.0` (commit `41d10b597304`)
- **Platform verified on:** Digilent PYNQ-Z1 (Xilinx Zynq-7000 `XC7Z020`, Dual Cortex-A9, GICv1)
- **Affected files:**
  - `arch/arm/core/cortex_a_r/isr_wrapper.S` (both `#ifndef CONFIG_USE_SWITCH` around line 184 and `#else` / `CONFIG_USE_SWITCH` around line 346)
  - `drivers/interrupt_controller/intc_gic.c` (`arm_gic_get_active()` lines 172–188, `arm_gic_eoi()` lines 191–205)
- **Reference implementation in Zephyr:** `arch/arm64/core/isr_wrapper.S` (lines 67–74)
- **Local fix patch:** `patches/0001-arch-arm-cortex_a_r-mask-gic-v1-v2-intid-in-isr-wrapper.patch`

## Summary

On AArch32 Cortex-A/R systems using GICv1 or GICv2 (`CONFIG_GIC_V1` / `CONFIG_GIC_V2`), any Software Generated Interrupt (SGI, INTID `0..15`) raised by a **non-zero CPU** (`CPUID >= 1`, e.g., CPU1 → CPU0) is silently dropped as a spurious interrupt by Zephyr's `_isr_wrapper`.

This breaks:
- **AMP / OpenAMP** inter-core signaling where CPU0 runs single-core Zephyr (`CONFIG_MP_MAX_NUM_CPUS=1`) and CPU1 (Linux, RTOS, or bare-metal) notifies CPU0 via GIC SGI.
- **SMP** scheduling IPIs (`SGI_SCHED_IPI = 0`) sent from secondary cores (`CPUID >= 1`) to CPU0 on AArch32 GICv1/v2 platforms.

## Root Cause & Exact Code Evidence

### 1. ARM GICv1/v2 Hardware Specification (ARM IHI 0048B)

- **Section 4.4.4 (`GICC_IAR`):**
  - Bits `[9:0]` = `Interrupt ID` (`INTID`, `0..1023`).
  - Bits `[12:10]` = `CPUID` (for SGIs `0..15`, identifies the requesting processor `0..7`; `0` for PPIs/SPIs).
- **Section 4.4.5 (`GICC_EOIR`):**
  - Software must write back the **full unmodified `GICC_IAR` value** (including `CPUID` in bits `[12:10]`) to acknowledge End-of-Interrupt.

### 2. What Zephyr `v4.5.0` Does on AArch32 vs. AArch64

In `drivers/interrupt_controller/intc_gic.c:172-188`, `arm_gic_get_active()` intentionally returns the raw `GICC_IAR` value so `arm_gic_eoi()` can write it back intact:

```c
unsigned int arm_gic_get_active(void)
{
	unsigned int irq;
	...
	irq = sys_read32(GICC_IAR);
	return irq;
}
```

On **AArch64**, `arch/arm64/core/isr_wrapper.S:67-74` preserves the raw `IAR` on the stack for `arm_gic_eoi()` and explicitly masks out `CPUID` bits `[12:10]` before bounds checking and `_sw_isr_table` lookup:

```asm
	/* Preserve original IAR value */
	str	x0, [sp, #-16]!

#if CONFIG_GIC_VER == 2 && defined(CONFIG_SMP)
	/* Mask out GICC_IAR.CPUID [12:10] */
	bic	x0, x0, #0x1c00
#endif
```

On **AArch32**, `arch/arm/core/cortex_a_r/isr_wrapper.S` (in **both** `#ifndef CONFIG_USE_SWITCH` at lines 178–224 and `#else` at lines 339–382) omits this mask completely:

```asm
	bl arm_gic_get_active
	push {r0, r1}
	...
	mov r1, #CONFIG_NUM_IRQS
	cmp r0, r1
	bge spurious_continue
```

### 3. Concrete Failure Trace (SGI #7 from CPU1 → CPU0)

When CPU1 writes `0x00010007` (`TargetListFilter=0b00`, `CPUTargetList=0x01` (CPU0), `SGIINTID=7`) to `GICD_SGIR` (`0xF8F01F00` on Zynq-7000):

1. CPU0 enters `_isr_wrapper` and calls `arm_gic_get_active()`.
2. `GICC_IAR` returns `(1 << 10) | 7 = 0x00000407` (**1031** decimal).
3. `push {r0, r1}` saves `0x407` on the stack.
4. `cmp r0, #CONFIG_NUM_IRQS` compares `1031 >= 96` → **true**, branches to `spurious_continue`.
5. `_sw_isr_table[7]` is **never called**.
6. `pop {r0, r1}` restores `0x407` and calls `arm_gic_eoi(0x407)`, cleanly clearing the interrupt so CPU0 does not hang — making the failure completely silent.

*(Why single-core tests miss this: an SGI raised by CPU0 to itself has `CPUID = 0`, so `GICC_IAR == INTID`. On GICv3, `ICC_IAR1_EL1` does not encode `CPUID`.)*

## Minimal Self-Contained Reproducer

Any dual-core Cortex-A9 GICv1/v2 board (such as Zynq-7000 ZedBoard, Zybo, or PYNQ-Z1) reproduces this with two steps:

### CPU0 (Zephyr)

Register an ISR on SGI #7, wake CPU1 (on Zynq-7000 via BootROM WFE address `0xFFFFFFF0` + `sev`), write a request sequence number to a shared `__nocache` struct, and wait on a semaphore signaled by the SGI ISR:

```c
#define CPU1_SGI_IRQ 7U
static K_SEM_DEFINE(sgi_sem, 0, 1);

static void sgi_isr(const void *arg)
{
	ARG_UNUSED(arg);
	k_sem_give(&sgi_sem);
}

/* During init on CPU0: */
IRQ_CONNECT(CPU1_SGI_IRQ, 0, sgi_isr, NULL, 0);
irq_enable(CPU1_SGI_IRQ);

/* Trigger request to CPU1 and wait for SGI #7 reply: */
shm.req_arg = 42U;
shm.req_seq = 1U;
barrier_dsync_fence_full();
__asm__ volatile("sev" ::: "memory");
int ret = k_sem_take(&sgi_sem, K_MSEC(500)); /* returns -EAGAIN before patch */
```

### CPU1 (minimal ARM payload)

Wait for `req_seq != rsp_seq` in the shared `__nocache` block, compute `rsp_val = req_arg * req_arg`, update `rsp_seq = req_seq`, and raise GICv1 SGI #7 to CPU0:

```asm
	/* r0 = &shm (in __nocache RAM) */
	ldr r5, [r0, #20]       /* req_seq */
	ldr r6, [r0, #28]       /* rsp_seq */
	cmp r5, r6
	beq loop
	ldr r6, [r0, #24]       /* req_arg */
	mul r7, r6, r6
	str r7, [r0, #32]       /* rsp_val */
	dsb sy
	str r5, [r0, #28]       /* rsp_seq = req_seq */
	dsb sy
	ldr r5, =0xF8F01F00     /* Zynq-7000 GICD_SGIR */
	ldr r6, =0x00010007     /* CPUTargetList=CPU0 (bit 16), SGIINTID=7 */
	str r6, [r5]
	dsb sy
```

Full reproducer in this repository: `src/cpu1_cmd.c` and `src/cpu1_trampoline.S` (invoked via Zephyr shell commands `cpu1 start` and `cpu1 ping <N>`).

## Hardware Verification Logs (Digilent PYNQ-Z1, Zynq-7000 XC7Z020)

### Before patch (unmodified Zephyr `v4.5.0`)

CPU1 executed the payload and updated `rsp_seq` (`req_seq=1 rsp_seq=1` and `req_seq=2 rsp_seq=2`), proving CPU1 ran and wrote `0x00010007` to `GICD_SGIR`, yet CPU0's `sgi_isr` never ran and `k_sem_take` timed out:

```text
uart:~$ cpu1 start
Waking CPU1 -> entry=0x0010433c (shm=0x147000)...
0xFFFFFFF0 = 0x0010433c
cpu1_shm @ 0x147000: magic=0x43505531 mpidr=0x80000001 (core=1) heartbeat=1 state=RUNNING sgi_irqs=0
CPU1 is ALIVE (RUNNING)
uart:~$ cpu1 ping 42
Timeout waiting for SGI#7 from CPU1 (req_seq=1 rsp_seq=1)
uart:~$ cpu1 ping 123
Timeout waiting for SGI#7 from CPU1 (req_seq=2 rsp_seq=2)
```

### After patch (`isr_wrapper.S` masks `GICC_IAR` to 10-bit `INTID`)

With only the `isr_wrapper.S` mask added (no changes to CPU0 or CPU1 test code), `sgi_isr` fired immediately on every SGI #7 from CPU1:

```text
uart:~$ cpu1 ping 42
CPU1 SGI#7 reply: 42^2 = 1764 (seq=1, sgi_irqs=1, rtt=16 us)
uart:~$ cpu1 stop
cpu1_shm @ 0x147000: magic=0x43505531 mpidr=0x80000001 (core=1) heartbeat=49 state=STOPPED sgi_irqs=1
CPU1 is PARKED (WFE)
uart:~$ cpu1 ping 123
CPU1 SGI#7 reply: 123^2 = 15129 (seq=2, sgi_irqs=2, rtt=7 us)
```

## Proposed Upstream Fix

In `arch/arm/core/cortex_a_r/isr_wrapper.S` (in **both** `#ifndef CONFIG_USE_SWITCH` and `#else` paths), mask out `GICC_IAR.CPUID` immediately **after** `push {r0, r1}`:

```asm
	push {r0, r1}

#if defined(CONFIG_GIC_V1) || defined(CONFIG_GIC_V2)
	/*
	 * GICC_IAR carries CPUID in bits [12:10] for SGIs from another core.
	 * Keep the full IAR on the stack for arm_gic_eoi(), but mask r0 to
	 * the 10-bit INTID before tracking, bounds-checking, and table lookup.
	 */
	ubfx r0, r0, #0, #10
#endif
```

*(Alternatively, `bic r0, r0, #0x1c00` mirrors `arch/arm64/core/isr_wrapper.S:73`.)*

### Why Not Gate on `CONFIG_SMP`?

Unlike `arch/arm64/core/isr_wrapper.S:71` (`#if CONFIG_GIC_VER == 2 && defined(CONFIG_SMP)`), gating on `CONFIG_SMP` breaks **AMP** configurations where Zephyr runs single-core (`CONFIG_MP_MAX_NUM_CPUS=1`, `CONFIG_SMP=n`) on CPU0 while another OS or firmware on CPU1 raises SGIs. Masking unconditionally under `CONFIG_GIC_V1 || CONFIG_GIC_V2` costs 1 cycle per IRQ and fixes both SMP and AMP.


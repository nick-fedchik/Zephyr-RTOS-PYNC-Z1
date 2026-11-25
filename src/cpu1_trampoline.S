/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal bare-metal ARM payload executed on Zynq-7000 CPU1 after wake-up
 * via the BootROM WFE loop (0xFFFFFFF0 + SEV).
 */

	.syntax unified
	.arm
	.section .text.cpu1_entry, "ax"
	.balign 4
	.global cpu1_entry
	.type cpu1_entry, %function

cpu1_entry:
	/* Keep IRQ/FIQ masked on CPU1 */
	cpsid if

	/* Read MPIDR (CP15 c0, 0, c0, 5) to verify Core 1 identity */
	mrc p15, 0, r1, c0, c0, 5

	/* Point r0 to shared __nocache status block:
	 *  #0  magic
	 *  #4  mpidr
	 *  #8  heartbeat
	 *  #12 run_cmd  (1 = run, 0 = park in WFE)
	 *  #16 state    (1 = running, 0 = stopped/parked)
	 *  #20 req_seq  (incremented by CPU0)
	 *  #24 req_arg  (input from CPU0)
	 *  #28 rsp_seq  (written by CPU1 when reply is ready)
	 *  #32 rsp_val  (result = req_arg * req_arg)
	 */
	ldr r0, =cpu1_shm

	/* Store magic "CPU1" (0x43505531) and MPIDR */
	ldr r2, =0x43505531
	str r2, [r0, #0]
	str r1, [r0, #4]
	mov r3, #0
	str r3, [r0, #8]
	dsb sy

1:
	/* Handle pending IPC request from CPU0 if req_seq != rsp_seq */
	bl cpu1_check_ipc

	/* Check run_cmd */
	ldr r2, [r0, #12]
	cmp r2, #0
	beq 3f

	/* Mark running and increment heartbeat */
	mov r2, #1
	str r2, [r0, #16]
	add r3, r3, #1
	str r3, [r0, #8]
	dsb sy

	/* Delay loop (~tens of ms), polling IPC every 256 iterations */
	ldr r4, =2000000
2:
	tst r4, #0xFF
	bleq cpu1_check_ipc
	subs r4, r4, #1
	bne 2b
	b 1b

3:
	/* Mark stopped and park in low-power WFE until CPU0 sends SEV */
	mov r2, #0
	str r2, [r0, #16]
	dsb sy
	wfe
	b 1b

/* Leaf helper: if req_seq != rsp_seq, compute rsp_val = req_arg^2 and raise GIC SGI#7 to CPU0 */
cpu1_check_ipc:
	ldr r5, [r0, #20]
	ldr r6, [r0, #28]
	cmp r5, r6
	bxeq lr
	ldr r6, [r0, #24]
	mul r7, r6, r6
	str r7, [r0, #32]
	dsb sy
	str r5, [r0, #28]
	dsb sy
	/* Trigger GICv1 SGI #7 targeted to CPU0 (GICD_SGIR = 0xF8F01F00) */
	ldr r5, =0xF8F01F00
	ldr r6, =0x00010007
	str r6, [r5]
	dsb sy
	bx lr

	.size cpu1_entry, . - cpu1_entry

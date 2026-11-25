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

	/* Point r0 to shared __nocache status block */
	ldr r0, =cpu1_shm

	/* Store magic "CPU1" (0x43505531) and MPIDR */
	ldr r2, =0x43505531
	str r2, [r0, #0]
	str r1, [r0, #4]
	mov r3, #0
	str r3, [r0, #8]
	dsb sy

1:
	add r3, r3, #1
	str r3, [r0, #8]
	dsb sy

	/* Delay loop (~tens of ms at 650 MHz with caches off) */
	ldr r4, =2000000
2:
	subs r4, r4, #1
	bne 2b
	b 1b

	.size cpu1_entry, . - cpu1_entry

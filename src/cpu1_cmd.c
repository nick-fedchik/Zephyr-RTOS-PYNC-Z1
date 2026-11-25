/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zynq-7000 CPU1 wake-up test (BootROM WFE loop at 0xFFFFFFF0 + SEV).
 */

#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/sys_io.h>
#include <stdint.h>

#define ZYNQ7000_CPU1_START_ADDR 0xFFFFFFF0U
#define CPU1_MAGIC               0x43505531U /* "CPU1" */

struct cpu1_shared_state {
	volatile uint32_t magic;
	volatile uint32_t mpidr;
	volatile uint32_t heartbeat;
};

extern void cpu1_entry(void);

struct cpu1_shared_state cpu1_shm __nocache;

static bool cpu1_started;

static int cmd_cpu1_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	uint32_t start_reg = sys_read32(ZYNQ7000_CPU1_START_ADDR);

	barrier_dsync_fence_full();
	shell_print(sh, "0xFFFFFFF0 = 0x%08x", start_reg);
	shell_print(sh, "cpu1_shm @ %p: magic=0x%08x mpidr=0x%08x (core=%u) heartbeat=%u",
		    (void *)&cpu1_shm, cpu1_shm.magic, cpu1_shm.mpidr,
		    cpu1_shm.mpidr & 0x3U, cpu1_shm.heartbeat);

	if (cpu1_shm.magic == CPU1_MAGIC) {
		shell_print(sh, "CPU1 is ALIVE");
	} else {
		shell_print(sh, "CPU1 has not reported in yet");
	}

	return 0;
}

static int cmd_cpu1_start(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (cpu1_started) {
		shell_warn(sh, "CPU1 wake-up was already triggered; use 'cpu1 status'");
		return 0;
	}

	uintptr_t entry_addr = (uintptr_t)cpu1_entry;

	cpu1_shm.magic = 0U;
	cpu1_shm.mpidr = 0U;
	cpu1_shm.heartbeat = 0U;
	barrier_dsync_fence_full();

	shell_print(sh, "Waking CPU1 -> entry=0x%08x (shm=%p)...",
		    (uint32_t)entry_addr, (void *)&cpu1_shm);

	sys_write32((uint32_t)entry_addr, ZYNQ7000_CPU1_START_ADDR);
	barrier_dsync_fence_full();
	__asm__ volatile("sev" ::: "memory");

	cpu1_started = true;
	k_msleep(100);

	return cmd_cpu1_status(sh, 0, NULL);
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_cpu1,
	SHELL_CMD(start, NULL, "Wake CPU1 via 0xFFFFFFF0 + SEV", cmd_cpu1_start),
	SHELL_CMD(status, NULL, "Show CPU1 magic, MPIDR, and heartbeat", cmd_cpu1_status),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(cpu1, &sub_cpu1, "Zynq-7000 CPU1 bring-up experiment", NULL);

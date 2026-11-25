/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zynq-7000 CPU1 wake-up test (BootROM WFE loop at 0xFFFFFFF0 + SEV).
 */

#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/sys_io.h>
#include <stdint.h>
#include <stdlib.h>

#define ZYNQ7000_CPU1_START_ADDR 0xFFFFFFF0U
#define CPU1_MAGIC               0x43505531U /* "CPU1" */
#define CPU1_SGI_IRQ             7U

struct cpu1_shared_state {
	volatile uint32_t magic;
	volatile uint32_t mpidr;
	volatile uint32_t heartbeat;
	volatile uint32_t run_cmd;
	volatile uint32_t state;
	volatile uint32_t req_seq;
	volatile uint32_t req_arg;
	volatile uint32_t rsp_seq;
	volatile uint32_t rsp_val;
};

extern void cpu1_entry(void);

struct cpu1_shared_state cpu1_shm __nocache;

static bool cpu1_booted;
static bool sgi_installed;
static volatile uint32_t sgi_irq_count;
static K_SEM_DEFINE(cpu1_sgi_sem, 0, 1);

static void cpu1_sgi_isr(const void *arg)
{
	ARG_UNUSED(arg);
	sgi_irq_count++;
	k_sem_give(&cpu1_sgi_sem);
}

static void cpu1_ensure_sgi_irq(void)
{
	if (!sgi_installed) {
		IRQ_CONNECT(CPU1_SGI_IRQ, 0, cpu1_sgi_isr, NULL, 0);
		irq_enable(CPU1_SGI_IRQ);
		sgi_installed = true;
	}
}

static int cmd_cpu1_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	uint32_t start_reg = sys_read32(ZYNQ7000_CPU1_START_ADDR);

	barrier_dsync_fence_full();
	shell_print(sh, "0xFFFFFFF0 = 0x%08x", start_reg);
	shell_print(sh, "cpu1_shm @ %p: magic=0x%08x mpidr=0x%08x (core=%u) heartbeat=%u state=%s sgi_irqs=%u",
		    (void *)&cpu1_shm, cpu1_shm.magic, cpu1_shm.mpidr,
		    cpu1_shm.mpidr & 0x3U, cpu1_shm.heartbeat,
		    cpu1_shm.state ? "RUNNING" : "STOPPED", sgi_irq_count);

	if (cpu1_shm.magic == CPU1_MAGIC) {
		shell_print(sh, "CPU1 is %s",
			    cpu1_shm.state ? "ALIVE (RUNNING)" : "PARKED (WFE)");
	} else {
		shell_print(sh, "CPU1 has not reported in yet");
	}

	return 0;
}

static int cmd_cpu1_start(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	cpu1_ensure_sgi_irq();

	if (cpu1_booted) {
		cpu1_shm.run_cmd = 1U;
		barrier_dsync_fence_full();
		__asm__ volatile("sev" ::: "memory");
		k_msleep(100);
		return cmd_cpu1_status(sh, 0, NULL);
	}

	uintptr_t entry_addr = (uintptr_t)cpu1_entry;

	cpu1_shm.magic = 0U;
	cpu1_shm.mpidr = 0U;
	cpu1_shm.heartbeat = 0U;
	cpu1_shm.run_cmd = 1U;
	cpu1_shm.state = 0U;
	cpu1_shm.req_seq = 0U;
	cpu1_shm.req_arg = 0U;
	cpu1_shm.rsp_seq = 0U;
	cpu1_shm.rsp_val = 0U;
	barrier_dsync_fence_full();

	shell_print(sh, "Waking CPU1 -> entry=0x%08x (shm=%p)...",
		    (uint32_t)entry_addr, (void *)&cpu1_shm);

	sys_write32((uint32_t)entry_addr, ZYNQ7000_CPU1_START_ADDR);
	barrier_dsync_fence_full();
	__asm__ volatile("sev" ::: "memory");

	cpu1_booted = true;
	k_msleep(100);

	return cmd_cpu1_status(sh, 0, NULL);
}

static int cmd_cpu1_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!cpu1_booted) {
		shell_warn(sh, "CPU1 has not been started yet");
		return 0;
	}

	cpu1_shm.run_cmd = 0U;
	barrier_dsync_fence_full();

	for (int i = 0; i < 50 && cpu1_shm.state != 0U; i++) {
		k_msleep(10);
	}

	return cmd_cpu1_status(sh, 0, NULL);
}

static int cmd_cpu1_ping(const struct shell *sh, size_t argc, char **argv)
{
	if (!cpu1_booted) {
		shell_warn(sh, "CPU1 has not been started yet; run 'cpu1 start' first");
		return 0;
	}

	uint32_t val = (argc >= 2) ? (uint32_t)strtoul(argv[1], NULL, 0) : 42U;
	uint32_t seq = cpu1_shm.req_seq + 1U;

	k_sem_reset(&cpu1_sgi_sem);
	cpu1_shm.req_arg = val;
	barrier_dsync_fence_full();

	uint32_t t0 = k_cycle_get_32();

	cpu1_shm.req_seq = seq;
	barrier_dsync_fence_full();
	__asm__ volatile("sev" ::: "memory");

	int ret = k_sem_take(&cpu1_sgi_sem, K_MSEC(500));
	uint32_t t1 = k_cycle_get_32();

	if (ret != 0) {
		shell_error(sh, "Timeout waiting for SGI#%u from CPU1 (req_seq=%u rsp_seq=%u)",
			    CPU1_SGI_IRQ, seq, cpu1_shm.rsp_seq);
		return -ETIMEDOUT;
	}

	uint32_t rtt_us = k_cyc_to_us_near32(t1 - t0);

	shell_print(sh, "CPU1 SGI#%u reply: %u^2 = %u (seq=%u, sgi_irqs=%u, rtt=%u us)",
		    CPU1_SGI_IRQ, val, cpu1_shm.rsp_val, cpu1_shm.rsp_seq,
		    sgi_irq_count, rtt_us);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_cpu1,
	SHELL_CMD(start, NULL, "Wake/resume CPU1 via 0xFFFFFFF0 + SEV", cmd_cpu1_start),
	SHELL_CMD(stop, NULL, "Park CPU1 in low-power WFE", cmd_cpu1_stop),
	SHELL_CMD(status, NULL, "Show CPU1 magic, MPIDR, state, and heartbeat", cmd_cpu1_status),
	SHELL_CMD_ARG(ping, NULL, "Send IPC request to CPU1 and wait for GIC SGI#7 reply [num]",
		      cmd_cpu1_ping, 1, 1),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(cpu1, &sub_cpu1, "Zynq-7000 CPU1 bring-up experiment", NULL);

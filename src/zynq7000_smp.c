/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zynq-7000 secondary CPU bring-up (pm_cpu_on via BootROM 0xFFFFFFF0 + SEV)
 * and SMP verification shell commands.
 */

#include <zephyr/arch/cpu.h>
#include <zephyr/drivers/pm_cpu_ops.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/sys_io.h>
#include <cmsis_core.h>
#include <errno.h>
#include <stdint.h>

#define ZYNQ7000_CPU1_START_ADDR 0xFFFFFFF0U

int pm_cpu_on(unsigned long cpuid, uintptr_t entry_point)
{
	if (cpuid != 1UL) {
		return -EINVAL;
	}

	printk("pm_cpu_on: cpu=%lu entry=0x%08x\n", cpuid, (uint32_t)entry_point);
	sys_write32((uint32_t)entry_point, ZYNQ7000_CPU1_START_ADDR);
	barrier_dsync_fence_full();
	__asm__ volatile("sev" ::: "memory");

	return 0;
}

K_THREAD_STACK_DEFINE(smp_stack_cpu0, 2048);
K_THREAD_STACK_DEFINE(smp_stack_cpu1, 2048);
static struct k_thread smp_thread_cpu0;
static struct k_thread smp_thread_cpu1;

struct smp_worker_result {
	struct k_sem done;
	int expected_cpu;
	int actual_cpu;
	uint32_t mpidr;
	uint32_t actlr;
	uint32_t cycles;
};

static void smp_worker(void *p1, void *p2, void *p3)
{
	struct smp_worker_result *res = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint32_t t0 = k_cycle_get_32();
	volatile uint32_t acc = 0;

	for (uint32_t i = 0; i < 500000U; i++) {
		acc += i;
	}
	ARG_UNUSED(acc);

	res->cycles = k_cycle_get_32() - t0;
	res->actual_cpu = arch_curr_cpu()->id;
	res->mpidr = __get_MPIDR();
	res->actlr = __get_ACTLR();
	k_sem_give(&res->done);
}

static int cmd_smp_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "CONFIG_SMP=y, num_cpus=%u, current_cpu=%d, MPIDR=0x%08x, ACTLR=0x%08x",
		    arch_num_cpus(), arch_curr_cpu()->id, __get_MPIDR(), __get_ACTLR());
	return 0;
}

static int cmd_smp_test(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct smp_worker_result res[2];
	k_tid_t tids[2];
	k_thread_stack_t *stacks[2] = { smp_stack_cpu0, smp_stack_cpu1 };
	struct k_thread *threads[2] = { &smp_thread_cpu0, &smp_thread_cpu1 };

	for (int cpu = 0; cpu < 2; cpu++) {
		k_sem_init(&res[cpu].done, 0, 1);
		res[cpu].expected_cpu = cpu;
		res[cpu].actual_cpu = -1;
		res[cpu].mpidr = 0U;
		res[cpu].actlr = 0U;
		res[cpu].cycles = 0U;

		tids[cpu] = k_thread_create(threads[cpu], stacks[cpu], 2048,
					    smp_worker, &res[cpu], NULL, NULL,
					    K_PRIO_PREEMPT(5), 0, K_FOREVER);
		k_thread_cpu_mask_clear(tids[cpu]);
		k_thread_cpu_mask_enable(tids[cpu], cpu);
	}

	uint32_t wall_t0 = k_cycle_get_32();

	k_thread_start(tids[0]);
	k_thread_start(tids[1]);

	for (int cpu = 0; cpu < 2; cpu++) {
		if (k_sem_take(&res[cpu].done, K_MSEC(2000)) != 0) {
			shell_error(sh, "Timeout waiting for worker pinned to CPU%d", cpu);
			return -ETIMEDOUT;
		}
	}

	uint32_t wall_us = k_cyc_to_us_near32(k_cycle_get_32() - wall_t0);

	for (int cpu = 0; cpu < 2; cpu++) {
		shell_print(sh, "Worker[CPU%d]: ran on cpu=%d MPIDR=0x%08x ACTLR=0x%08x time=%u us",
			    cpu, res[cpu].actual_cpu, res[cpu].mpidr, res[cpu].actlr,
			    k_cyc_to_us_near32(res[cpu].cycles));
	}
	shell_print(sh, "Total wall time (parallel): %u us", wall_us);

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_smp,
	SHELL_CMD(info, NULL, "Show active SMP CPU count and current core", cmd_smp_info),
	SHELL_CMD(test, NULL, "Run parallel workers pinned to CPU0 and CPU1", cmd_smp_test),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(smp, &sub_smp, "Zynq-7000 dual-core SMP verification", NULL);

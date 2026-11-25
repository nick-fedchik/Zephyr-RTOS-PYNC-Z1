/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal Zephyr AMP firmware for Zynq-7000 CPU1 loaded by Linux
 * zynq_remoteproc. Publishes a Linux-compatible .resource_table with a
 * RSC_TRACE buffer readable from:
 *   /sys/kernel/debug/remoteproc/remoteproc0/trace0
 */

#include <zephyr/arch/cpu.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/linker-defs.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/sys/barrier.h>
#include <cmsis_core.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RSC_TRACE 2U
#define AMP_TRACE_BUF_SIZE 2048U

/*
 * Override the weak Zynq/ARMv7-A relocate_vector_table() so CPU1 keeps its
 * exception vectors inside 0x1e000000 via VBAR and never overwrites Linux's
 * vector table at 0x00000000.
 */
void relocate_vector_table(void)
{
	uint32_t sctlr = __get_SCTLR();

	sctlr &= ~SCTLR_V_Msk; /* Clear HIVECS so VBAR is active */
	__set_SCTLR(sctlr);
	__set_VBAR((uint32_t)_vector_start);
	barrier_isync_fence_full();
}

struct fw_rsc_trace {
	uint32_t type;
	uint32_t da;
	uint32_t len;
	uint32_t reserved;
	uint8_t name[32];
} __packed;

struct amp_resource_table {
	uint32_t ver;
	uint32_t num;
	uint32_t reserved[2];
	uint32_t offset[1];
	struct fw_rsc_trace trace;
} __packed;

static char amp_trace_buf[AMP_TRACE_BUF_SIZE] __nocache;
static size_t amp_trace_pos;

__attribute__((section(".resource_table"), used))
const struct amp_resource_table rproc_resource_table = {
	.ver = 1U,
	.num = 1U,
	.reserved = { 0U, 0U },
	.offset = { offsetof(struct amp_resource_table, trace) },
	.trace = {
		.type = RSC_TRACE,
		.da = (uint32_t)(uintptr_t)amp_trace_buf,
		.len = AMP_TRACE_BUF_SIZE,
		.reserved = 0U,
		.name = "zephyr_cpu1_trace",
	},
};

static void trace_append(const char *msg)
{
	size_t len = strlen(msg);

	if (len >= AMP_TRACE_BUF_SIZE - 1U) {
		return;
	}

	if (amp_trace_pos + len >= AMP_TRACE_BUF_SIZE - 1U) {
		amp_trace_pos = 0U;
	}

	memcpy(&amp_trace_buf[amp_trace_pos], msg, len);
	amp_trace_pos += len;
	amp_trace_buf[amp_trace_pos] = '\0';
	barrier_dsync_fence_full();
}

int main(void)
{
	char line[96];
	uint32_t mpidr = __get_MPIDR();
	uint32_t vbar = __get_VBAR();
	uint32_t tick = 0U;

	memset(amp_trace_buf, 0, sizeof(amp_trace_buf));
	snprintf(line, sizeof(line),
		 "*** Zephyr AMP on PYNQ-Z1 CPU1 booted (MPIDR=0x%08x VBAR=0x%08x) ***\n",
		 mpidr, vbar);
	trace_append(line);

	while (1) {
		k_msleep(1000);
		tick++;
		snprintf(line, sizeof(line),
			 "Zephyr CPU1 heartbeat tick=%u uptime_ms=%lld\n",
			 tick, k_uptime_get());
		trace_append(line);
	}

	return 0;
}

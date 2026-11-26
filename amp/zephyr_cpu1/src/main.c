/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr AMP + OpenAMP/RPMsg firmware for Zynq-7000 CPU1 loaded by Linux
 * zynq_remoteproc.
 *  - Trace buffer: /sys/kernel/debug/remoteproc/remoteproc0/trace0
 *  - RPMsg echo endpoint: "rpmsg-raw" (/dev/rpmsg0 via Linux rpmsg_char)
 *  - Inter-processor kicks: GICv1 SGI #14 (Linux -> CPU1) and SGI #15 (CPU1 -> Linux)
 */

#include <zephyr/arch/cpu.h>
#include <zephyr/cache.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/linker-defs.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/sys_io.h>
#include <cmsis_core.h>
#include <openamp/open_amp.h>
#include <metal/io.h>
#include <metal/sys.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AMP_TRACE_BUF_SIZE 2048U

/* Shared memory layout matching amp/openamp.dts */
#define SHM_REGION_ADDR 0x1F000000U
#define SHM_REGION_SIZE 0x00108000U
#define VRING0_ADDR     0x1F000000U
#define VRING1_ADDR     0x1F004000U
#define VRING_ALIGN     4096U
#define VRING_NUM       16U

/* Zynq-7000 GICv1 SGI registers (zynq_remoteproc uses SGIs 14 and 15) */
#define GICD_SGIR_ADDR   0xF8F01F00U
#define SGI_FROM_LINUX_0 14U
#define SGI_FROM_LINUX_1 15U
#define SGI_TO_LINUX     15U

/* PYNQ-Z1 base.bit AXI GPIO physical addresses (from base.hwh) */
#define AXI_SW_BASE      0x41200000U /* SW0-SW1 (2 bits) */
#define AXI_BTN_BASE     0x41210000U /* BTN0-BTN3 (4 bits) */
#define AXI_RGB_BASE     0x41240000U /* LD4-LD5 RGB (6 bits) */
#define AXI_LED_BASE     0x41250000U /* LD0-LD3 (4 bits) */
#define AXI_GPIO_DATA    0x00U
#define AXI_GPIO_TRI     0x04U

#define RPMSG_SVC_NAME "rpmsg-raw"

static char amp_trace_buf[AMP_TRACE_BUF_SIZE] __nocache;
static size_t amp_trace_pos;

void relocate_vector_table(void)
{
	uint32_t sctlr = __get_SCTLR();

	L1C_InvalidateICacheAll();
	L1C_InvalidateBTAC();
	L1C_InvalidateDCacheAll();
	/* Join Cortex-A9 SCU coherency (Linux already enabled SCU on CPU0) */
	__set_ACTLR(__get_ACTLR() | BIT(6) | BIT(0));

	sctlr &= ~SCTLR_V_Msk; /* Clear HIVECS so VBAR is active */
	__set_SCTLR(sctlr);
	__set_VBAR((uint32_t)_vector_start);
	barrier_dsync_fence_full();
	barrier_isync_fence_full();
}

struct amp_resource_table {
	uint32_t ver;
	uint32_t num;
	uint32_t reserved[2];
	uint32_t offset[2];
	struct fw_rsc_trace trace;
	struct fw_rsc_vdev rpmsg_vdev;
	struct fw_rsc_vdev_vring vring0;
	struct fw_rsc_vdev_vring vring1;
} __packed;

__attribute__((section(".resource_table"), used))
struct amp_resource_table rproc_resource_table = {
	.ver = 1U,
	.num = 2U,
	.reserved = { 0U, 0U },
	.offset = {
		offsetof(struct amp_resource_table, trace),
		offsetof(struct amp_resource_table, rpmsg_vdev),
	},
	.trace = {
		.type = RSC_TRACE,
		.da = (uint32_t)(uintptr_t)amp_trace_buf,
		.len = AMP_TRACE_BUF_SIZE,
		.reserved = 0U,
		.name = "zephyr_cpu1_trace",
	},
	.rpmsg_vdev = {
		.type = RSC_VDEV,
		.id = VIRTIO_ID_RPMSG,
		.notifyid = 0U,
		.dfeatures = (1U << VIRTIO_RPMSG_F_NS),
		.gfeatures = 0U,
		.config_len = 0U,
		.status = 0U,
		.num_of_vrings = 2U,
		.reserved = { 0U, 0U },
	},
	.vring0 = {
		.da = VRING0_ADDR,
		.align = VRING_ALIGN,
		.num = VRING_NUM,
		.notifyid = 0U,
		.reserved = 0U,
	},
	.vring1 = {
		.da = VRING1_ADDR,
		.align = VRING_ALIGN,
		.num = VRING_NUM,
		.notifyid = 1U,
		.reserved = 0U,
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

static K_SEM_DEFINE(rpmsg_kick_sem, 0, 1);
static struct metal_io_region shm_io;
static struct metal_io_region rsc_io;
static metal_phys_addr_t shm_phys = SHM_REGION_ADDR;
static metal_phys_addr_t rsc_phys;
static struct rpmsg_virtio_device rvdev;
static struct rpmsg_endpoint raw_ept;
static uint32_t msg_count;
static uint32_t blink_period_ms;
static uint8_t blink_step;

static void axi_gpio_init_pl(void)
{
	/* Configure LD0-LD3 and RGB LD4-LD5 as outputs (TRI=0) */
	sys_write32(0x0U, AXI_LED_BASE + AXI_GPIO_TRI);
	sys_write32(0x0U, AXI_RGB_BASE + AXI_GPIO_TRI);
}

static void blink_timer_cb(struct k_timer *timer)
{
	static const uint8_t pattern[4] = { 0x1U, 0x2U, 0x4U, 0x8U };

	ARG_UNUSED(timer);
	sys_write32(pattern[blink_step & 0x3U], AXI_LED_BASE + AXI_GPIO_DATA);
	blink_step++;
}

static K_TIMER_DEFINE(pl_blink_timer, blink_timer_cb, NULL);

static void sgi_kick_isr(const void *arg)
{
	ARG_UNUSED(arg);
	k_sem_give(&rpmsg_kick_sem);
}

static int zynq_amp_notify(void *priv, uint32_t id)
{
	ARG_UNUSED(priv);
	ARG_UNUSED(id);

	/* Raise GICv1 SGI #15 targeted to CPU0 (Linux) */
	barrier_dsync_fence_full();
	sys_write32((1U << 16) | SGI_TO_LINUX, GICD_SGIR_ADDR);
	barrier_dsync_fence_full();
	return 0;
}

static int raw_ept_cb(struct rpmsg_endpoint *ept, void *data, size_t len,
		      uint32_t src, void *priv)
{
	char rx_buf[128];
	char tx_buf[192];
	size_t copy_len = (len < sizeof(rx_buf) - 1U) ? len : (sizeof(rx_buf) - 1U);

	ARG_UNUSED(src);
	ARG_UNUSED(priv);

	memcpy(rx_buf, data, copy_len);
	rx_buf[copy_len] = '\0';
	while (copy_len > 0U && (rx_buf[copy_len - 1U] == '\n' || rx_buf[copy_len - 1U] == '\r')) {
		rx_buf[--copy_len] = '\0';
	}

	msg_count++;
	trace_append("RX: ");
	trace_append(rx_buf);
	trace_append("\n");

	if (strncmp(rx_buf, "led ", 4) == 0) {
		uint32_t val = (uint32_t)strtoul(&rx_buf[4], NULL, 0) & 0xFU;

		k_timer_stop(&pl_blink_timer);
		blink_period_ms = 0U;
		sys_write32(val, AXI_LED_BASE + AXI_GPIO_DATA);
		snprintf(tx_buf, sizeof(tx_buf), "OK led=0x%x\n", val);
	} else if (strncmp(rx_buf, "rgb ", 4) == 0) {
		uint32_t val = (uint32_t)strtoul(&rx_buf[4], NULL, 0) & 0x3FU;

		sys_write32(val, AXI_RGB_BASE + AXI_GPIO_DATA);
		snprintf(tx_buf, sizeof(tx_buf), "OK rgb=0x%02x\n", val);
	} else if (strncmp(rx_buf, "blink ", 6) == 0) {
		uint32_t ms = (uint32_t)strtoul(&rx_buf[6], NULL, 0);

		if (ms == 0U) {
			k_timer_stop(&pl_blink_timer);
			blink_period_ms = 0U;
			sys_write32(0U, AXI_LED_BASE + AXI_GPIO_DATA);
			snprintf(tx_buf, sizeof(tx_buf), "OK blink=stopped\n");
		} else {
			blink_period_ms = ms;
			k_timer_start(&pl_blink_timer, K_NO_WAIT, K_MSEC(ms));
			snprintf(tx_buf, sizeof(tx_buf), "OK blink=%u ms\n", ms);
		}
	} else if (strcmp(rx_buf, "read") == 0) {
		uint32_t btns = sys_read32(AXI_BTN_BASE + AXI_GPIO_DATA) & 0xFU;
		uint32_t sws = sys_read32(AXI_SW_BASE + AXI_GPIO_DATA) & 0x3U;
		uint32_t leds = sys_read32(AXI_LED_BASE + AXI_GPIO_DATA) & 0xFU;
		uint32_t rgb = sys_read32(AXI_RGB_BASE + AXI_GPIO_DATA) & 0x3FU;

		snprintf(tx_buf, sizeof(tx_buf),
			 "OK btns=0x%x sw=0x%x leds=0x%x rgb=0x%02x blink_ms=%u\n",
			 btns, sws, leds, rgb, blink_period_ms);
	} else {
		snprintf(tx_buf, sizeof(tx_buf), "[Zephyr CPU1 #%u] echo: %s\n", msg_count, rx_buf);
	}

	(void)rpmsg_send(ept, tx_buf, strlen(tx_buf));
	return RPMSG_SUCCESS;
}

static void ns_bind_cb(struct rpmsg_device *rdev, const char *name, uint32_t dest)
{
	ARG_UNUSED(rdev);
	ARG_UNUSED(name);
	ARG_UNUSED(dest);
}

int main(void)
{
	struct metal_init_params metal_params = METAL_INIT_DEFAULTS;
	struct virtio_device *vdev;
	struct rpmsg_device *rdev;
	char line[96];
	int ret;

	amp_trace_pos = 0U;
	axi_gpio_init_pl();
	trace_append("*** Zephyr AMP + OpenAMP + PL AXI GPIO on PYNQ-Z1 CPU1 booted ***\n");
	snprintf(line, sizeof(line), "MPIDR=0x%08x VBAR=0x%08x\n",
		 __get_MPIDR(), __get_VBAR());
	trace_append(line);

	IRQ_CONNECT(SGI_FROM_LINUX_0, 0, sgi_kick_isr, NULL, 0);
	IRQ_CONNECT(SGI_FROM_LINUX_1, 0, sgi_kick_isr, NULL, 0);
	irq_enable(SGI_FROM_LINUX_0);
	irq_enable(SGI_FROM_LINUX_1);

	ret = metal_init(&metal_params);
	if (ret != 0) {
		trace_append("ERROR: metal_init failed\n");
		return ret;
	}

	metal_io_init(&shm_io, (void *)SHM_REGION_ADDR, &shm_phys,
		      SHM_REGION_SIZE, -1, 0, NULL);

	rsc_phys = (metal_phys_addr_t)(uintptr_t)&rproc_resource_table;
	metal_io_init(&rsc_io, &rproc_resource_table, &rsc_phys,
		      sizeof(rproc_resource_table), -1, 0, NULL);

	sys_cache_data_invd_range(&rproc_resource_table, sizeof(rproc_resource_table));

	vdev = rproc_virtio_create_vdev(VIRTIO_DEV_DEVICE, 0U,
					&rproc_resource_table.rpmsg_vdev,
					&rsc_io, NULL, zynq_amp_notify, NULL);
	if (vdev == NULL) {
		trace_append("ERROR: rproc_virtio_create_vdev failed\n");
		return -1;
	}

	trace_append("Waiting for Linux remoteproc DRIVER_OK...\n");
	while ((rproc_resource_table.rpmsg_vdev.status & VIRTIO_CONFIG_STATUS_DRIVER_OK) == 0U) {
		sys_cache_data_invd_range(&rproc_resource_table, sizeof(rproc_resource_table));
		k_msleep(10);
	}
	trace_append("Linux DRIVER_OK received!\n");

	ret = rproc_virtio_init_vring(vdev, 0, rproc_resource_table.vring0.notifyid,
				      (void *)(uintptr_t)rproc_resource_table.vring0.da,
				      &shm_io, rproc_resource_table.vring0.num,
				      rproc_resource_table.vring0.align);
	if (ret != 0) {
		trace_append("ERROR: init_vring 0 failed\n");
		return ret;
	}

	ret = rproc_virtio_init_vring(vdev, 1, rproc_resource_table.vring1.notifyid,
				      (void *)(uintptr_t)rproc_resource_table.vring1.da,
				      &shm_io, rproc_resource_table.vring1.num,
				      rproc_resource_table.vring1.align);
	if (ret != 0) {
		trace_append("ERROR: init_vring 1 failed\n");
		return ret;
	}

	ret = rpmsg_init_vdev(&rvdev, vdev, ns_bind_cb, &shm_io, NULL);
	if (ret != 0) {
		trace_append("ERROR: rpmsg_init_vdev failed\n");
		return ret;
	}

	rdev = rpmsg_virtio_get_rpmsg_device(&rvdev);
	ret = rpmsg_create_ept(&raw_ept, rdev, RPMSG_SVC_NAME,
			       RPMSG_ADDR_ANY, RPMSG_ADDR_ANY,
			       raw_ept_cb, NULL);
	if (ret != 0) {
		trace_append("ERROR: rpmsg_create_ept failed\n");
		return ret;
	}

	trace_append("RPMsg endpoint 'rpmsg-raw' ready!\n");

	while (1) {
		k_sem_take(&rpmsg_kick_sem, K_MSEC(100));
		rproc_virtio_notified(rvdev.vdev, 0U);
		rproc_virtio_notified(rvdev.vdev, 1U);
	}

	return 0;
}

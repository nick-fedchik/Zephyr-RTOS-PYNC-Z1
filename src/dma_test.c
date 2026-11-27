/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shell test commands for the Zynq-7000 PS DMAC (PL330) memory-to-memory driver:
 *   dma info              - print the controller configuration
 *   dma selftest [kib]    - copy <kib> KiB (default 64, max 1024), verify, compare with memcpy
 *   dma align             - unaligned source/destination/length sweep with guard-byte checks
 *   dma multi             - all 8 channels at the same time
 *   dma chain             - three chained blocks on one channel
 *   dma fill              - fixed source address (pattern fill)
 *   dma stop              - abort a running transfer, then run a full one on the same channel
 *   dma neg               - invalid configurations must be rejected
 */

#include <stdlib.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#define NUM_CH   8
#define BUF_SZ   (1024 * 1024)
#define GUARD    64
#define SENTINEL 0x7fff
#define TMO_MS   5000
#define FILL     0xa5

static const struct device *const dma_dev = DEVICE_DT_GET(DT_NODELABEL(dmac_s));

static uint8_t src_buf[BUF_SZ] __aligned(32);
static uint8_t dst_buf[BUF_SZ + 2 * GUARD + 64] __aligned(32);
static uint8_t cpu_buf[BUF_SZ] __aligned(32);

static volatile int dma_rc[NUM_CH];
static struct k_sem dma_sem[NUM_CH];

static void dma_cb(const struct device *dev, void *user_data, uint32_t ch, int status)
{
	dma_rc[ch] = status;
	k_sem_give(&dma_sem[ch]);
}

static void fill_pattern(uint8_t *p, size_t n, uint32_t seed)
{
	uint32_t x = seed | 1;

	for (size_t i = 0; i < n; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		p[i] = x;
	}
}

static void blk_init(struct dma_block_config *b, const void *src, void *dst, size_t len)
{
	memset(b, 0, sizeof(*b));
	b->source_address = (uintptr_t)src;
	b->dest_address = (uintptr_t)dst;
	b->block_size = len;
	b->source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	b->dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;
}

static int chan_config(uint32_t ch, struct dma_block_config *head, uint32_t nblk)
{
	struct dma_config cfg = {
		.channel_direction = MEMORY_TO_MEMORY,
		.complete_callback_en = 1,
		.block_count = nblk,
		.head_block = head,
		.dma_callback = dma_cb,
	};

	k_sem_init(&dma_sem[ch], 0, 1);
	dma_rc[ch] = SENTINEL;

	return dma_config(dma_dev, ch, &cfg);
}

static uint32_t cyc_to_us(uint32_t cyc)
{
	return (uint32_t)k_cyc_to_us_floor64(cyc);
}

static uint32_t mib_s(size_t bytes, uint32_t us)
{
	return us ? (uint32_t)((uint64_t)bytes * 1000000 / us / 1024 / 1024) : 0;
}

static int wait_done(uint32_t ch)
{
	if (k_sem_take(&dma_sem[ch], K_MSEC(TMO_MS)) != 0) {
		return -ETIMEDOUT;
	}
	return dma_rc[ch];
}

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t v;

	if (!device_is_ready(dma_dev)) {
		shell_error(sh, "%s is not ready", dma_dev->name);
		return -ENODEV;
	}
	shell_print(sh, "%s ready", dma_dev->name);
	if (dma_get_attribute(dma_dev, DMA_ATTR_MAX_BLOCK_COUNT, &v) == 0) {
		shell_print(sh, "max chained blocks: %u", v);
	}
	shell_print(sh, "timer: %u Hz", (unsigned int)sys_clock_hw_cycles_per_sec());
	return 0;
}

static int cmd_selftest(const struct shell *sh, size_t argc, char **argv)
{
	size_t kib = argc > 1 ? strtoul(argv[1], NULL, 0) : 64;
	size_t len = kib * 1024;
	struct dma_block_config blk;
	struct dma_status st;
	volatile bool never = false;
	uint32_t t0, t_start, t_dma, t_cpu, calib_us;
	uint32_t iters = 0, calib_iters = 0;
	int err;

	if (kib == 0 || len > BUF_SZ) {
		shell_error(sh, "size must be 1..%u KiB", BUF_SZ / 1024);
		return -EINVAL;
	}

	/* CPU spin rate, to express how much work fits next to a running transfer */
	t0 = k_cycle_get_32();
	while (!never && calib_iters < 1000000) {
		calib_iters++;
	}
	calib_us = cyc_to_us(k_cycle_get_32() - t0);

	/* Dirty cache lines on both buffers: the driver must flush/invalidate them */
	fill_pattern(src_buf, len, 0x1234567 + kib);
	memset(dst_buf, FILL, len);

	blk_init(&blk, src_buf, dst_buf, len);
	err = chan_config(0, &blk, 1);
	if (err) {
		shell_error(sh, "dma_config: %d", err);
		return err;
	}

	t0 = k_cycle_get_32();
	err = dma_start(dma_dev, 0);
	t_start = k_cycle_get_32() - t0;
	if (err) {
		shell_error(sh, "dma_start: %d", err);
		return err;
	}
	while (dma_rc[0] == SENTINEL && k_cyc_to_ms_floor32(k_cycle_get_32() - t0) < TMO_MS) {
		iters++;
	}
	t_dma = k_cycle_get_32() - t0;
	err = dma_rc[0];
	if (err != 0) {
		shell_error(sh, "transfer failed: %d", err);
		return err ? err : -EIO;
	}
	if (dma_get_status(dma_dev, 0, &st) || st.busy || st.total_copied < len) {
		shell_error(sh, "bad status after completion");
		return -EIO;
	}
	if (memcmp(src_buf, dst_buf, len) != 0) {
		for (size_t i = 0; i < len; i++) {
			if (src_buf[i] != dst_buf[i]) {
				shell_error(sh, "MISMATCH at offset %u: %02x != %02x", (unsigned int)i,
					    dst_buf[i], src_buf[i]);
				break;
			}
		}
		return -EIO;
	}

	t0 = k_cycle_get_32();
	memcpy(cpu_buf, src_buf, len);
	t_cpu = k_cycle_get_32() - t0;

	uint32_t us_dma = cyc_to_us(t_dma), us_cpu = cyc_to_us(t_cpu);
	uint32_t cpu_free = (calib_us && us_dma) ?
		(uint32_t)((uint64_t)iters * calib_us * 100 / calib_iters / us_dma) : 0;

	shell_print(sh, "PASS: %u KiB byte-exact", (unsigned int)kib);
	shell_print(sh, "  DMA    %u us (%u MiB/s), dma_start() returned after %u us",
		    us_dma, mib_s(len, us_dma), cyc_to_us(t_start));
	shell_print(sh, "  memcpy %u us (%u MiB/s)", us_cpu, mib_s(len, us_cpu));
	shell_print(sh, "  CPU ran %u loop iterations during the DMA (%u%% of the transfer time free)",
		    iters, cpu_free);
	return 0;
}

static int copy_check(uint32_t ch, size_t so, size_t dof, size_t len)
{
	struct dma_block_config blk;
	uint8_t *dst = dst_buf + GUARD + dof;
	size_t total = GUARD + dof + len + GUARD;
	int err;

	memset(dst_buf, FILL, total);
	blk_init(&blk, src_buf + so, dst, len);
	err = chan_config(ch, &blk, 1);
	if (err == 0) {
		err = dma_start(dma_dev, ch);
	}
	if (err == 0) {
		err = wait_done(ch);
	}
	if (err) {
		return err;
	}
	if (memcmp(src_buf + so, dst, len) != 0) {
		return -EIO;
	}
	for (size_t i = 0; i < GUARD + dof; i++) {
		if (dst_buf[i] != FILL) {
			return -ERANGE;
		}
	}
	for (size_t i = GUARD + dof + len; i < total; i++) {
		if (dst_buf[i] != FILL) {
			return -ERANGE;
		}
	}
	return 0;
}

static int cmd_align(const struct shell *sh, size_t argc, char **argv)
{
	static const size_t fixed[][3] = {
		{0, 0, 1}, {0, 0, 7}, {0, 0, 8}, {0, 0, 9}, {0, 0, 127}, {0, 0, 128}, {0, 0, 129},
		{1, 3, 100}, {3, 1, 100}, {4, 4, 4093}, {5, 2, 4097}, {7, 0, 4096}, {0, 7, 4096},
		{2, 6, 8191}, {1, 1, 65537}, {0, 0, 65536}, {13, 21, 300000},
	};
	uint32_t n = 0, bad = 0;
	uint32_t seed = 0x9e3779b9;
	int err;

	fill_pattern(src_buf, BUF_SZ, 0xdeadbeef);

	for (size_t i = 0; i < ARRAY_SIZE(fixed); i++, n++) {
		err = copy_check(i % NUM_CH, fixed[i][0], fixed[i][1], fixed[i][2]);
		if (err) {
			shell_error(sh, "FAIL src+%u dst+%u len %u: %d", (unsigned int)fixed[i][0],
				    (unsigned int)fixed[i][1], (unsigned int)fixed[i][2], err);
			bad++;
		}
	}
	for (int i = 0; i < 300; i++, n++) {
		seed = seed * 1664525 + 1013904223;
		size_t so = (seed >> 8) & 15, dof = (seed >> 12) & 15;
		size_t len = 1 + ((seed >> 16) % 3000);

		err = copy_check(i % NUM_CH, so, dof, len);
		if (err) {
			shell_error(sh, "FAIL src+%u dst+%u len %u: %d", (unsigned int)so,
				    (unsigned int)dof, (unsigned int)len, err);
			bad++;
		}
	}

	if (bad) {
		shell_error(sh, "%u of %u cases failed", bad, n);
		return -EIO;
	}
	shell_print(sh, "PASS: %u unaligned cases (all 8 channels), guards intact", n);
	return 0;
}

static int cmd_multi(const struct shell *sh, size_t argc, char **argv)
{
	const size_t len = 64 * 1024;
	struct dma_block_config blk[NUM_CH];
	uint32_t t0, us;
	int err;

	fill_pattern(src_buf, NUM_CH * len, 0xabcdef);
	memset(dst_buf, FILL, NUM_CH * len);

	for (uint32_t ch = 0; ch < NUM_CH; ch++) {
		blk_init(&blk[ch], src_buf + ch * len, dst_buf + ch * len, len);
		err = chan_config(ch, &blk[ch], 1);
		if (err) {
			shell_error(sh, "config ch%u: %d", ch, err);
			return err;
		}
	}
	t0 = k_cycle_get_32();
	for (uint32_t ch = 0; ch < NUM_CH; ch++) {
		err = dma_start(dma_dev, ch);
		if (err) {
			shell_error(sh, "start ch%u: %d", ch, err);
			return err;
		}
	}
	for (uint32_t ch = 0; ch < NUM_CH; ch++) {
		err = wait_done(ch);
		if (err) {
			shell_error(sh, "ch%u: %d", ch, err);
			return err;
		}
	}
	us = cyc_to_us(k_cycle_get_32() - t0);
	if (memcmp(src_buf, dst_buf, NUM_CH * len) != 0) {
		shell_error(sh, "data mismatch");
		return -EIO;
	}
	shell_print(sh, "PASS: 8 channels x %u KiB concurrently in %u us (%u MiB/s aggregate)",
		    (unsigned int)(len / 1024), us, mib_s(NUM_CH * len, us));
	return 0;
}

static int cmd_chain(const struct shell *sh, size_t argc, char **argv)
{
	static const size_t sz[3] = {1000, 4096, 77};
	struct dma_block_config blk[3];
	size_t soff = 0, doff = 0;
	int err;

	fill_pattern(src_buf, 8192, 0x5151);
	memset(dst_buf, FILL, sizeof(dst_buf));

	for (int i = 0; i < 3; i++) {
		/* gaps in the destination prove that every block uses its own addresses */
		blk_init(&blk[i], src_buf + soff, dst_buf + doff, sz[i]);
		blk[i].next_block = i < 2 ? &blk[i + 1] : NULL;
		soff += sz[i];
		doff += sz[i] + 100 + i;
	}
	err = chan_config(3, blk, 3);
	if (err == 0) {
		err = dma_start(dma_dev, 3);
	}
	if (err == 0) {
		err = wait_done(3);
	}
	if (err) {
		shell_error(sh, "chain failed: %d", err);
		return err;
	}
	soff = doff = 0;
	for (int i = 0; i < 3; i++) {
		if (memcmp(src_buf + soff, dst_buf + doff, sz[i]) != 0) {
			shell_error(sh, "block %d mismatch", i);
			return -EIO;
		}
		soff += sz[i];
		doff += sz[i] + 100 + i;
	}
	shell_print(sh, "PASS: 3 chained blocks (%u + %u + %u bytes)", (unsigned int)sz[0],
		    (unsigned int)sz[1], (unsigned int)sz[2]);
	return 0;
}

static int cmd_fill(const struct shell *sh, size_t argc, char **argv)
{
	static uint32_t pattern __aligned(32) = 0xc0ffee42;
	struct dma_block_config blk;
	struct dma_config cfg = {
		.channel_direction = MEMORY_TO_MEMORY,
		.source_data_size = sizeof(pattern),
		.dest_data_size = sizeof(pattern),
		.complete_callback_en = 1,
		.block_count = 1,
		.head_block = &blk,
		.dma_callback = dma_cb,
	};
	int err;

	blk_init(&blk, &pattern, dst_buf, 4096);
	blk.source_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;
	memset(dst_buf, FILL, 4096 + GUARD);
	k_sem_init(&dma_sem[5], 0, 1);
	dma_rc[5] = SENTINEL;
	err = dma_config(dma_dev, 5, &cfg);
	if (err == 0) {
		err = dma_start(dma_dev, 5);
	}
	if (err == 0) {
		err = wait_done(5);
	}
	if (err) {
		shell_error(sh, "fill failed: %d", err);
		return err;
	}
	for (size_t i = 0; i < 4096; i += 4) {
		if (*(uint32_t *)&dst_buf[i] != pattern) {
			shell_error(sh, "fill mismatch at %u", (unsigned int)i);
			for (size_t k = 0; k < 16; k++) {
				shell_print(sh, "  dst[%u] = 0x%08x", (unsigned int)k,
					    ((uint32_t *)dst_buf)[k]);
			}
			return -EIO;
		}
	}
	shell_print(sh, "PASS: 4096-byte fill from a fixed source address");
	return 0;
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	struct dma_block_config blk;
	struct dma_status st;
	size_t copied = 0;
	int err;

	fill_pattern(src_buf, BUF_SZ, 0x777);
	memset(dst_buf, FILL, BUF_SZ);

	blk_init(&blk, src_buf, dst_buf + GUARD, BUF_SZ - GUARD);
	err = chan_config(2, &blk, 1);
	if (err == 0) {
		err = dma_start(dma_dev, 2);
	}
	if (err) {
		shell_error(sh, "start: %d", err);
		return err;
	}
	k_busy_wait(300);
	dma_get_status(dma_dev, 2, &st);
	shell_print(sh, "running: busy=%d pending=%u of %u", st.busy, st.pending_length,
		    (unsigned int)(BUF_SZ - GUARD));
	err = dma_stop(dma_dev, 2);
	if (err) {
		shell_error(sh, "stop: %d", err);
		return err;
	}
	dma_get_status(dma_dev, 2, &st);
	if (st.busy) {
		shell_error(sh, "still busy after stop");
		return -EIO;
	}
	for (size_t i = 0; i < BUF_SZ - GUARD; i++) {
		if (dst_buf[GUARD + i] == src_buf[i]) {
			copied++;
		} else {
			break;
		}
	}
	shell_print(sh, "stopped: about %u of %u bytes had been copied", (unsigned int)copied,
		    (unsigned int)(BUF_SZ - GUARD));
	if (copied == BUF_SZ - GUARD) {
		shell_error(sh, "transfer finished before stop (too fast?)");
		return -EIO;
	}

	/* The channel must be reusable straight away */
	blk_init(&blk, src_buf, dst_buf, 64 * 1024);
	err = chan_config(2, &blk, 1);
	if (err == 0) {
		err = dma_start(dma_dev, 2);
	}
	if (err == 0) {
		err = wait_done(2);
	}
	if (err || memcmp(src_buf, dst_buf, 64 * 1024) != 0) {
		shell_error(sh, "restart after stop failed: %d", err);
		return err ? err : -EIO;
	}
	shell_print(sh, "PASS: stop mid-transfer, then a full 64 KiB copy on the same channel");
	return 0;
}

static int cmd_neg(const struct shell *sh, size_t argc, char **argv)
{
	struct dma_block_config blk[4 + 1];
	struct dma_config cfg = {
		.channel_direction = MEMORY_TO_MEMORY,
		.block_count = 1,
		.head_block = &blk[0],
		.dma_callback = dma_cb,
	};
	int bad = 0;

	for (size_t i = 0; i < ARRAY_SIZE(blk); i++) {
		blk_init(&blk[i], src_buf, dst_buf, 64);
		blk[i].next_block = (i + 1 < ARRAY_SIZE(blk)) ? &blk[i + 1] : NULL;
	}

#define EXPECT(call, rc, what)                                                                     \
	do {                                                                                       \
		int r_ = (call);                                                                   \
		if (r_ != (rc)) {                                                                  \
			shell_error(sh, "%s: got %d, expected %d", what, r_, (int)(rc));          \
			bad++;                                                                     \
		}                                                                                  \
	} while (0)

	EXPECT(dma_config(dma_dev, 8, &cfg), -EINVAL, "channel 8 (out of range)");

	cfg.channel_direction = PERIPHERAL_TO_MEMORY;
	EXPECT(dma_config(dma_dev, 0, &cfg), -ENOTSUP, "peripheral-to-memory");
	cfg.channel_direction = MEMORY_TO_MEMORY;

	cfg.block_count = ARRAY_SIZE(blk);
	EXPECT(dma_config(dma_dev, 0, &cfg), -EINVAL, "too many blocks");
	cfg.block_count = 1;

	blk[0].source_gather_en = 1;
	EXPECT(dma_config(dma_dev, 0, &cfg), -ENOTSUP, "gather");
	blk[0].source_gather_en = 0;

	blk[0].block_size = 0;
	EXPECT(dma_config(dma_dev, 0, &cfg), -ENOTSUP, "zero-length block");
	blk[0].block_size = 64;

	blk[0].source_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;
	EXPECT(dma_config(dma_dev, 0, &cfg), -EINVAL, "fixed address without a data size");
	cfg.source_data_size = 3;
	EXPECT(dma_config(dma_dev, 0, &cfg), -EINVAL, "fixed address, odd data size");
	cfg.source_data_size = 4;
	blk[0].block_size = 62;
	EXPECT(dma_config(dma_dev, 0, &cfg), -EINVAL, "length not a multiple of the data size");
	blk[0].block_size = 64;
	EXPECT(dma_config(dma_dev, 0, &cfg), 0, "valid fixed-source config");
#undef EXPECT

	if (bad) {
		return -EIO;
	}
	shell_print(sh, "PASS: invalid configurations are rejected");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	dma_cmds,
	SHELL_CMD(info, NULL, "Controller information", cmd_info),
	SHELL_CMD_ARG(selftest, NULL, "[kib] copy, verify and compare with memcpy", cmd_selftest, 1, 1),
	SHELL_CMD(align, NULL, "Unaligned source/destination/length sweep", cmd_align),
	SHELL_CMD(multi, NULL, "Eight concurrent channels", cmd_multi),
	SHELL_CMD(chain, NULL, "Three chained blocks", cmd_chain),
	SHELL_CMD(fill, NULL, "Fixed source address (pattern fill)", cmd_fill),
	SHELL_CMD(stop, NULL, "Abort a running transfer and reuse the channel", cmd_stop),
	SHELL_CMD(neg, NULL, "Invalid configuration checks", cmd_neg),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(dma, &dma_cmds, "Zynq PS DMAC test commands", NULL);

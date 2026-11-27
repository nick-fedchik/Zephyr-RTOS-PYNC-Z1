/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shell test commands for the Zynq-7000 PS QSPI controller + NOR flash:
 *   qspi id                 - read the JEDEC ID and print the flash geometry
 *   qspi selftest [kib]     - erase, program, read back and verify <kib> KiB (default 64)
 *                             of the "qspi-scratch" partition, then erase it again
 *
 * Only the scratch partition is ever touched; the rest of the flash is read-only
 * by convention (it holds the board's boot image).
 */

#include <stdlib.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/syscon.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#define SCRATCH_NODE DT_NODELABEL(qspi_scratch)
#define SCRATCH_OFF  DT_REG_ADDR(SCRATCH_NODE)
#define SCRATCH_SIZE DT_REG_SIZE(SCRATCH_NODE)
#define MAX_KIB      256

#define SLCR_IO_PLL_CTRL    0x108
#define SLCR_LQSPI_CLK_CTRL 0x14C

static const struct device *const slcr_dev = DEVICE_DT_GET(DT_NODELABEL(slcr));
static const struct device *const flash_dev = DEVICE_DT_GET(DT_NODELABEL(qspi_flash));

static uint8_t wr_buf[MAX_KIB * 1024];
static uint8_t rd_buf[MAX_KIB * 1024];

static int cmd_qspi_id(const struct shell *sh, size_t argc, char **argv)
{
	struct flash_pages_info info;
	uint8_t id[3];
	uint32_t clk, pll;
	int err;

	if (!device_is_ready(flash_dev)) {
		shell_error(sh, "%s is not ready", flash_dev->name);
		return -ENODEV;
	}

	err = flash_read_jedec_id(flash_dev, id);
	if (err) {
		shell_error(sh, "flash_read_jedec_id failed: %d", err);
		return err;
	}

	shell_print(sh, "JEDEC ID: %02x %02x %02x", id[0], id[1], id[2]);
	shell_print(sh, "Size: %u bytes", (unsigned int)DT_PROP(DT_NODELABEL(qspi_flash), size) / 8);
	if (flash_get_page_info_by_offs(flash_dev, SCRATCH_OFF, &info) == 0) {
		shell_print(sh, "Erase sector: %u bytes", (unsigned int)info.size);
	}
	if (syscon_read_reg(slcr_dev, SLCR_LQSPI_CLK_CTRL, &clk) == 0 &&
	    syscon_read_reg(slcr_dev, SLCR_IO_PLL_CTRL, &pll) == 0) {
		/* 50 MHz PS_CLK; SRCSEL[5:4]: 0/1 = IO PLL, 2 = ARM PLL, 3 = DDR PLL (IO PLL shown) */
		shell_print(sh, "LQSPI_CLK_CTRL 0x%08x, IO_PLL_CTRL 0x%08x -> ref %u Hz (DT %u Hz)",
			    clk, pll, (unsigned int)(50000000U * ((pll >> 12) & 0x7f) / ((clk >> 8) & 0x3f)),
			    (unsigned int)DT_PROP(DT_NODELABEL(qspi), clock_frequency));
	}
	shell_print(sh, "Scratch partition: 0x%x + 0x%x", SCRATCH_OFF, SCRATCH_SIZE);

	return 0;
}

static int cmd_qspi_selftest(const struct shell *sh, size_t argc, char **argv)
{
	struct flash_pages_info info;
	size_t kib = argc > 1 ? strtoul(argv[1], NULL, 0) : 64;
	size_t len, erase_len;
	uint32_t seed = 0x1badb002;
	int64_t t0, t_erase, t_write, t_read;
	int err;

	if (!device_is_ready(flash_dev)) {
		shell_error(sh, "%s is not ready", flash_dev->name);
		return -ENODEV;
	}
	if (kib == 0 || kib > MAX_KIB || kib * 1024 > SCRATCH_SIZE) {
		shell_error(sh, "<kib> must be 1..%u", (unsigned int)MIN(MAX_KIB, SCRATCH_SIZE / 1024));
		return -EINVAL;
	}
	len = kib * 1024;

	err = flash_get_page_info_by_offs(flash_dev, SCRATCH_OFF, &info);
	if (err) {
		shell_error(sh, "flash_get_page_info_by_offs failed: %d", err);
		return err;
	}
	erase_len = ROUND_UP(len, info.size);
	if (erase_len > SCRATCH_SIZE) {
		shell_error(sh, "erase length 0x%x exceeds the scratch partition", (unsigned int)erase_len);
		return -EINVAL;
	}

	for (size_t i = 0; i < len; i++) {
		seed = seed * 1664525U + 1013904223U;
		wr_buf[i] = seed >> 24;
	}

	t0 = k_uptime_get();
	err = flash_erase(flash_dev, SCRATCH_OFF, erase_len);
	t_erase = k_uptime_get() - t0;
	if (err) {
		shell_error(sh, "erase failed: %d", err);
		return err;
	}

	err = flash_read(flash_dev, SCRATCH_OFF, rd_buf, len);
	if (err) {
		shell_error(sh, "read-after-erase failed: %d", err);
		return err;
	}
	for (size_t i = 0; i < len; i++) {
		if (rd_buf[i] != 0xff) {
			shell_error(sh, "not blank at +0x%x: 0x%02x", (unsigned int)i, rd_buf[i]);
			return -EIO;
		}
	}

	t0 = k_uptime_get();
	err = flash_write(flash_dev, SCRATCH_OFF, wr_buf, len);
	t_write = k_uptime_get() - t0;
	if (err) {
		shell_error(sh, "write failed: %d", err);
		return err;
	}

	memset(rd_buf, 0, len);
	t0 = k_uptime_get();
	err = flash_read(flash_dev, SCRATCH_OFF, rd_buf, len);
	t_read = k_uptime_get() - t0;
	if (err) {
		shell_error(sh, "read failed: %d", err);
		return err;
	}
	if (memcmp(wr_buf, rd_buf, len) != 0) {
		for (size_t i = 0; i < len; i++) {
			if (wr_buf[i] != rd_buf[i]) {
				shell_error(sh, "mismatch at +0x%x: wrote 0x%02x read 0x%02x",
					    (unsigned int)i, wr_buf[i], rd_buf[i]);
				break;
			}
		}
		return -EIO;
	}

	/* Odd offsets and lengths exercise the 1..3 byte FIFO paths */
	for (size_t off = 0; off < 9; off++) {
		for (size_t n = 1; n < 11; n++) {
			uint8_t small[16];

			err = flash_read(flash_dev, SCRATCH_OFF + off, small, n);
			if (err || memcmp(small, &wr_buf[off], n) != 0) {
				shell_error(sh, "short read mismatch (off %u, len %u, err %d)",
					    (unsigned int)off, (unsigned int)n, err);
				return err ? err : -EIO;
			}
		}
	}

	shell_print(sh, "PASS: %u KiB erase %d ms, program %d ms (%u KiB/s), read %d ms (%u KiB/s)",
		    (unsigned int)kib, (int)t_erase, (int)t_write,
		    (unsigned int)(t_write ? kib * 1000 / t_write : 0), (int)t_read,
		    (unsigned int)(t_read ? kib * 1000 / t_read : 0));

	err = flash_erase(flash_dev, SCRATCH_OFF, erase_len);
	if (err) {
		shell_error(sh, "final erase failed: %d", err);
	}

	return err;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	qspi_cmds,
	SHELL_CMD(id, NULL, "Read the JEDEC ID and print the geometry", cmd_qspi_id),
	SHELL_CMD_ARG(selftest, NULL, "[kib] erase/program/verify the scratch partition",
		      cmd_qspi_selftest, 1, 1),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(qspi, &qspi_cmds, "Zynq QSPI NOR flash test commands", NULL);

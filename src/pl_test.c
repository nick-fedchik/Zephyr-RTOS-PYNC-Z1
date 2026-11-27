/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * PL (FPGA) shell test commands for the Zynq-7000 DevCfg FPGA manager:
 *   pl load <addr> <size>  - program a byte-swapped .bin bitstream from DDR and restore the
 *                            XADC configuration, which the bitstream resets
 *   pl loadfile [path]     - read a .bin bitstream from a FAT microSD (default /SD:/base.bin)
 *                            into the staging window and program the PL
 *   pl status              - report fpga_get_status()
 *   pl led [0-15]          - write/read the PYNQ-Z1 base.bit LD0-LD3 AXI GPIO
 *   pl read <addr>         - 32-bit MMIO read (for PL AXI peripherals)
 */

#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/drivers/fpga.h>
#include <zephyr/drivers/sensor/zynq7000_xadc.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/sys_io.h>

#if defined(CONFIG_FAT_FILESYSTEM_ELM) && DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(sdhc0))
#define PL_HAVE_SD 1
#include <zephyr/fs/fs.h>
#include <ff.h>
#endif

#define AXI_LED_BASE  0x41250000U /* PYNQ-Z1 base.bit LD0-LD3 (4 bits) */
#define AXI_GPIO_DATA 0x00U
#define AXI_GPIO_TRI  0x04U

#define PL_AXI_BASE   0x41200000U
#define PL_AXI_END    0x4125ffffU

#define SD_BITSTREAM_PATH "/SD:/base.bin"
#define SD_READ_CHUNK     (256U * 1024U)

static const struct device *const fpga_dev = DEVICE_DT_GET(DT_NODELABEL(devcfg));

#ifdef PL_HAVE_SD
static FATFS sd_fat;
static struct fs_mount_t sd_mnt = {
	.type = FS_FATFS,
	.fs_data = &sd_fat,
	.mnt_point = "/SD:",
};
#endif

static int pl_program(const struct shell *sh, uint32_t addr, uint32_t size)
{
	uint32_t t0;
	int err;

	if (!device_is_ready(fpga_dev)) {
		shell_error(sh, "%s is not ready", fpga_dev->name);
		return -ENODEV;
	}

	shell_print(sh, "Loading %u bytes from 0x%08x...", size, addr);

	t0 = k_cycle_get_32();
	err = fpga_load(fpga_dev, (uint32_t *)(uintptr_t)addr, size);
	t0 = k_cyc_to_us_near32(k_cycle_get_32() - t0);

	if (err) {
		shell_error(sh, "fpga_load failed: %d", err);
		return err;
	}

	shell_print(sh, "PL programmed in %u us, fpga_get_status()=%d", t0,
		    fpga_get_status(fpga_dev));

	/* The bitstream reloads the XADC registers, so restore the driver configuration */
	if (DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(xadc))) {
		err = zynq7000_xadc_reconfigure(DEVICE_DT_GET(DT_NODELABEL(xadc)));
		shell_print(sh, "XADC reconfigured: %d (alarms disarmed)", err);
	}

	return 0;
}

static int cmd_pl_load(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t addr = strtoul(argv[1], NULL, 0);
	uint32_t size = strtoul(argv[2], NULL, 0);

	return pl_program(sh, addr, size);
}

#ifdef PL_HAVE_SD
static int cmd_pl_loadfile(const struct shell *sh, size_t argc, char **argv)
{
	const char *path = (argc > 1) ? argv[1] : SD_BITSTREAM_PATH;
	uint8_t *dst = (uint8_t *)(uintptr_t)CONFIG_ZYNQ7000_FPGA_STAGING_ADDR;
	struct fs_dirent st;
	struct fs_file_t file;
	uint32_t size = 0U, done = 0U;
	int64_t t0;
	ssize_t n;
	int err;

	err = fs_mount(&sd_mnt);
	if (err != 0) {
		shell_error(sh, "cannot mount the SD card (%d): no card, or not FAT-formatted", err);
		return err;
	}

	err = fs_stat(path, &st);
	if (err != 0) {
		shell_error(sh, "cannot stat %s (%d)%s", path, err,
			    (err == -ENOENT) ? ": file not found" : "");
		goto unmount;
	}

	if (st.type != FS_DIR_ENTRY_FILE) {
		shell_error(sh, "%s is not a regular file", path);
		err = -EISDIR;
		goto unmount;
	}

	if (st.size == 0U || st.size > CONFIG_ZYNQ7000_FPGA_STAGING_SIZE) {
		shell_error(sh, "%s is %zu bytes: must be 1..%u (staging window)", path, st.size,
			    (uint32_t)CONFIG_ZYNQ7000_FPGA_STAGING_SIZE);
		err = -EFBIG;
		goto unmount;
	}

	size = st.size;

	fs_file_t_init(&file);
	err = fs_open(&file, path, FS_O_READ);
	if (err != 0) {
		shell_error(sh, "cannot open %s (%d)", path, err);
		goto unmount;
	}

	shell_print(sh, "Reading %s (%u bytes) into 0x%08x...", path, size,
		    (uint32_t)CONFIG_ZYNQ7000_FPGA_STAGING_ADDR);

	t0 = k_uptime_get();
	while (done < size) {
		n = fs_read(&file, dst + done, MIN(SD_READ_CHUNK, size - done));
		if (n <= 0) {
			shell_error(sh, "read failed at offset %u (%zd)", done, n);
			err = (n < 0) ? (int)n : -EIO;
			break;
		}
		done += n;
	}

	(void)fs_close(&file);
	err = (done == size) ? 0 : err;
	if (err == 0) {
		shell_print(sh, "Read %u bytes in %lld ms", size, k_uptime_get() - t0);
	}

unmount:
	(void)fs_unmount(&sd_mnt);

	if (err != 0) {
		return err;
	}

	return pl_program(sh, CONFIG_ZYNQ7000_FPGA_STAGING_ADDR, size);
}
#endif

static int cmd_pl_status(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "%s: %s", fpga_dev->name,
		    fpga_get_status(fpga_dev) == FPGA_STATUS_ACTIVE ? "ACTIVE (PL DONE)"
								    : "INACTIVE");
	shell_print(sh, "info: %s", fpga_get_info(fpga_dev));

	return 0;
}

static int cmd_pl_led(const struct shell *sh, size_t argc, char **argv)
{
	if (fpga_get_status(fpga_dev) != FPGA_STATUS_ACTIVE) {
		shell_error(sh, "PL is not configured, run 'pl load' first");
		return -ENODEV;
	}

	if (argc > 1) {
		uint32_t val = strtoul(argv[1], NULL, 0) & 0xfU;

		sys_write32(0x0U, AXI_LED_BASE + AXI_GPIO_TRI);
		sys_write32(val, AXI_LED_BASE + AXI_GPIO_DATA);
	}

	shell_print(sh, "LD0-LD3 DATA=0x%x TRI=0x%x",
		    sys_read32(AXI_LED_BASE + AXI_GPIO_DATA) & 0xfU,
		    sys_read32(AXI_LED_BASE + AXI_GPIO_TRI) & 0xfU);

	return 0;
}

static int cmd_pl_read(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t addr = strtoul(argv[1], NULL, 0);

	if (addr < PL_AXI_BASE || addr > PL_AXI_END || (addr & 3U) != 0U) {
		shell_error(sh, "address must be 4-byte aligned in 0x%08x..0x%08x", PL_AXI_BASE,
			    PL_AXI_END);
		return -EINVAL;
	}

	if (fpga_get_status(fpga_dev) != FPGA_STATUS_ACTIVE) {
		shell_error(sh, "PL is not configured, run 'pl load' first");
		return -ENODEV;
	}

	shell_print(sh, "[0x%08x] = 0x%08x", addr, sys_read32(addr));

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_pl,
	SHELL_CMD_ARG(load, NULL, "<addr> <size>: program PL from a .bin in DDR", cmd_pl_load,
		      3, 0),
#ifdef PL_HAVE_SD
	SHELL_CMD_ARG(loadfile, NULL, "[path]: program PL from a .bin on the FAT microSD",
		      cmd_pl_loadfile, 1, 1),
#endif
	SHELL_CMD(status, NULL, "Show PL configuration status", cmd_pl_status),
	SHELL_CMD_ARG(led, NULL, "[0-15]: write/read AXI GPIO LD0-LD3", cmd_pl_led, 1, 1),
	SHELL_CMD_ARG(read, NULL, "<addr>: read a PL AXI register", cmd_pl_read, 2, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(pl, &sub_pl, "Zynq-7000 PL (FPGA) test commands", NULL);

/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shell test commands for the Cadence PS watchdog driver (wdt_cdns):
 *   wdt start <ms> [reset|irq]  - install a timeout and start the watchdog
 *   wdt feed                    - restart the watchdog counter
 *   wdt stop                    - disable the watchdog
 *   wdt run <ms> <seconds>      - feed at ms/4 for <seconds>, then stop
 *   wdt hang                    - stop feeding and print the time since the last feed
 *   wdt cause [clear]           - print (and optionally clear) the SLCR REBOOT_STATUS reset flags
 */

#include <stdlib.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/syscon.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#define SLCR_REBOOT_STATUS 0x258

static const struct device *const wdt_dev = DEVICE_DT_GET(DT_NODELABEL(watchdog0));
static const struct device *const slcr_dev = DEVICE_DT_GET(DT_NODELABEL(slcr));

static int wdt_channel = -1;
static int64_t last_feed_ms;
static volatile uint32_t irq_count;

static void wdt_irq_cb(const struct device *dev, int channel_id)
{
	irq_count++;
	printk("[wdt irq] channel %d, %d ms since last feed\n", channel_id,
	       (int)(k_uptime_get() - last_feed_ms));
	wdt_feed(dev, channel_id);
	last_feed_ms = k_uptime_get();
}

static int wdt_start(const struct shell *sh, uint32_t ms, bool irq_mode)
{
	struct wdt_timeout_cfg cfg = {
		.window.max = ms,
		.flags = irq_mode ? WDT_FLAG_RESET_NONE : WDT_FLAG_RESET_SOC,
		.callback = irq_mode ? wdt_irq_cb : NULL,
	};
	int err;

	if (!device_is_ready(wdt_dev)) {
		shell_error(sh, "%s is not ready", wdt_dev->name);
		return -ENODEV;
	}

	wdt_channel = wdt_install_timeout(wdt_dev, &cfg);
	if (wdt_channel < 0) {
		shell_error(sh, "wdt_install_timeout(%u ms) failed: %d", ms, wdt_channel);
		return wdt_channel;
	}

	err = wdt_setup(wdt_dev, 0);
	if (err) {
		shell_error(sh, "wdt_setup failed: %d", err);
		return err;
	}

	last_feed_ms = k_uptime_get();
	shell_print(sh, "watchdog started: %u ms, %s, uptime %lld ms", ms,
		    irq_mode ? "interrupt" : "reset", last_feed_ms);

	return 0;
}

static int cmd_wdt_start(const struct shell *sh, size_t argc, char **argv)
{
	bool irq_mode = argc > 2 && strcmp(argv[2], "irq") == 0;

	return wdt_start(sh, strtoul(argv[1], NULL, 0), irq_mode);
}

static int cmd_wdt_feed(const struct shell *sh, size_t argc, char **argv)
{
	int err = wdt_feed(wdt_dev, wdt_channel);

	last_feed_ms = k_uptime_get();
	if (err) {
		shell_error(sh, "wdt_feed failed: %d", err);
	}

	return err;
}

static int cmd_wdt_stop(const struct shell *sh, size_t argc, char **argv)
{
	int err = wdt_disable(wdt_dev);

	if (err) {
		shell_error(sh, "wdt_disable failed: %d", err);
		return err;
	}

	shell_print(sh, "watchdog stopped, uptime %lld ms", k_uptime_get());

	return 0;
}

static int cmd_wdt_run(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t ms = strtoul(argv[1], NULL, 0);
	uint32_t seconds = strtoul(argv[2], NULL, 0);
	int64_t end;
	int err;

	err = wdt_start(sh, ms, false);
	if (err) {
		return err;
	}

	end = k_uptime_get() + (int64_t)seconds * 1000;
	while (k_uptime_get() < end) {
		k_msleep(ms / 4);
		wdt_feed(wdt_dev, wdt_channel);
		last_feed_ms = k_uptime_get();
	}

	shell_print(sh, "survived %u s with a %u ms watchdog, uptime %lld ms", seconds, ms,
		    k_uptime_get());

	return wdt_disable(wdt_dev);
}

static int cmd_wdt_hang(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "not feeding, last feed at %lld ms (output stops at reset)", last_feed_ms);

	while (true) {
		printk("hang: +%d ms since last feed\n", (int)(k_uptime_get() - last_feed_ms));
		k_msleep(20);
	}

	return 0;
}

static int cmd_wdt_cause(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t val;
	int err = syscon_read_reg(slcr_dev, SLCR_REBOOT_STATUS, &val);

	if (err) {
		shell_error(sh, "syscon_read_reg failed: %d", err);
		return err;
	}

	shell_print(sh, "SLCR REBOOT_STATUS = 0x%08x (bit 16 = SWDT reset), wdt irq count = %u", val,
		    irq_count);

	if (argc > 1 && strcmp(argv[1], "clear") == 0) {
		/* Keep the software state field [31:24], clear the persistent reset-cause flags */
		err = syscon_write_reg(slcr_dev, SLCR_REBOOT_STATUS, val & 0xff000000U);
		if (err) {
			shell_error(sh, "syscon_write_reg failed: %d", err);
		}
	}

	return err;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	wdt_cmds,
	SHELL_CMD_ARG(start, NULL, "<ms> [reset|irq]", cmd_wdt_start, 2, 1),
	SHELL_CMD(feed, NULL, "Feed the watchdog", cmd_wdt_feed),
	SHELL_CMD(stop, NULL, "Disable the watchdog", cmd_wdt_stop),
	SHELL_CMD_ARG(run, NULL, "<ms> <seconds>", cmd_wdt_run, 3, 0),
	SHELL_CMD(hang, NULL, "Stop feeding and print elapsed time", cmd_wdt_hang),
	SHELL_CMD_ARG(cause, NULL, "[clear]", cmd_wdt_cause, 1, 1),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(wdt, &wdt_cmds, "Cadence PS watchdog test commands", NULL);

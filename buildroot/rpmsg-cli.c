/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Minimal CLI client for /dev/rpmsg0 (Linux CPU0 <-> Zephyr CPU1 AMP).
 * Usage: rpmsg-cli <command> [args...]
 * Example: rpmsg-cli "blink 150"
 *          rpmsg-cli jitter
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/rpmsg0";
	char tx[256] = { 0 };
	char rx[512] = { 0 };
	size_t off = 0;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s <cmd> [args...]\n", argv[0]);
		return 1;
	}

	for (int i = 1; i < argc && off + 2 < sizeof(tx); i++) {
		int n = snprintf(tx + off, sizeof(tx) - off, "%s%s",
				 (i > 1) ? " " : "", argv[i]);
		if (n > 0)
			off += (size_t)n;
	}

	int fd = open(dev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open(%s): %s\n", dev, strerror(errno));
		return 1;
	}

	double t0 = now_us();
	if (write(fd, tx, off) < 0) {
		fprintf(stderr, "write(%s): %s\n", dev, strerror(errno));
		close(fd);
		return 1;
	}

	ssize_t r = read(fd, rx, sizeof(rx) - 1);
	double dt = now_us() - t0;
	close(fd);

	if (r < 0) {
		fprintf(stderr, "read(%s): %s\n", dev, strerror(errno));
		return 1;
	}
	rx[r] = '\0';
	while (r > 0 && (rx[r - 1] == '\n' || rx[r - 1] == '\r'))
		rx[--r] = '\0';

	printf("%s (%.1f us)\n", rx, dt);
	return 0;
}

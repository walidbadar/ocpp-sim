/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: Apache-2.0
 *
 * native_sim only: command line options and the host's clock.
 */
#ifndef OCPP_SIM_APP_HOST_H_
#define OCPP_SIM_APP_HOST_H_

#include <errno.h>
#include <stdint.h>

#include <zephyr/toolchain.h>

#if defined(CONFIG_BOARD_NATIVE_SIM)

/* Set the clock and apply --url, --id and --page; after config_init() */
void host_init(void);

/* On the main thread, every loop: takes the screenshots that are due */
void host_poll(int64_t now_ms);

/* Write the screen to a binary PPM at the next host_poll(). Any thread. */
int host_request_shot(const char *path);

/* Touch the display at x, y for a moment, as the mouse would. Any thread. */
int host_tap(int x, int y);

#else

static inline void host_init(void)
{
}

static inline void host_poll(int64_t now_ms)
{
	ARG_UNUSED(now_ms);
}

static inline int host_request_shot(const char *path)
{
	ARG_UNUSED(path);

	return -ENOTSUP;
}

static inline int host_tap(int x, int y)
{
	ARG_UNUSED(x);
	ARG_UNUSED(y);

	return -ENOTSUP;
}

#endif /* CONFIG_BOARD_NATIVE_SIM */

#endif /* OCPP_SIM_APP_HOST_H_ */

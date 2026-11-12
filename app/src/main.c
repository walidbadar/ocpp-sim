/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: Apache-2.0
 *
 * OCPP 1.6 charge point simulator: Zephyr's OCPP library, simulated
 * connectors and an LVGL front end to plug in, authorise and charge.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <lvgl.h>

#include "config.h"
#include "cp.h"
#include "host.h"
#include "ui.h"
#include "wifi.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define REFRESH_MS 200

int main(void)
{
	const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	int64_t next_refresh = 0;

	if (!device_is_ready(display)) {
		LOG_ERR("display not ready");
		return 0;
	}

	(void)config_init();
	host_init();
	wifi_init();

	LOG_INF("OCPP 1.6 charge point simulator, %d connectors", CP_CONNECTORS);
	cp_init();

	ui_init();
	ui_refresh();
	lv_timer_handler();
	display_blanking_off(display);

	for (;;) {
		int64_t now = k_uptime_get();
		uint32_t idle;

		if (now >= next_refresh) {
			next_refresh = now + REFRESH_MS;
			ui_refresh();
		}

		idle = lv_timer_handler();
		host_poll(now);

		k_msleep(CLAMP(idle, 5, 20));
	}

	return 0;
}

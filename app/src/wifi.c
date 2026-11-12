/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: Apache-2.0
 */

#include "wifi.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_credentials.h>
#include <zephyr/net/wifi_mgmt.h>

LOG_MODULE_REGISTER(wifi, LOG_LEVEL_INF);

BUILD_ASSERT(IS_ENABLED(CONFIG_WIFI_CREDENTIALS_CONNECT_STORED),
	     "the network comes from the wifi_credentials library");
BUILD_ASSERT(IS_ENABLED(CONFIG_NET_MGMT_EVENT_INFO), "the connect result comes with the event");

/* Between two attempts: after a failure, a lost link or without a network */
#define RETRY K_SECONDS(5)

static struct k_work_delayable connect_work;
static struct net_mgmt_event_callback wifi_cb;

static void connect(struct k_work *work)
{
	static bool told;
	struct net_if *iface = net_if_get_wifi_sta();
	int ret;

	ARG_UNUSED(work);

	if (iface == NULL) {
		LOG_ERR("no Wi-Fi station interface");
		return;
	}

	/* a network of the build is not in the store */
	if (!IS_ENABLED(CONFIG_WIFI_CREDENTIALS_STATIC) && wifi_credentials_is_empty()) {
		if (!told) {
			told = true;
			LOG_WRN("no Wi-Fi network: build with "
				"CONFIG_WIFI_CREDENTIALS_STATIC_SSID and _PASSWORD");
		}
	} else {
		ret = net_mgmt(NET_REQUEST_WIFI_CONNECT_STORED, iface, NULL, 0);
		if (ret == 0) {
			/* the result comes as an event */
			return;
		}
		LOG_WRN("connect request: %d", ret);
	}

	(void)k_work_reschedule(&connect_work, RETRY);
}

static void on_event(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
	const struct wifi_status *status = cb->info;

	ARG_UNUSED(iface);

	if (event == NET_EVENT_WIFI_CONNECT_RESULT && status->status == 0) {
		/* the driver starts DHCP */
		LOG_INF("connected");
		(void)k_work_cancel_delayable(&connect_work);
		return;
	}

	LOG_WRN("%s, trying again",
		event == NET_EVENT_WIFI_CONNECT_RESULT ? "connect failed" : "disconnected");
	(void)k_work_reschedule(&connect_work, RETRY);
}

void wifi_init(void)
{
	k_work_init_delayable(&connect_work, connect);

	net_mgmt_init_event_callback(&wifi_cb, on_event,
				     NET_EVENT_WIFI_CONNECT_RESULT |
					     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_cb);

	(void)k_work_schedule(&connect_work, K_NO_WAIT);
}

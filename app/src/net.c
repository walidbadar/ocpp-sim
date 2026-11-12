/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: Apache-2.0
 */

#include "net.h"

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/clock.h>

#if defined(CONFIG_NET_CONNECTION_MANAGER)
#include <zephyr/net/conn_mgr_monitor.h>
#endif
#if defined(CONFIG_SNTP)
#include <zephyr/net/sntp.h>
#endif

LOG_MODULE_REGISTER(net, LOG_LEVEL_INF);

#if defined(CONFIG_NET_CONNECTION_MANAGER)

static K_SEM_DEFINE(l4_up, 0, 1);
static struct net_mgmt_event_callback l4_cb;
static bool l4_registered;

static void l4_handler(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	if (event == NET_EVENT_L4_CONNECTED) {
		k_sem_give(&l4_up);
	}
}

int net_wait_ready(k_timeout_t timeout)
{
	if (!l4_registered) {
		l4_registered = true;
		net_mgmt_init_event_callback(&l4_cb, l4_handler, NET_EVENT_L4_CONNECTED);
		net_mgmt_add_event_callback(&l4_cb);
		conn_mgr_mon_resend_status();
	}

	return k_sem_take(&l4_up, timeout) == 0 ? 0 : -ETIMEDOUT;
}

#else

/* Offloaded sockets (native_sim) and static addresses: nothing to wait for */
int net_wait_ready(k_timeout_t timeout)
{
	ARG_UNUSED(timeout);

	return 0;
}

#endif /* CONFIG_NET_CONNECTION_MANAGER */

int net_resolve(const char *host, char *ip, size_t len)
{
	struct zsock_addrinfo hints = {
		.ai_family = NET_AF_INET,
		.ai_socktype = NET_SOCK_STREAM,
	};
	struct zsock_addrinfo *res = NULL;
	struct net_in_addr addr;
	int ret;

	/* an address needs no resolver */
	if (zsock_inet_pton(NET_AF_INET, host, &addr) == 1) {
		if (strlen(host) >= len) {
			return -ENAMETOOLONG;
		}
		strcpy(ip, host);
		return 0;
	}

	ret = zsock_getaddrinfo(host, NULL, &hints, &res);
	if (ret != 0 || res == NULL) {
		LOG_WRN("cannot resolve %s (%d)", host, ret);
		return -EHOSTUNREACH;
	}

	if (zsock_inet_ntop(NET_AF_INET, &net_sin(res->ai_addr)->sin_addr, ip, len) == NULL) {
		ret = -ENAMETOOLONG;
	}
	zsock_freeaddrinfo(res);

	return ret;
}

void net_sync_time(void)
{
#if defined(CONFIG_SNTP)
	struct sntp_time now;
	struct timespec ts;
	int ret;

	ret = sntp_simple(CONFIG_OCPP_SIM_APP_SNTP_SERVER, 4000, &now);
	if (ret < 0) {
		LOG_WRN("SNTP %s: %d, timestamps start at 1970", CONFIG_OCPP_SIM_APP_SNTP_SERVER,
			ret);
		return;
	}

	ts.tv_sec = now.seconds;
	ts.tv_nsec = 0;
	sys_clock_settime(SYS_CLOCK_REALTIME, &ts);
	LOG_INF("time set from %s", CONFIG_OCPP_SIM_APP_SNTP_SERVER);
#endif
}

/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef OCPP_SIM_APP_NET_H_
#define OCPP_SIM_APP_NET_H_

#include <stddef.h>

#include <zephyr/kernel.h>

/** Wait until the network is usable. 0, or -ETIMEDOUT. */
int net_wait_ready(k_timeout_t timeout);

/** IPv4 address of @p host (a name or an address) as text. 0 or -errno. */
int net_resolve(const char *host, char *ip, size_t len);

/** Set the clock the OCPP timestamps come from, where the board has none. */
void net_sync_time(void);

#endif /* OCPP_SIM_APP_NET_H_ */

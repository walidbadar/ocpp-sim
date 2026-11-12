/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: Apache-2.0
 *
 * Two things the OCPP library knows but has no API for. With
 * CONFIG_OCPP_SIM_APP_PEEK they are read from its structures (ocpp_peek.c),
 * without it the callers get "unknown".
 */
#ifndef OCPP_SIM_APP_OCPP_PEEK_H_
#define OCPP_SIM_APP_OCPP_PEEK_H_

#include <zephyr/net/ocpp.h>

enum ocpp_peek_link {
	OCPP_PEEK_UNKNOWN,
	OCPP_PEEK_OFFLINE, /* no WebSocket to the central system */
	OCPP_PEEK_BOOTING, /* connected, BootNotification not accepted */
	OCPP_PEEK_ONLINE,
};

#if defined(CONFIG_OCPP_SIM_APP_PEEK)

/** Link state as the library sees it; @p heartbeat_s is set when on line. */
enum ocpp_peek_link ocpp_peek_link(ocpp_session_handle_t session, int *heartbeat_s);

/** Transaction id the central system gave to the session, -1 if none. */
int ocpp_peek_txn_id(ocpp_session_handle_t session);

#else

static inline enum ocpp_peek_link ocpp_peek_link(ocpp_session_handle_t session, int *heartbeat_s)
{
	ARG_UNUSED(session);
	ARG_UNUSED(heartbeat_s);

	return OCPP_PEEK_UNKNOWN;
}

static inline int ocpp_peek_txn_id(ocpp_session_handle_t session)
{
	ARG_UNUSED(session);

	return -1;
}

#endif /* CONFIG_OCPP_SIM_APP_PEEK */

#endif /* OCPP_SIM_APP_OCPP_PEEK_H_ */

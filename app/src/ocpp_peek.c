/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: Apache-2.0
 *
 * The only file that depends on the layout of the OCPP library's private
 * structures (subsys/net/lib/ocpp/ocpp_i.h). The fields are read without
 * the library's lock: they are a flag and two integers, and a stale value
 * only delays the display by one refresh.
 */

#include "ocpp_peek.h"

#include "ocpp_i.h"

enum ocpp_peek_link ocpp_peek_link(ocpp_session_handle_t session, int *heartbeat_s)
{
	struct ocpp_session *sh = session;
	struct ocpp_info *ctx;

	if (sh == NULL || sh->ctx == NULL) {
		return OCPP_PEEK_UNKNOWN;
	}
	ctx = sh->ctx;

	if (ctx->is_cs_offline) {
		return OCPP_PEEK_OFFLINE;
	}
	if (ctx->state < CP_STATE_READY) {
		return OCPP_PEEK_BOOTING;
	}
	if (heartbeat_s != NULL) {
		*heartbeat_s = ctx->hb_sec;
	}

	return OCPP_PEEK_ONLINE;
}

int ocpp_peek_txn_id(ocpp_session_handle_t session)
{
	struct ocpp_session *sh = session;

	return sh != NULL ? sh->idtxn : -1;
}

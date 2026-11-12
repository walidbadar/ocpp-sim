/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: Apache-2.0
 *
 * The charge point: the simulated connectors (evse.c) joined to Zephyr's
 * OCPP 1.6 library. The display and the shell call in here; the
 * requests to the central system are made by a thread of this module, so
 * none of these functions waits for the network.
 */
#ifndef OCPP_SIM_APP_CP_H_
#define OCPP_SIM_APP_CP_H_

#include <stdbool.h>
#include <stdint.h>

#include "evse.h"
#include "url.h"

#define CP_CONNECTORS CONFIG_OCPP_SIM_APP_CONNECTORS

/** Where the connection to the central system stands. */
enum cp_link {
	CP_LINK_UNCONFIGURED, /**< no usable backend URL */
	CP_LINK_NO_NETWORK,   /**< waiting for an IP address */
	CP_LINK_RESOLVING,    /**< looking up the backend's address */
	CP_LINK_CONNECTING,   /**< WebSocket not (or no longer) open */
	CP_LINK_BOOTING,      /**< connected, BootNotification not accepted yet */
	CP_LINK_ONLINE,       /**< BootNotification accepted */
};

struct cp_status {
	enum cp_link link;
	int heartbeat_s;      /**< interval from the central system, 0 if unknown */
	struct backend_url url; /**< backend in use */
	char ws_path[BACKEND_URL_PATH_MAX + 64];
	char ip[48];          /**< address the host resolved to */
};

/** What the display shows of one connector. */
struct cp_connector {
	enum evse_status status;
	enum evse_auth auth;
	enum evse_txn txn;
	enum evse_stop_reason stop_reason;
	bool ev_plugged;
	bool ev_ready;
	bool evse_ready;
	bool faulted;
	bool inoperative;
	char idtag[EVSE_IDTAG_MAX + 1]; /**< of the session, or the last one */
	int32_t txn_id;                  /**< -1 without transaction or if unknown */
	int32_t last_txn_id;             /**< of the running or the last transaction */
	uint32_t power_w;
	uint32_t energy_wh;
	uint32_t session_wh;
	uint32_t session_s;
	uint32_t current_ma;
	uint32_t offered_ma;
	uint32_t limit_w;
	uint32_t max_power_w;
	uint16_t voltage_v;
};

/** Set the connectors up and start the thread that talks to the backend. */
int cp_init(void);

void cp_get_status(struct cp_status *status);
/** @p connector is 1..CP_CONNECTORS in all functions below. */
void cp_get_connector(int connector, struct cp_connector *view);

void cp_set_plugged(int connector, bool plugged);
void cp_set_ev_ready(int connector, bool ready);
void cp_set_evse_ready(int connector, bool ready);
void cp_set_fault(int connector, bool faulted);
void cp_set_inoperative(int connector, bool inoperative);
void cp_set_limit(int connector, uint32_t limit_w);

/**
 * Present an identifier; NULL takes the one configured for the connector.
 * See evse_present_tag() for what that does and returns.
 */
int cp_present_tag(int connector, const char *idtag);

/** Stop button: ends the transaction or withdraws the identifier. */
int cp_stop(int connector);

const char *cp_link_str(enum cp_link link);

#endif /* OCPP_SIM_APP_CP_H_ */

/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: Apache-2.0
 *
 * Threads that come through here:
 *
 *   main (LVGL), shell   inputs and readings, under "lock", never the network
 *   cp (this file)       takes the requests the connector models hand out and
 *                        makes them with ocpp_authorize() and friends, which
 *                        block until the central system answers
 *   OCPP library         notify(): remote start/stop, unlock, meter readings
 *
 * "lock" is never held across a call into the OCPP library.
 */

#include "cp.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/ocpp.h>
#include <zephyr/version.h>

#include "config.h"
#include "net.h"
#include "ocpp_peek.h"

LOG_MODULE_REGISTER(cp, LOG_LEVEL_INF);

#define REQUEST_TIMEOUT_MS CONFIG_OCPP_SIM_APP_REQUEST_TIMEOUT_MS

/* The OCPP library of Zephyr 4.5 keeps the identifier in 20 bytes, which
 * leaves one of 20 characters without its terminator: the StartTransaction
 * then goes out malformed. Later versions have room for it.
 */
#if KERNEL_VERSION_NUMBER < ZEPHYR_VERSION(4, 5, 99)
#define LIB_IDTAG_MAX (EVSE_IDTAG_MAX - 1)
#else
#define LIB_IDTAG_MAX EVSE_IDTAG_MAX
#endif
#define POLL_MS            250
#define RETRY_MS           5000

static struct evse evse[CP_CONNECTORS];
static ocpp_session_handle_t session[CP_CONNECTORS];
static struct cp_status link_status;
static bool ocpp_up;

static K_MUTEX_DEFINE(lock);
static K_SEM_DEFINE(wake, 0, 1);

static K_THREAD_STACK_DEFINE(cp_stack, CONFIG_OCPP_SIM_APP_THREAD_STACK_SIZE);
static struct k_thread cp_thread_data;

static bool valid(int connector)
{
	return connector >= 1 && connector <= CP_CONNECTORS;
}

const char *cp_link_str(enum cp_link link)
{
	switch (link) {
	case CP_LINK_UNCONFIGURED:
		return "No backend";
	case CP_LINK_NO_NETWORK:
		return "No network";
	case CP_LINK_RESOLVING:
		return "Resolving";
	case CP_LINK_CONNECTING:
		return "Connecting";
	case CP_LINK_BOOTING:
		return "Booting";
	case CP_LINK_ONLINE:
		return "Online";
	default:
		return "?";
	}
}

static void set_link(enum cp_link link)
{
	enum cp_link before;

	k_mutex_lock(&lock, K_FOREVER);
	before = link_status.link;
	link_status.link = link;
	k_mutex_unlock(&lock);

	if (before == link) {
		return;
	}

	switch (link) {
	case CP_LINK_ONLINE:
		LOG_INF("BootNotification accepted, heartbeat every %d s",
			  link_status.heartbeat_s);
		break;
	case CP_LINK_BOOTING:
		LOG_INF("Connected, waiting for the BootNotification answer");
		break;
	case CP_LINK_CONNECTING:
		if (before == CP_LINK_ONLINE || before == CP_LINK_BOOTING) {
			LOG_WRN("Connection to the central system lost");
		}
		break;
	default:
		break;
	}
}

/* ---- inputs and readings (any thread) ---------------------------------- */

void cp_get_status(struct cp_status *status)
{
	k_mutex_lock(&lock, K_FOREVER);
	*status = link_status;
	k_mutex_unlock(&lock);
}

void cp_get_connector(int connector, struct cp_connector *view)
{
	struct evse *e;

	memset(view, 0, sizeof(*view));
	view->txn_id = -1;
	view->last_txn_id = -1;
	if (!valid(connector)) {
		return;
	}
	e = &evse[connector - 1];

	k_mutex_lock(&lock, K_FOREVER);
	evse_advance(e, k_uptime_get());
	view->status = evse_status(e);
	view->auth = e->auth;
	view->txn = e->txn;
	view->stop_reason = e->stop_reason;
	view->ev_plugged = e->ev_plugged;
	view->ev_ready = e->ev_ready;
	view->evse_ready = e->evse_ready;
	view->faulted = e->faulted;
	view->inoperative = e->inoperative;
	strcpy(view->idtag, e->idtag);
	view->txn_id = e->txn_id;
	view->last_txn_id = e->last_txn_id;
	view->power_w = e->power_w;
	view->energy_wh = evse_energy_wh(e);
	view->session_wh = evse_session_wh(e);
	view->session_s = evse_session_s(e);
	view->current_ma = evse_current_ma(e);
	view->offered_ma = evse_offered_ma(e);
	view->limit_w = e->limit_w;
	view->max_power_w = e->cfg.max_power_w;
	view->voltage_v = evse_voltage_v(e);
	k_mutex_unlock(&lock);
}

#define CP_INPUT(name, type, call)                                                                 \
	void name(int connector, type value)                                                       \
	{                                                                                          \
		if (!valid(connector)) {                                                           \
			return;                                                                    \
		}                                                                                  \
		k_mutex_lock(&lock, K_FOREVER);                                                    \
		call(&evse[connector - 1], value, k_uptime_get());                                 \
		k_mutex_unlock(&lock);                                                             \
		k_sem_give(&wake);                                                                 \
	}

CP_INPUT(cp_set_plugged, bool, evse_set_plugged)
CP_INPUT(cp_set_ev_ready, bool, evse_set_ev_ready)
CP_INPUT(cp_set_evse_ready, bool, evse_set_evse_ready)
CP_INPUT(cp_set_fault, bool, evse_set_fault)
CP_INPUT(cp_set_inoperative, bool, evse_set_inoperative)
CP_INPUT(cp_set_limit, uint32_t, evse_set_limit)

int cp_present_tag(int connector, const char *idtag)
{
	struct app_config cfg;
	int ret;

	if (!valid(connector)) {
		return -EINVAL;
	}
	if (idtag == NULL) {
		config_get(&cfg);
		idtag = cfg.idtag[connector - 1];
	}

	k_mutex_lock(&lock, K_FOREVER);
	ret = evse_present_tag(&evse[connector - 1], idtag, k_uptime_get());
	k_mutex_unlock(&lock);
	k_sem_give(&wake);

	if (ret == -EBUSY) {
		LOG_WRN("C%d: %s ignored, the connector is busy", connector, idtag);
	} else if (ret == -EACCES) {
		LOG_WRN("C%d: %s ignored, the connector is not in service", connector, idtag);
	}

	return ret;
}

int cp_stop(int connector)
{
	int ret;

	if (!valid(connector)) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);
	ret = evse_stop(&evse[connector - 1], EVSE_STOP_LOCAL, k_uptime_get());
	k_mutex_unlock(&lock);
	k_sem_give(&wake);

	return ret;
}

/* ---- OCPP library callback --------------------------------------------- */

static int meter_value(int connector, enum ocpp_meter_measurand measurand, char *val)
{
	struct evse *e;
	int ret = 0;

	if (!valid(connector)) {
		return -EINVAL;
	}
	e = &evse[connector - 1];

	k_mutex_lock(&lock, K_FOREVER);
	evse_advance(e, k_uptime_get());

	if (e->txn != EVSE_TXN_ACTIVE) {
		/* the library also asks for sessions that are only authorised */
		ret = -ENODATA;
	} else {
		switch (measurand) {
		case OCPP_OMM_ACTIVE_ENERGY_TO_EV:
			snprintf(val, CISTR50, "%u", evse_energy_wh(e));
			break;
		case OCPP_OMM_ACTIVE_POWER_TO_EV:
			snprintf(val, CISTR50, "%u", e->power_w);
			break;
		case OCPP_OMM_CURRENT_TO_EV: {
			uint32_t ma = evse_current_ma(e);

			snprintf(val, CISTR50, "%u.%u", ma / 1000, (ma % 1000) / 100);
			break;
		}
		case OCPP_OMM_VOLTAGE_AC_RMS:
			snprintf(val, CISTR50, "%u", evse_voltage_v(e));
			break;
		default:
			ret = -ENOTSUP;
			break;
		}
	}
	k_mutex_unlock(&lock);

	return ret;
}

static int remote_start(int connector, const char *idtag)
{
	int ret = -EBUSY;

	k_mutex_lock(&lock, K_FOREVER);
	if (!valid(connector)) {
		/* no connector named: the first one that is free, cable in first */
		connector = 0;
		for (int pass = 0; pass < 2 && connector == 0; pass++) {
			for (int i = 0; i < CP_CONNECTORS; i++) {
				if (evse_is_idle(&evse[i]) &&
				    (pass == 1 || evse[i].ev_plugged)) {
					connector = i + 1;
					break;
				}
			}
		}
	}
	if (valid(connector)) {
		ret = evse_remote_start(&evse[connector - 1], idtag, k_uptime_get());
	}
	k_mutex_unlock(&lock);

	if (ret == 0) {
		LOG_INF("C%d: RemoteStartTransaction %s", connector, idtag);
	} else {
		LOG_WRN("RemoteStartTransaction %s: no free connector (%d)", idtag, ret);
	}
	k_sem_give(&wake);

	return ret;
}

static int remote_stop(int connector, bool unlock)
{
	int ret;

	if (!valid(connector)) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);
	ret = evse_stop(&evse[connector - 1], EVSE_STOP_REMOTE, k_uptime_get());
	k_mutex_unlock(&lock);

	LOG_INF("C%d: %s%s", connector, unlock ? "UnlockConnector" : "RemoteStopTransaction",
		  ret == 0 ? "" : ", nothing to stop");
	k_sem_give(&wake);

	return unlock ? 0 : ret;
}

static int notify(enum ocpp_notify_reason reason, union ocpp_io_value *io, void *user_data)
{
	ARG_UNUSED(user_data);

	switch (reason) {
	case OCPP_USR_GET_METER_VALUE:
		return meter_value(io->meter_val.id_con, io->meter_val.mes, io->meter_val.val);
	case OCPP_USR_START_CHARGING:
		return remote_start(io->start_charge.id_con, io->start_charge.idtag);
	case OCPP_USR_STOP_CHARGING:
		return remote_stop(io->stop_charge.id_con, false);
	case OCPP_USR_UNLOCK_CONNECTOR:
		/* a running transaction ends before the cable is released */
		return remote_stop(io->unlock_con.id_con, true);
	default:
		return -ENOTSUP;
	}
}

/* ---- requests to the central system (cp thread) ------------------------ */

static const char *auth_status_str(enum ocpp_auth_status status)
{
	switch (status) {
	case OCPP_AUTH_ACCEPTED:
		return "Accepted";
	case OCPP_AUTH_BLOCKED:
		return "Blocked";
	case OCPP_AUTH_EXPIRED:
		return "Expired";
	case OCPP_AUTH_CONCURRENT_TX:
		return "ConcurrentTx";
	default:
		return "Invalid";
	}
}

static bool reachable(void)
{
	enum cp_link link;

	if (!ocpp_up) {
		return false;
	}

	k_mutex_lock(&lock, K_FOREVER);
	link = link_status.link;
	k_mutex_unlock(&lock);

	/* without the peek the state is a guess: let the library decide */
	return link == CP_LINK_ONLINE || !IS_ENABLED(CONFIG_OCPP_SIM_APP_PEEK);
}

static void do_authorize(int connector)
{
	struct evse *e = &evse[connector - 1];
	enum ocpp_auth_status status = OCPP_AUTH_INVALID;
	enum evse_result result = EVSE_RESULT_FAILED;
	char idtag[EVSE_IDTAG_MAX + 1];
	int ret = -ENOTCONN;

	k_mutex_lock(&lock, K_FOREVER);
	strcpy(idtag, e->idtag);
	k_mutex_unlock(&lock);

	if (strlen(idtag) > LIB_IDTAG_MAX) {
		LOG_WRN("C%d: %s is longer than the %d characters the OCPP library of this "
			"Zephyr can send",
			connector, idtag, LIB_IDTAG_MAX);
		result = EVSE_RESULT_REJECTED;
	} else {
		if (reachable()) {
			ret = ocpp_authorize(session[connector - 1], idtag, &status,
					     REQUEST_TIMEOUT_MS);
		}

		if (ret < 0) {
			LOG_WRN("C%d: Authorize %s: no answer (%d)", connector, idtag, ret);
		} else {
			LOG_INF("C%d: Authorize %s: %s", connector, idtag,
				auth_status_str(status));
			result = status == OCPP_AUTH_ACCEPTED ? EVSE_RESULT_ACCEPTED
							      : EVSE_RESULT_REJECTED;
		}
	}

	k_mutex_lock(&lock, K_FOREVER);
	evse_auth_result(e, result, k_uptime_get());
	k_mutex_unlock(&lock);
}

static void do_start(int connector)
{
	struct evse *e = &evse[connector - 1];
	enum evse_result result = EVSE_RESULT_FAILED;
	int txn_id = -1;
	int meter_wh;
	int ret = -ENOTCONN;

	k_mutex_lock(&lock, K_FOREVER);
	meter_wh = (int)evse_energy_wh(e);
	k_mutex_unlock(&lock);

	if (reachable()) {
		ret = ocpp_start_transaction(session[connector - 1], meter_wh, (uint8_t)connector,
					     REQUEST_TIMEOUT_MS);
	}

	if (ret == 0) {
		result = EVSE_RESULT_ACCEPTED;
		txn_id = ocpp_peek_txn_id(session[connector - 1]);
		if (txn_id >= 0) {
			LOG_INF("C%d: StartTransaction at %d Wh: transaction %d", connector,
				  meter_wh, txn_id);
		} else {
			LOG_INF("C%d: StartTransaction at %d Wh: accepted", connector, meter_wh);
		}
	} else if (ret == -EACCES) {
		result = EVSE_RESULT_REJECTED;
		LOG_INF("C%d: StartTransaction: not authorised", connector);
	} else {
		LOG_WRN("C%d: StartTransaction: no answer (%d)", connector, ret);
	}

	k_mutex_lock(&lock, K_FOREVER);
	evse_txn_started(e, result, txn_id, k_uptime_get());
	k_mutex_unlock(&lock);
}

static void do_stop(int connector)
{
	struct evse *e = &evse[connector - 1];
	enum evse_stop_reason reason;
	uint32_t session_wh;
	int meter_wh;
	int ret = -ENOTCONN;

	k_mutex_lock(&lock, K_FOREVER);
	meter_wh = (int)evse_energy_wh(e);
	session_wh = evse_session_wh(e);
	reason = e->stop_reason;
	k_mutex_unlock(&lock);

	if (ocpp_up) {
		ret = ocpp_stop_transaction(session[connector - 1], meter_wh, REQUEST_TIMEOUT_MS);
	}

	if (ret < 0) {
		LOG_WRN("C%d: StopTransaction (%s): no answer (%d), stopped here", connector,
			  evse_stop_reason_str(reason), ret);
	} else {
		LOG_INF("C%d: StopTransaction (%s) at %d Wh, %u Wh charged", connector,
			  evse_stop_reason_str(reason), meter_wh, session_wh);
	}

	/* the session is over for the connector whatever the backend said */
	k_mutex_lock(&lock, K_FOREVER);
	evse_txn_stopped(e, k_uptime_get());
	k_mutex_unlock(&lock);
}

static void service(int connector)
{
	struct evse *e = &evse[connector - 1];
	enum evse_request request;

	/* one request can lead to the next: Authorize, then StartTransaction */
	for (;;) {
		k_mutex_lock(&lock, K_FOREVER);
		evse_advance(e, k_uptime_get());
		request = evse_take_request(e);
		k_mutex_unlock(&lock);

		switch (request) {
		case EVSE_REQ_AUTHORIZE:
			do_authorize(connector);
			break;
		case EVSE_REQ_START_TXN:
			do_start(connector);
			break;
		case EVSE_REQ_STOP_TXN:
			do_stop(connector);
			break;
		default:
			return;
		}
	}
}

static void poll_link(void)
{
	enum ocpp_peek_link peek;
	int heartbeat_s = 0;

	if (!ocpp_up) {
		return;
	}

	peek = ocpp_peek_link(session[0], &heartbeat_s);
	switch (peek) {
	case OCPP_PEEK_ONLINE:
		k_mutex_lock(&lock, K_FOREVER);
		link_status.heartbeat_s = heartbeat_s;
		k_mutex_unlock(&lock);
		set_link(CP_LINK_ONLINE);
		break;
	case OCPP_PEEK_BOOTING:
		set_link(CP_LINK_BOOTING);
		break;
	case OCPP_PEEK_OFFLINE:
		set_link(CP_LINK_CONNECTING);
		break;
	default:
		break;
	}
}

/* Bring the OCPP library up. It cannot be stopped or pointed elsewhere
 * afterwards, which is why a new backend URL needs a restart.
 */
static int link_up(void)
{
	static char fw_version[32];
	struct ocpp_cp_info cpi = {
		.model = CONFIG_OCPP_SIM_APP_MODEL,
		.vendor = CONFIG_OCPP_SIM_APP_VENDOR,
		.num_of_con = CP_CONNECTORS,
		.sl_no = CONFIG_OCPP_SIM_APP_SERIAL,
		.fw_ver = fw_version,
	};
	struct ocpp_cs_info csi = {0};
	struct app_config cfg;
	struct cp_status st = {0};
	int ret;

	config_get(&cfg);
	if (cfg.url[0] == '\0') {
		LOG_WRN("No backend URL: set one on the OCPP page or with \"sim url\"");
		return -ENOENT;
	}

	ret = backend_url_parse(cfg.url, &st.url);
	if (ret == 0) {
		ret = backend_url_ws_path(&st.url, cfg.cbid, st.ws_path, sizeof(st.ws_path));
	}
	if (ret < 0) {
		LOG_WRN("Backend URL %s is not usable (%d)", cfg.url, ret);
		return -ENOENT;
	}
	if (st.url.tls != IS_ENABLED(CONFIG_NET_SOCKETS_SOCKOPT_TLS)) {
		LOG_WRN("%s needs a build %s TLS sockets", cfg.url,
			  st.url.tls ? "with" : "without");
		return -ENOENT;
	}

	st.link = CP_LINK_NO_NETWORK;
	k_mutex_lock(&lock, K_FOREVER);
	link_status = st;
	k_mutex_unlock(&lock);

	while (net_wait_ready(K_MSEC(RETRY_MS)) < 0) {
		LOG_INF("waiting for the network");
	}
	net_sync_time();

	set_link(CP_LINK_RESOLVING);
	while (net_resolve(st.url.host, st.ip, sizeof(st.ip)) < 0) {
		k_sleep(K_MSEC(RETRY_MS));
	}
	k_mutex_lock(&lock, K_FOREVER);
	strcpy(link_status.ip, st.ip);
	k_mutex_unlock(&lock);

	snprintf(fw_version, sizeof(fw_version), "zephyr-%s", KERNEL_VERSION_STRING);
	csi.cs_ip = st.ip;
	csi.ws_url = st.ws_path;
	csi.port = st.url.port;
	csi.sa_family = NET_AF_INET;

	ret = ocpp_init(&cpi, &csi, notify, NULL);
	if (ret < 0) {
		LOG_WRN("OCPP library did not start (%d)", ret);
		return ret;
	}

	for (int i = 0; i < CP_CONNECTORS; i++) {
		ret = ocpp_session_open(&session[i]);
		if (ret < 0) {
			LOG_WRN("OCPP session %d did not open (%d)", i + 1, ret);
			return ret;
		}
	}

	ocpp_up = true;
	LOG_INF("Connecting to %s://%s:%u%s", st.url.tls ? "wss" : "ws", st.url.host,
		  st.url.port, st.ws_path);
	set_link(CP_LINK_CONNECTING);

	return 0;
}

static void cp_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	(void)link_up();

	for (;;) {
		(void)k_sem_take(&wake, K_MSEC(POLL_MS));
		poll_link();
		for (int connector = 1; connector <= CP_CONNECTORS; connector++) {
			service(connector);
		}
	}
}

int cp_init(void)
{
	static const struct evse_config cfg = {
		.max_power_w = CONFIG_OCPP_SIM_APP_MAX_POWER_W,
		.voltage_v = CONFIG_OCPP_SIM_APP_VOLTAGE_V,
		.phases = CONFIG_OCPP_SIM_APP_PHASES,
		.auth_timeout_s = CONFIG_OCPP_SIM_APP_AUTH_TIMEOUT_S,
	};
	int64_t now = k_uptime_get();

	for (int i = 0; i < CP_CONNECTORS; i++) {
		evse_init(&evse[i], (uint8_t)(i + 1), &cfg, now);
		/* a car that wants energy at a station that offers it: plugging in
		 * and starting is all it takes, the inputs are there to pause
		 */
		evse_set_ev_ready(&evse[i], true, now);
		evse_set_evse_ready(&evse[i], true, now);
	}

	k_thread_create(&cp_thread_data, cp_stack, K_THREAD_STACK_SIZEOF(cp_stack), cp_thread,
			NULL, NULL, NULL, 8, 0, K_NO_WAIT);
	k_thread_name_set(&cp_thread_data, "cp");

	return 0;
}

/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: Apache-2.0
 */

#include "evse.h"

#include <errno.h>
#include <string.h>

#include <zephyr/sys/util.h>

/* W*ms in one Wh */
#define WMS_PER_WH 3600000ULL

/* The contactor is closed and the EV draws what the EVSE offers */
static bool delivering(const struct evse *e)
{
	return e->txn == EVSE_TXN_ACTIVE && e->ev_plugged && e->ev_ready && e->evse_ready &&
	       !e->faulted;
}

/* Recompute what follows from the inputs after any change */
static void settle(struct evse *e, int64_t now_ms)
{
	if (!e->ev_plugged) {
		e->finishing = false;
		if (e->txn == EVSE_TXN_ACTIVE) {
			e->txn = EVSE_TXN_STOPPING;
			e->request = EVSE_REQ_STOP_TXN;
			e->stop_reason = EVSE_STOP_EV_DISCONNECTED;
		} else if (e->txn == EVSE_TXN_STARTING && e->stop_pending == EVSE_STOP_NONE) {
			e->stop_pending = EVSE_STOP_EV_DISCONNECTED;
		}
	}

	if (e->auth == EVSE_AUTH_ACCEPTED && e->txn == EVSE_TXN_IDLE) {
		if (e->ev_plugged && !e->faulted && !e->inoperative) {
			e->txn = EVSE_TXN_STARTING;
			e->request = EVSE_REQ_START_TXN;
			e->stop_pending = EVSE_STOP_NONE;
			e->finishing = false;
		} else if (now_ms >= e->auth_deadline_ms) {
			/* ConnectionTimeOut: nobody plugged in */
			e->auth = EVSE_AUTH_NONE;
		}
	}

	e->power_w = delivering(e) ? MIN(e->limit_w, e->cfg.max_power_w) : 0;
}

void evse_init(struct evse *e, uint8_t id, const struct evse_config *cfg, int64_t now_ms)
{
	memset(e, 0, sizeof(*e));

	e->cfg = *cfg;
	if (e->cfg.phases == 0) {
		e->cfg.phases = 1;
	}
	e->id = id;
	e->txn_id = -1;
	e->last_txn_id = -1;
	e->limit_w = e->cfg.max_power_w;
	e->last_ms = now_ms;
}

void evse_advance(struct evse *e, int64_t now_ms)
{
	int64_t dt = now_ms - e->last_ms;

	if (dt <= 0) {
		return;
	}

	/* the power only changes in settle(), so it was constant since last_ms */
	e->energy_wms += (uint64_t)e->power_w * dt;
	e->last_ms = now_ms;

	settle(e, now_ms);
}

void evse_set_plugged(struct evse *e, bool plugged, int64_t now_ms)
{
	evse_advance(e, now_ms);

	if (e->ev_plugged && !plugged &&
	    (e->auth == EVSE_AUTH_REJECTED || e->auth == EVSE_AUTH_FAILED)) {
		e->auth = EVSE_AUTH_NONE;
	}
	e->ev_plugged = plugged;

	settle(e, now_ms);
}

void evse_set_ev_ready(struct evse *e, bool ready, int64_t now_ms)
{
	evse_advance(e, now_ms);
	e->ev_ready = ready;
	settle(e, now_ms);
}

void evse_set_evse_ready(struct evse *e, bool ready, int64_t now_ms)
{
	evse_advance(e, now_ms);
	e->evse_ready = ready;
	settle(e, now_ms);
}

void evse_set_fault(struct evse *e, bool faulted, int64_t now_ms)
{
	evse_advance(e, now_ms);
	e->faulted = faulted;
	settle(e, now_ms);
}

void evse_set_inoperative(struct evse *e, bool inoperative, int64_t now_ms)
{
	evse_advance(e, now_ms);
	e->inoperative = inoperative;
	settle(e, now_ms);
}

void evse_set_limit(struct evse *e, uint32_t limit_w, int64_t now_ms)
{
	evse_advance(e, now_ms);
	e->limit_w = MIN(limit_w, e->cfg.max_power_w);
	settle(e, now_ms);
}

bool evse_is_idle(const struct evse *e)
{
	return e->txn == EVSE_TXN_IDLE && e->auth != EVSE_AUTH_PENDING &&
	       e->auth != EVSE_AUTH_ACCEPTED && !e->faulted && !e->inoperative;
}

static int check_idtag(const char *idtag)
{
	size_t len;

	if (idtag == NULL) {
		return -EINVAL;
	}
	len = strlen(idtag);
	if (len == 0 || len > EVSE_IDTAG_MAX) {
		return -EINVAL;
	}

	return 0;
}

static int begin_authorize(struct evse *e, const char *idtag)
{
	if (e->faulted || e->inoperative) {
		return -EACCES;
	}

	strcpy(e->idtag, idtag);
	e->auth = EVSE_AUTH_PENDING;
	e->request = EVSE_REQ_AUTHORIZE;

	return 0;
}

int evse_present_tag(struct evse *e, const char *idtag, int64_t now_ms)
{
	int ret = check_idtag(idtag);

	if (ret < 0) {
		return ret;
	}

	evse_advance(e, now_ms);

	if (e->auth == EVSE_AUTH_PENDING || e->txn == EVSE_TXN_STARTING ||
	    e->txn == EVSE_TXN_STOPPING) {
		return -EBUSY;
	}

	if (e->txn == EVSE_TXN_ACTIVE || e->auth == EVSE_AUTH_ACCEPTED) {
		/* only the identifier that started the session ends it */
		if (strcmp(e->idtag, idtag) != 0) {
			return -EBUSY;
		}
		return evse_stop(e, EVSE_STOP_LOCAL, now_ms);
	}

	ret = begin_authorize(e, idtag);
	settle(e, now_ms);

	return ret;
}

int evse_remote_start(struct evse *e, const char *idtag, int64_t now_ms)
{
	int ret = check_idtag(idtag);

	if (ret < 0) {
		return ret;
	}

	evse_advance(e, now_ms);

	if (e->faulted || e->inoperative) {
		return -EACCES;
	}
	if (!evse_is_idle(e)) {
		return -EBUSY;
	}

	ret = begin_authorize(e, idtag);
	settle(e, now_ms);

	return ret;
}

int evse_stop(struct evse *e, enum evse_stop_reason reason, int64_t now_ms)
{
	int ret = 0;

	evse_advance(e, now_ms);

	if (e->txn == EVSE_TXN_ACTIVE) {
		e->txn = EVSE_TXN_STOPPING;
		e->request = EVSE_REQ_STOP_TXN;
		e->stop_reason = reason;
	} else if (e->txn == EVSE_TXN_STARTING) {
		e->stop_pending = reason;
	} else if (e->txn == EVSE_TXN_IDLE && e->auth == EVSE_AUTH_ACCEPTED) {
		e->auth = EVSE_AUTH_NONE;
	} else {
		ret = -EALREADY;
	}

	settle(e, now_ms);

	return ret;
}

enum evse_request evse_take_request(struct evse *e)
{
	enum evse_request request = e->request;

	e->request = EVSE_REQ_NONE;

	return request;
}

void evse_auth_result(struct evse *e, enum evse_result result, int64_t now_ms)
{
	evse_advance(e, now_ms);

	if (e->auth != EVSE_AUTH_PENDING) {
		return;
	}

	switch (result) {
	case EVSE_RESULT_ACCEPTED:
		e->auth = EVSE_AUTH_ACCEPTED;
		e->auth_deadline_ms = now_ms + (int64_t)e->cfg.auth_timeout_s * 1000;
		break;
	case EVSE_RESULT_REJECTED:
		e->auth = EVSE_AUTH_REJECTED;
		break;
	default:
		e->auth = EVSE_AUTH_FAILED;
		break;
	}

	settle(e, now_ms);
}

void evse_txn_started(struct evse *e, enum evse_result result, int32_t txn_id, int64_t now_ms)
{
	evse_advance(e, now_ms);

	if (e->txn != EVSE_TXN_STARTING) {
		return;
	}

	if (result == EVSE_RESULT_ACCEPTED) {
		e->txn = EVSE_TXN_ACTIVE;
		e->txn_id = txn_id;
		e->last_txn_id = txn_id;
		e->txn_start_ms = now_ms;
		e->txn_end_ms = now_ms;
		e->session_start_wms = e->energy_wms;
		e->stop_reason = EVSE_STOP_NONE;
		if (e->stop_pending != EVSE_STOP_NONE) {
			e->txn = EVSE_TXN_STOPPING;
			e->request = EVSE_REQ_STOP_TXN;
			e->stop_reason = e->stop_pending;
		}
	} else {
		e->txn = EVSE_TXN_IDLE;
		e->txn_id = -1;
		e->auth = result == EVSE_RESULT_REJECTED ? EVSE_AUTH_REJECTED : EVSE_AUTH_FAILED;
	}
	e->stop_pending = EVSE_STOP_NONE;

	settle(e, now_ms);
}

void evse_txn_stopped(struct evse *e, int64_t now_ms)
{
	evse_advance(e, now_ms);

	if (e->txn != EVSE_TXN_STOPPING) {
		return;
	}

	e->txn = EVSE_TXN_IDLE;
	e->txn_id = -1;
	e->txn_end_ms = now_ms;
	e->auth = EVSE_AUTH_NONE;
	e->finishing = e->ev_plugged;

	settle(e, now_ms);
}

enum evse_status evse_status(const struct evse *e)
{
	if (e->faulted) {
		return EVSE_STATUS_FAULTED;
	}

	switch (e->txn) {
	case EVSE_TXN_ACTIVE:
		if (!e->evse_ready) {
			return EVSE_STATUS_SUSPENDED_EVSE;
		}
		if (!e->ev_ready || !e->ev_plugged) {
			return EVSE_STATUS_SUSPENDED_EV;
		}
		return EVSE_STATUS_CHARGING;
	case EVSE_TXN_STOPPING:
		return EVSE_STATUS_FINISHING;
	case EVSE_TXN_STARTING:
		return EVSE_STATUS_PREPARING;
	default:
		break;
	}

	if (e->inoperative) {
		return EVSE_STATUS_UNAVAILABLE;
	}
	if (e->finishing && e->ev_plugged) {
		return EVSE_STATUS_FINISHING;
	}
	if (e->ev_plugged || e->auth == EVSE_AUTH_PENDING || e->auth == EVSE_AUTH_ACCEPTED) {
		return EVSE_STATUS_PREPARING;
	}

	return EVSE_STATUS_AVAILABLE;
}

uint32_t evse_energy_wh(const struct evse *e)
{
	return (uint32_t)(e->energy_wms / WMS_PER_WH);
}

uint32_t evse_session_wh(const struct evse *e)
{
	return (uint32_t)((e->energy_wms - e->session_start_wms) / WMS_PER_WH);
}

uint32_t evse_session_s(const struct evse *e)
{
	int64_t end = e->txn == EVSE_TXN_IDLE ? e->txn_end_ms : e->last_ms;

	return (uint32_t)((end - e->txn_start_ms) / 1000);
}

static uint32_t phase_current_ma(const struct evse *e, uint32_t power_w)
{
	uint32_t volts = (uint32_t)e->cfg.voltage_v * e->cfg.phases;

	if (volts == 0) {
		return 0;
	}

	return (uint32_t)((uint64_t)power_w * 1000 / volts);
}

uint32_t evse_current_ma(const struct evse *e)
{
	return phase_current_ma(e, e->power_w);
}

uint32_t evse_offered_ma(const struct evse *e)
{
	return phase_current_ma(e, MIN(e->limit_w, e->cfg.max_power_w));
}

uint16_t evse_voltage_v(const struct evse *e)
{
	return delivering(e) ? e->cfg.voltage_v : 0;
}

const char *evse_status_str(enum evse_status status)
{
	static const char *const names[EVSE_STATUS_COUNT] = {
		[EVSE_STATUS_AVAILABLE] = "Available",
		[EVSE_STATUS_PREPARING] = "Preparing",
		[EVSE_STATUS_CHARGING] = "Charging",
		[EVSE_STATUS_SUSPENDED_EV] = "SuspendedEV",
		[EVSE_STATUS_SUSPENDED_EVSE] = "SuspendedEVSE",
		[EVSE_STATUS_FINISHING] = "Finishing",
		[EVSE_STATUS_UNAVAILABLE] = "Unavailable",
		[EVSE_STATUS_FAULTED] = "Faulted",
	};

	if ((unsigned int)status >= EVSE_STATUS_COUNT) {
		return "?";
	}

	return names[status];
}

const char *evse_auth_str(enum evse_auth auth)
{
	switch (auth) {
	case EVSE_AUTH_NONE:
		return "None";
	case EVSE_AUTH_PENDING:
		return "Pending";
	case EVSE_AUTH_ACCEPTED:
		return "Accepted";
	case EVSE_AUTH_REJECTED:
		return "Rejected";
	case EVSE_AUTH_FAILED:
		return "No answer";
	default:
		return "?";
	}
}

const char *evse_stop_reason_str(enum evse_stop_reason reason)
{
	switch (reason) {
	case EVSE_STOP_NONE:
		return "None";
	case EVSE_STOP_LOCAL:
		return "Local";
	case EVSE_STOP_REMOTE:
		return "Remote";
	case EVSE_STOP_EV_DISCONNECTED:
		return "EVDisconnected";
	default:
		return "?";
	}
}

/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: Apache-2.0
 *
 * One simulated connector: the inputs a charge point reads from its
 * hardware (cable, EV ready, EVSE ready, fault), the OCPP 1.6 connector
 * status that follows from them, the session (authorise, start, stop) and
 * an energy meter.
 *
 * No I/O in here. The time is an argument and the caller serialises access.
 * What has to be asked of the central system is handed out with
 * evse_take_request(); cp.c asks it through Zephyr's OCPP library and
 * reports the answer back.
 */

#ifndef OCPP_SIM_APP_EVSE_H_
#define OCPP_SIM_APP_EVSE_H_

#include <stdbool.h>
#include <stdint.h>

/* Longest identifier (OCPP 1.6 IdToken is a CiString20) */
#define EVSE_IDTAG_MAX 20

/* OCPP 1.6 ChargePointStatus */
enum evse_status {
	EVSE_STATUS_AVAILABLE,
	EVSE_STATUS_PREPARING,
	EVSE_STATUS_CHARGING,
	EVSE_STATUS_SUSPENDED_EV,
	EVSE_STATUS_SUSPENDED_EVSE,
	EVSE_STATUS_FINISHING,
	EVSE_STATUS_UNAVAILABLE,
	EVSE_STATUS_FAULTED,

	EVSE_STATUS_COUNT
};

enum evse_auth {
	EVSE_AUTH_NONE,     /* nothing presented */
	EVSE_AUTH_PENDING,  /* Authorize is on its way */
	EVSE_AUTH_ACCEPTED, /* the session may start */
	EVSE_AUTH_REJECTED, /* the central system refused the identifier */
	EVSE_AUTH_FAILED,   /* no answer */
};

enum evse_txn {
	EVSE_TXN_IDLE,
	EVSE_TXN_STARTING, /* StartTransaction is on its way */
	EVSE_TXN_ACTIVE,
	EVSE_TXN_STOPPING, /* StopTransaction is on its way */
};

/* What to ask the central system next */
enum evse_request {
	EVSE_REQ_NONE,
	EVSE_REQ_AUTHORIZE, /* answer with evse_auth_result() */
	EVSE_REQ_START_TXN, /* answer with evse_txn_started() */
	EVSE_REQ_STOP_TXN,  /* answer with evse_txn_stopped() */
};

enum evse_result {
	EVSE_RESULT_ACCEPTED,
	EVSE_RESULT_REJECTED,
	EVSE_RESULT_FAILED, /* no answer */
};

enum evse_stop_reason {
	EVSE_STOP_NONE,
	EVSE_STOP_LOCAL,           /* identifier presented again, stop button */
	EVSE_STOP_REMOTE,          /* RemoteStopTransaction */
	EVSE_STOP_EV_DISCONNECTED, /* cable pulled */
};

struct evse_config {
	uint32_t max_power_w;    /* rating of the EVSE */
	uint16_t voltage_v;      /* phase voltage */
	uint8_t phases;          /* 1 or 3 */
	uint16_t auth_timeout_s; /* accepted identifier is dropped if no EV plugs in */
};

/* Read the fields, change them through the functions below */
struct evse {
	struct evse_config cfg;
	uint8_t id; /* connector id, starts at 1 */

	/* inputs */
	bool ev_plugged;  /* cable between EV and EVSE */
	bool ev_ready;    /* the EV asks for energy */
	bool evse_ready;  /* the EVSE offers energy */
	bool faulted;     /* hardware fault */
	bool inoperative; /* taken out of service */

	/* session */
	enum evse_auth auth;
	enum evse_txn txn;
	enum evse_request request;
	enum evse_stop_reason stop_reason;  /* of the running or the last stop */
	enum evse_stop_reason stop_pending; /* stop asked for while starting */
	char idtag[EVSE_IDTAG_MAX + 1];
	int32_t txn_id;      /* from the central system, -1 without transaction */
	int32_t last_txn_id; /* of the running or the last transaction, -1 if none yet */
	bool finishing; /* transaction over, cable still in */
	int64_t auth_deadline_ms;
	int64_t txn_start_ms;
	int64_t txn_end_ms;

	/* meter */
	int64_t last_ms;
	uint32_t limit_w;           /* power the EVSE offers */
	uint32_t power_w;           /* power going to the EV now */
	uint64_t energy_wms;        /* meter register in W*ms */
	uint64_t session_start_wms; /* meter register when the transaction began */
};

/* Available, nothing plugged in */
void evse_init(struct evse *e, uint8_t id, const struct evse_config *cfg, int64_t now_ms);

/* Bring the meter and the time-outs up to now_ms. Every other function
 * does this first; call it to keep the readings fresh in between.
 */
void evse_advance(struct evse *e, int64_t now_ms);

/* Inputs */
void evse_set_plugged(struct evse *e, bool plugged, int64_t now_ms);
void evse_set_ev_ready(struct evse *e, bool ready, int64_t now_ms);
void evse_set_evse_ready(struct evse *e, bool ready, int64_t now_ms);
void evse_set_fault(struct evse *e, bool faulted, int64_t now_ms);
void evse_set_inoperative(struct evse *e, bool inoperative, int64_t now_ms);
/* Power the EVSE offers, clamped to 0..cfg.max_power_w */
void evse_set_limit(struct evse *e, uint32_t limit_w, int64_t now_ms);

/* An identifier is presented at the connector (RFID card). Idle: asks for
 * authorisation. Accepted and waiting for the EV: the same identifier
 * withdraws it. Transaction running: the same identifier stops it.
 *
 * 0, -EINVAL (empty or too long), -EBUSY (a request is pending or another
 * identifier is in use), -EACCES (faulted or out of service).
 */
int evse_present_tag(struct evse *e, const char *idtag, int64_t now_ms);

/* RemoteStartTransaction: like presenting idtag, but never stops or
 * withdraws anything. Same return values.
 */
int evse_remote_start(struct evse *e, const char *idtag, int64_t now_ms);

/* Stop the transaction or withdraw an accepted identifier. 0, or -EALREADY
 * if there is nothing to stop.
 */
int evse_stop(struct evse *e, enum evse_stop_reason reason, int64_t now_ms);

/* The connector can take a new session */
bool evse_is_idle(const struct evse *e);

/* The request to send, if any. Handed out once; the connector stays in the
 * matching pending state until the result is reported.
 */
enum evse_request evse_take_request(struct evse *e);

void evse_auth_result(struct evse *e, enum evse_result result, int64_t now_ms);
/* txn_id is used when accepted */
void evse_txn_started(struct evse *e, enum evse_result result, int32_t txn_id, int64_t now_ms);
/* The transaction ends whatever the central system answered */
void evse_txn_stopped(struct evse *e, int64_t now_ms);

/* Readings */
enum evse_status evse_status(const struct evse *e);
uint32_t evse_energy_wh(const struct evse *e);  /* meter register */
uint32_t evse_session_wh(const struct evse *e); /* running or last transaction */
uint32_t evse_session_s(const struct evse *e);  /* its duration so far */
uint32_t evse_current_ma(const struct evse *e); /* per phase */
uint32_t evse_offered_ma(const struct evse *e); /* per phase */
uint16_t evse_voltage_v(const struct evse *e);  /* 0 unless energy flows */

/* Names as OCPP spells them */
const char *evse_status_str(enum evse_status status);
const char *evse_auth_str(enum evse_auth auth);
const char *evse_stop_reason_str(enum evse_stop_reason reason);

#endif /* OCPP_SIM_APP_EVSE_H_ */

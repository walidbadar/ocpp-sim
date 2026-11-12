/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "evse.h"

#define TAG   "ABC123"
#define OTHER "ZZZ999"

/* 11 kW over three phases */
static const struct evse_config plain = {
	.max_power_w = 11000,
	.voltage_v = 230,
	.phases = 3,
	.auth_timeout_s = 30,
};

static struct evse e;
static int64_t now;

static void before(void *fixture)
{
	ARG_UNUSED(fixture);

	now = 1000;
	evse_init(&e, 1, &plain, now);
}

/* Authorise TAG and answer the requests until the transaction runs */
static void start_session(int32_t txn_id)
{
	zassert_ok(evse_present_tag(&e, TAG, now));
	zassert_equal(evse_take_request(&e), EVSE_REQ_AUTHORIZE);
	evse_auth_result(&e, EVSE_RESULT_ACCEPTED, now);
	if (!e.ev_plugged) {
		evse_set_plugged(&e, true, now);
	}
	zassert_equal(evse_take_request(&e), EVSE_REQ_START_TXN);
	evse_txn_started(&e, EVSE_RESULT_ACCEPTED, txn_id, now);
	zassert_equal(e.txn, EVSE_TXN_ACTIVE);
}

static void make_ready(void)
{
	evse_set_ev_ready(&e, true, now);
	evse_set_evse_ready(&e, true, now);
}

ZTEST(evse, test_starts_available)
{
	zassert_equal(evse_status(&e), EVSE_STATUS_AVAILABLE);
	zassert_equal(e.id, 1);
	zassert_equal(e.txn_id, -1);
	zassert_equal(e.last_txn_id, -1);
	zassert_equal(e.power_w, 0);
	zassert_equal(e.limit_w, 11000);
	zassert_equal(evse_energy_wh(&e), 0);
	zassert_equal(evse_voltage_v(&e), 0);
	zassert_true(evse_is_idle(&e));
	zassert_equal(evse_take_request(&e), EVSE_REQ_NONE);
}

ZTEST(evse, test_plug_alone_is_preparing)
{
	evse_set_plugged(&e, true, now);
	zassert_equal(evse_status(&e), EVSE_STATUS_PREPARING);
	zassert_equal(evse_take_request(&e), EVSE_REQ_NONE);

	evse_set_plugged(&e, false, now);
	zassert_equal(evse_status(&e), EVSE_STATUS_AVAILABLE);
}

ZTEST(evse, test_tag_then_plug)
{
	zassert_ok(evse_present_tag(&e, TAG, now));
	zassert_equal(e.auth, EVSE_AUTH_PENDING);
	zassert_equal(evse_status(&e), EVSE_STATUS_PREPARING);
	zassert_equal(evse_take_request(&e), EVSE_REQ_AUTHORIZE);
	/* handed out once */
	zassert_equal(evse_take_request(&e), EVSE_REQ_NONE);
	zassert_str_equal(e.idtag, TAG);

	evse_auth_result(&e, EVSE_RESULT_ACCEPTED, now);
	zassert_equal(e.auth, EVSE_AUTH_ACCEPTED);
	/* nothing to start before the cable is in */
	zassert_equal(evse_take_request(&e), EVSE_REQ_NONE);
	zassert_equal(evse_status(&e), EVSE_STATUS_PREPARING);

	evse_set_plugged(&e, true, now);
	zassert_equal(e.txn, EVSE_TXN_STARTING);
	zassert_equal(evse_take_request(&e), EVSE_REQ_START_TXN);
	zassert_equal(evse_status(&e), EVSE_STATUS_PREPARING);

	evse_txn_started(&e, EVSE_RESULT_ACCEPTED, 42, now);
	zassert_equal(e.txn, EVSE_TXN_ACTIVE);
	zassert_equal(e.txn_id, 42);
}

ZTEST(evse, test_plug_then_tag)
{
	evse_set_plugged(&e, true, now);
	zassert_ok(evse_present_tag(&e, TAG, now));
	zassert_equal(evse_take_request(&e), EVSE_REQ_AUTHORIZE);

	evse_auth_result(&e, EVSE_RESULT_ACCEPTED, now);
	/* the cable is in already: the transaction starts right away */
	zassert_equal(evse_take_request(&e), EVSE_REQ_START_TXN);
}

ZTEST(evse, test_status_follows_the_ready_inputs)
{
	start_session(7);

	/* neither side is ready: the EVSE does not offer energy */
	zassert_equal(evse_status(&e), EVSE_STATUS_SUSPENDED_EVSE);

	evse_set_evse_ready(&e, true, now);
	zassert_equal(evse_status(&e), EVSE_STATUS_SUSPENDED_EV);
	zassert_equal(e.power_w, 0);

	evse_set_ev_ready(&e, true, now);
	zassert_equal(evse_status(&e), EVSE_STATUS_CHARGING);
	zassert_equal(e.power_w, 11000);
	zassert_equal(evse_voltage_v(&e), 230);

	evse_set_evse_ready(&e, false, now);
	zassert_equal(evse_status(&e), EVSE_STATUS_SUSPENDED_EVSE);
	zassert_equal(e.power_w, 0);
	zassert_equal(evse_voltage_v(&e), 0);
}

ZTEST(evse, test_energy_is_power_times_time)
{
	start_session(1);
	make_ready();

	now += 3600 * 1000;
	evse_advance(&e, now);
	zassert_equal(evse_energy_wh(&e), 11000);
	zassert_equal(evse_session_wh(&e), 11000);

	/* nothing flows while suspended */
	evse_set_ev_ready(&e, false, now);
	now += 3600 * 1000;
	evse_advance(&e, now);
	zassert_equal(evse_energy_wh(&e), 11000);
}

ZTEST(evse, test_current_per_phase)
{
	start_session(1);
	make_ready();

	/* 11 kW over three phases of 230 V */
	zassert_equal(evse_current_ma(&e), 15942);
	zassert_equal(evse_offered_ma(&e), 15942);

	evse_set_limit(&e, 6900, now);
	zassert_equal(e.power_w, 6900);
	zassert_equal(evse_current_ma(&e), 10000);
	zassert_equal(evse_offered_ma(&e), 10000);
}

ZTEST(evse, test_limit_is_clamped_to_the_rating)
{
	evse_set_limit(&e, 50000, now);
	zassert_equal(e.limit_w, 11000);

	start_session(1);
	make_ready();
	evse_set_limit(&e, 0, now);
	zassert_equal(e.power_w, 0);
	/* offering nothing does not change the status by itself */
	zassert_equal(evse_status(&e), EVSE_STATUS_CHARGING);
}

ZTEST(evse, test_rejected_tag)
{
	evse_set_plugged(&e, true, now);
	zassert_ok(evse_present_tag(&e, TAG, now));
	(void)evse_take_request(&e);
	evse_auth_result(&e, EVSE_RESULT_REJECTED, now);

	zassert_equal(e.auth, EVSE_AUTH_REJECTED);
	zassert_equal(e.txn, EVSE_TXN_IDLE);
	zassert_equal(evse_take_request(&e), EVSE_REQ_NONE);
	zassert_equal(evse_status(&e), EVSE_STATUS_PREPARING);
	zassert_true(evse_is_idle(&e));

	/* another try is possible, pulling the cable forgets the refusal */
	zassert_ok(evse_present_tag(&e, OTHER, now));
	(void)evse_take_request(&e);
	evse_auth_result(&e, EVSE_RESULT_FAILED, now);
	zassert_equal(e.auth, EVSE_AUTH_FAILED);
	evse_set_plugged(&e, false, now);
	zassert_equal(e.auth, EVSE_AUTH_NONE);
	zassert_equal(evse_status(&e), EVSE_STATUS_AVAILABLE);
}

ZTEST(evse, test_accepted_tag_times_out_without_ev)
{
	zassert_ok(evse_present_tag(&e, TAG, now));
	(void)evse_take_request(&e);
	evse_auth_result(&e, EVSE_RESULT_ACCEPTED, now);

	now += 29 * 1000;
	evse_advance(&e, now);
	zassert_equal(e.auth, EVSE_AUTH_ACCEPTED);

	now += 1000;
	evse_advance(&e, now);
	zassert_equal(e.auth, EVSE_AUTH_NONE);
	zassert_equal(evse_status(&e), EVSE_STATUS_AVAILABLE);

	/* plugging in now does not start anything */
	evse_set_plugged(&e, true, now);
	zassert_equal(evse_take_request(&e), EVSE_REQ_NONE);
}

ZTEST(evse, test_same_tag_withdraws_before_plugging)
{
	zassert_ok(evse_present_tag(&e, TAG, now));
	(void)evse_take_request(&e);
	evse_auth_result(&e, EVSE_RESULT_ACCEPTED, now);

	zassert_equal(evse_present_tag(&e, OTHER, now), -EBUSY);
	zassert_ok(evse_present_tag(&e, TAG, now));
	zassert_equal(e.auth, EVSE_AUTH_NONE);
	zassert_equal(evse_status(&e), EVSE_STATUS_AVAILABLE);
}

ZTEST(evse, test_same_tag_stops_the_transaction)
{
	start_session(5);
	make_ready();

	zassert_equal(evse_present_tag(&e, OTHER, now), -EBUSY);
	zassert_equal(e.txn, EVSE_TXN_ACTIVE);

	zassert_ok(evse_present_tag(&e, TAG, now));
	zassert_equal(e.txn, EVSE_TXN_STOPPING);
	zassert_equal(e.stop_reason, EVSE_STOP_LOCAL);
	zassert_equal(evse_take_request(&e), EVSE_REQ_STOP_TXN);
	zassert_equal(e.power_w, 0);
	zassert_equal(evse_status(&e), EVSE_STATUS_FINISHING);
	/* the transaction id is still there for the StopTransaction */
	zassert_equal(e.txn_id, 5);

	evse_txn_stopped(&e, now);
	zassert_equal(e.txn, EVSE_TXN_IDLE);
	zassert_equal(e.txn_id, -1);
	/* still known for the receipt */
	zassert_equal(e.last_txn_id, 5);
	zassert_equal(e.auth, EVSE_AUTH_NONE);
	/* cable still in */
	zassert_equal(evse_status(&e), EVSE_STATUS_FINISHING);

	evse_set_plugged(&e, false, now);
	zassert_equal(evse_status(&e), EVSE_STATUS_AVAILABLE);
}

ZTEST(evse, test_second_session_without_unplugging)
{
	start_session(1);
	zassert_ok(evse_stop(&e, EVSE_STOP_LOCAL, now));
	(void)evse_take_request(&e);
	evse_txn_stopped(&e, now);
	zassert_equal(evse_status(&e), EVSE_STATUS_FINISHING);

	start_session(2);
	zassert_equal(e.txn_id, 2);
	zassert_false(e.finishing);
}

ZTEST(evse, test_unplug_stops_the_transaction)
{
	start_session(9);
	make_ready();

	evse_set_plugged(&e, false, now);
	zassert_equal(e.txn, EVSE_TXN_STOPPING);
	zassert_equal(e.stop_reason, EVSE_STOP_EV_DISCONNECTED);
	zassert_equal(evse_take_request(&e), EVSE_REQ_STOP_TXN);
	zassert_equal(e.power_w, 0);

	evse_txn_stopped(&e, now);
	zassert_equal(evse_status(&e), EVSE_STATUS_AVAILABLE);
}

ZTEST(evse, test_unplug_while_starting_stops_after_the_start)
{
	zassert_ok(evse_present_tag(&e, TAG, now));
	(void)evse_take_request(&e);
	evse_auth_result(&e, EVSE_RESULT_ACCEPTED, now);
	evse_set_plugged(&e, true, now);
	zassert_equal(evse_take_request(&e), EVSE_REQ_START_TXN);

	evse_set_plugged(&e, false, now);
	/* the StartTransaction is on its way and cannot be taken back */
	zassert_equal(e.txn, EVSE_TXN_STARTING);
	zassert_equal(evse_take_request(&e), EVSE_REQ_NONE);

	evse_txn_started(&e, EVSE_RESULT_ACCEPTED, 3, now);
	zassert_equal(e.txn, EVSE_TXN_STOPPING);
	zassert_equal(e.txn_id, 3);
	zassert_equal(e.stop_reason, EVSE_STOP_EV_DISCONNECTED);
	zassert_equal(evse_take_request(&e), EVSE_REQ_STOP_TXN);
}

ZTEST(evse, test_start_refused)
{
	evse_set_plugged(&e, true, now);
	zassert_ok(evse_present_tag(&e, TAG, now));
	(void)evse_take_request(&e);
	evse_auth_result(&e, EVSE_RESULT_ACCEPTED, now);
	(void)evse_take_request(&e);

	evse_txn_started(&e, EVSE_RESULT_REJECTED, 0, now);
	zassert_equal(e.txn, EVSE_TXN_IDLE);
	zassert_equal(e.txn_id, -1);
	zassert_equal(e.auth, EVSE_AUTH_REJECTED);
	/* no second attempt on its own */
	zassert_equal(evse_take_request(&e), EVSE_REQ_NONE);
}

ZTEST(evse, test_requests_are_refused_while_one_is_pending)
{
	zassert_ok(evse_present_tag(&e, TAG, now));
	zassert_equal(evse_present_tag(&e, TAG, now), -EBUSY);
	zassert_equal(evse_remote_start(&e, OTHER, now), -EBUSY);
	zassert_false(evse_is_idle(&e));
}

ZTEST(evse, test_bad_tags)
{
	zassert_equal(evse_present_tag(&e, "", now), -EINVAL);
	zassert_equal(evse_present_tag(&e, NULL, now), -EINVAL);
	zassert_equal(evse_present_tag(&e, "123456789012345678901", now), -EINVAL);
	zassert_ok(evse_present_tag(&e, "12345678901234567890", now));
}

ZTEST(evse, test_remote_start_and_stop)
{
	evse_set_plugged(&e, true, now);
	make_ready();

	zassert_ok(evse_remote_start(&e, TAG, now));
	zassert_equal(evse_take_request(&e), EVSE_REQ_AUTHORIZE);
	evse_auth_result(&e, EVSE_RESULT_ACCEPTED, now);
	zassert_equal(evse_take_request(&e), EVSE_REQ_START_TXN);
	evse_txn_started(&e, EVSE_RESULT_ACCEPTED, 11, now);
	zassert_equal(evse_status(&e), EVSE_STATUS_CHARGING);

	/* a running session is not replaced */
	zassert_equal(evse_remote_start(&e, TAG, now), -EBUSY);

	zassert_ok(evse_stop(&e, EVSE_STOP_REMOTE, now));
	zassert_equal(e.stop_reason, EVSE_STOP_REMOTE);
	zassert_equal(evse_take_request(&e), EVSE_REQ_STOP_TXN);
	evse_txn_stopped(&e, now);

	zassert_equal(evse_stop(&e, EVSE_STOP_REMOTE, now), -EALREADY);
}

ZTEST(evse, test_fault_interrupts_and_resumes)
{
	start_session(1);
	make_ready();

	evse_set_fault(&e, true, now);
	zassert_equal(evse_status(&e), EVSE_STATUS_FAULTED);
	zassert_equal(e.power_w, 0);
	/* the transaction is kept */
	zassert_equal(e.txn, EVSE_TXN_ACTIVE);

	now += 3600 * 1000;
	evse_advance(&e, now);
	zassert_equal(evse_energy_wh(&e), 0);

	evse_set_fault(&e, false, now);
	zassert_equal(evse_status(&e), EVSE_STATUS_CHARGING);
	zassert_equal(e.power_w, 11000);
}

ZTEST(evse, test_no_new_session_when_faulted_or_out_of_service)
{
	evse_set_fault(&e, true, now);
	zassert_equal(evse_present_tag(&e, TAG, now), -EACCES);
	zassert_equal(evse_remote_start(&e, TAG, now), -EACCES);
	evse_set_fault(&e, false, now);

	evse_set_inoperative(&e, true, now);
	zassert_equal(evse_status(&e), EVSE_STATUS_UNAVAILABLE);
	zassert_equal(evse_present_tag(&e, TAG, now), -EACCES);
	zassert_false(evse_is_idle(&e));

	evse_set_inoperative(&e, false, now);
	zassert_equal(evse_status(&e), EVSE_STATUS_AVAILABLE);
	zassert_ok(evse_present_tag(&e, TAG, now));
}

ZTEST(evse, test_out_of_service_lets_the_transaction_finish)
{
	start_session(1);
	make_ready();

	evse_set_inoperative(&e, true, now);
	zassert_equal(evse_status(&e), EVSE_STATUS_CHARGING);
	zassert_equal(e.power_w, 11000);

	zassert_ok(evse_stop(&e, EVSE_STOP_LOCAL, now));
	(void)evse_take_request(&e);
	evse_txn_stopped(&e, now);
	zassert_equal(evse_status(&e), EVSE_STATUS_UNAVAILABLE);
}

ZTEST(evse, test_session_energy_counts_from_the_start)
{
	start_session(1);
	make_ready();
	now += 1800 * 1000;
	zassert_ok(evse_stop(&e, EVSE_STOP_LOCAL, now));
	(void)evse_take_request(&e);
	evse_txn_stopped(&e, now);
	zassert_equal(evse_session_wh(&e), 5500);

	start_session(2);
	zassert_equal(evse_session_wh(&e), 0);
	zassert_equal(evse_energy_wh(&e), 5500);
	now += 1800 * 1000;
	evse_advance(&e, now);
	zassert_equal(evse_session_wh(&e), 5500);
	zassert_equal(evse_energy_wh(&e), 11000);
}

ZTEST(evse, test_session_duration)
{
	zassert_equal(evse_session_s(&e), 0);

	start_session(1);
	now += 90 * 1000;
	evse_advance(&e, now);
	zassert_equal(evse_session_s(&e), 90);

	zassert_ok(evse_stop(&e, EVSE_STOP_LOCAL, now));
	(void)evse_take_request(&e);
	now += 2000;
	evse_txn_stopped(&e, now);
	zassert_equal(evse_session_s(&e), 92);

	/* it stays once the transaction is over */
	now += 60 * 1000;
	evse_advance(&e, now);
	zassert_equal(evse_session_s(&e), 92);
}

ZTEST(evse, test_time_going_backwards_is_ignored)
{
	start_session(1);
	make_ready();
	now += 3600 * 1000;
	evse_advance(&e, now);

	evse_advance(&e, now - 5000);
	zassert_equal(evse_energy_wh(&e), 11000);
	zassert_equal(e.last_ms, now);
}

ZTEST(evse, test_late_results_are_ignored)
{
	evse_auth_result(&e, EVSE_RESULT_ACCEPTED, now);
	zassert_equal(e.auth, EVSE_AUTH_NONE);

	evse_txn_started(&e, EVSE_RESULT_ACCEPTED, 1, now);
	zassert_equal(e.txn, EVSE_TXN_IDLE);

	evse_txn_stopped(&e, now);
	zassert_equal(e.txn, EVSE_TXN_IDLE);
}

ZTEST(evse, test_names)
{
	zassert_str_equal(evse_status_str(EVSE_STATUS_AVAILABLE), "Available");
	zassert_str_equal(evse_status_str(EVSE_STATUS_SUSPENDED_EVSE), "SuspendedEVSE");
	zassert_str_equal(evse_status_str(EVSE_STATUS_COUNT), "?");
	zassert_str_equal(evse_auth_str(EVSE_AUTH_ACCEPTED), "Accepted");
	zassert_str_equal(evse_stop_reason_str(EVSE_STOP_EV_DISCONNECTED), "EVDisconnected");
}

ZTEST_SUITE(evse, NULL, NULL, before, NULL, NULL);

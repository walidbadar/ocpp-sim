..
   Copyright (c) 2026 Muhammad Waleed Badar
   SPDX-License-Identifier: Apache-2.0

Architecture
############

.. code-block:: none

   display (ui.c)    shell (sim_shell.c)
          \              /
           v            v
      charge point (cp.c) -----> connectors (evse.c)
           |      ^
           v      |  remote start/stop, unlock, meter readings
      Zephyr OCPP library (subsys/net/lib/ocpp)
           |
           v  WebSocket, OCPP 1.6J
      central system (CitrineOS)

The OCPP library speaks the protocol and nothing else: it has no notion of a
cable or a car, asks the application for meter readings and leaves it to the
application when to authorise, start and stop. The application supplies the
charger around it.

Source files
************

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - File
     - Role
   * - ``main.c``
     - start-up and the display loop
   * - ``cp.c``
     - the charge point: the connectors joined to the OCPP library
   * - ``evse.c``
     - one simulated connector: inputs, status, session, meter
   * - ``url.c``
     - splits the backend URL into host, port and path
   * - ``ocpp_peek.c``
     - link state and transaction id from the library's internals
   * - ``ui.c``
     - the LVGL screens
   * - ``config.c``
     - backend URL, identity and cards in the settings
   * - ``net.c``
     - waits for the network, looks up the backend, sets the clock
   * - ``sim_shell.c``
     - the ``sim`` commands
   * - ``host.c``
     - ``native_sim`` only: command line, host clock, screenshots, taps
   * - ``wifi.c``
     - boards with Wi-Fi only: joins the stored network, again when it drops

The connector
*************

``evse.c`` is the charger hardware of the simulation, in plain C without
I/O: the cable, the two readiness inputs, the fault and service switches,
the session and an energy meter. The time is an argument of every function,
which is what lets ``tests/evse`` run it through hours of charging in no
time.

It never talks to the central system. When something has to be asked it
hands out a request, and is told the outcome:

.. list-table::
   :header-rows: 1
   :widths: 30 35 35

   * - Request
     - Made with
     - Answered with
   * - ``EVSE_REQ_AUTHORIZE``
     - ``ocpp_authorize()``
     - ``evse_auth_result()``
   * - ``EVSE_REQ_START_TXN``
     - ``ocpp_start_transaction()``
     - ``evse_txn_started()``
   * - ``EVSE_REQ_STOP_TXN``
     - ``ocpp_stop_transaction()``
     - ``evse_txn_stopped()``

The meter is simple on purpose: while the status is *Charging* the power is
what the connector offers, the energy is that power over time, the current
is the power over voltage and phases.

Threads
*******

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Thread
     - What it does in ``cp.c``
   * - main
     - runs LVGL; reads the connectors and changes their inputs
   * - shell
     - the same, for the ``sim`` commands
   * - ``cp``
     - takes the requests of the connectors and makes them with the OCPP
       library, which blocks until the central system answers
   * - OCPP library
     - calls back for RemoteStartTransaction, RemoteStopTransaction,
       UnlockConnector and for the meter readings

One mutex guards the connectors. It is never held across a call into the
OCPP library, so the display stays responsive while a request waits for its
answer.

Use of the OCPP library
***********************

``ocpp_init()`` gets the address the backend host resolves to, the port, the
WebSocket path (the path of the URL with the charge box identity appended)
and the callback. One session per connector is opened with
``ocpp_session_open()`` and kept. The library then connects, sends the
BootNotification and the heartbeats and reconnects on its own.

The library cannot be stopped or pointed elsewhere once started, which is why
a new backend URL takes effect after a restart.

Library internals
=================

Two things the library knows have no API:

- whether the WebSocket is up and the BootNotification was accepted,
- the transaction id the central system returned.

``ocpp_peek.c`` reads them from the library's private structures
(``subsys/net/lib/ocpp/ocpp_i.h``). It is the only file that depends on
their layout. With ``CONFIG_OCPP_SIM_APP_PEEK=n`` it is left out: the display
then cannot tell the link state and shows no transaction id.

The display
***********

``ui.c`` builds all screens once and shows the one that fits the connector
on display; five times a second it brings the texts up to date, touching
only what changed.

All sizes are given for 800x480 and scaled to the width of the display.
Headlines and numbers are Montserrat Bold, rendered at the needed sizes by
LVGL's TinyTTF from ``app/fonts``; the remaining text and the icons are
LVGL's built-in fonts. The socket is drawn from circles.

Settings
********

``config.c`` keeps three things in Zephyr's settings (NVS): ``sim/url``,
``sim/cbid`` and ``sim/tag/<n>``. On ``native_sim`` the flash behind them is
the file ``flash.bin``.

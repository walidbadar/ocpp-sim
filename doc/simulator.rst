..
   Copyright (c) 2026 Muhammad Waleed Badar
   SPDX-License-Identifier: Apache-2.0

The simulator
#############

The display
***********

The top bar is always there: the charge box identity, a switch between the
connectors (``1``, ``2``), a round button that shows the link to the central
system (green on line, amber while connecting, grey without backend) and
opens the diagnostics, the OCPP status of the connector on display, and the
time in UTC.

Below it, one screen follows what that connector is doing.

Ready
=====

Nothing is plugged in.

- Tap the **socket** to plug the cable in.
- Tap the **Tap card** tile to present the card of the connector first; the
  cable can follow within 30 seconds.
- **Fast**, **Standard** and **Eco** set the power the connector offers: 16,
  10 and 6 A of an 11 kW connector.
- The card next to the headline is the identifier that gets presented. Tap it
  to enter another one.

Connected
=========

The cable is in, or a card was accepted and the cable is awaited.

- **Start charging** presents the card. While the central system is asked the
  button reads *Checking*; a refusal or a missing answer is shown under the
  card.
- **EV asks for energy** and **EVSE offers energy** are the two readiness
  inputs of a charger. Both are on by default so that plugging in and
  starting is all it takes.
- **Unplug** pulls the cable; **Cancel** withdraws an accepted card.
- The arrow above the headline does both and returns to Ready.

Charging
========

A transaction is running. The large number is the energy of the session;
below are power, current per phase, voltage and duration, and a bar with the
power as a share of the rating.

- **EV** pauses and resumes on the side of the vehicle: the status becomes
  *SuspendedEV*.
- **EVSE** does the same for the station: *SuspendedEVSE*.
- **Stop charging** ends the transaction.

Complete
========

The transaction is over and the cable is still in: energy delivered,
duration, transaction id, card, and the meter reading that went out with the
StopTransaction. **Unplug to finish** returns to Ready.

Out of order
============

The connector has a fault or was taken out of service. Both are switches on
the diagnostics page.

Diagnostics
===========

- **Central system**: the four steps to being on line, each *OK* or
  *Pending*; the backend URL; **Restart**, which is needed after changing the
  URL or the identity.
- **Charge box**: the identity, with the endpoint in use as a QR code. Tap
  the identity to change it.
- **Connector**: power offered, the **Hardware fault** and **In service**
  switches and the card of the connector on display.

Connector status
****************

The status in the top bar is the OCPP 1.6 ChargePointStatus, worked out from
the inputs:

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Status
     - When
   * - Faulted
     - the fault switch is on
   * - Charging
     - transaction running, cable in, the EV asks and the EVSE offers
   * - SuspendedEV
     - transaction running, the EVSE offers, the EV does not ask
   * - SuspendedEVSE
     - transaction running, the EVSE does not offer
   * - Finishing
     - StopTransaction on its way, or transaction over with the cable in
   * - Preparing
     - cable in, card presented or accepted, or StartTransaction on its way
   * - Unavailable
     - out of service and no transaction
   * - Available
     - none of the above

The rules of a session:

- A transaction starts when a card was accepted and the cable is in, in
  either order. An accepted card is dropped after 30 seconds without cable.
- The card that started a transaction stops it; another card is ignored.
- Pulling the cable stops the transaction.
- RemoteStartTransaction authorises its card like a local one, on the
  connector it names or else on the first free one. RemoteStopTransaction
  and UnlockConnector stop the transaction.
- A fault interrupts the energy but keeps the transaction. Out of service
  blocks new sessions and lets a running one finish.

While a transaction runs the library sends the meter every
``MeterValueSampleInterval`` (5 seconds unless the central system changes
it): energy register in Wh, power in W, current in A and voltage in V.

Shell
*****

The terminal that started the simulator is its console.

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - Command
     - Effect
   * - ``sim status``
     - backend, link state and the connectors
   * - ``sim plug <n>``, ``sim unplug <n>``
     - cable of connector ``n``
   * - ``sim tag <n> [idTag]``
     - present a card, the saved one if omitted
   * - ``sim stop <n>``
     - stop button
   * - ``sim ev <n> on|off``
     - the EV asks for energy
   * - ``sim evse <n> on|off``
     - the EVSE offers energy
   * - ``sim limit <n> <watts>``
     - power offered
   * - ``sim fault <n> on|off``
     - hardware fault
   * - ``sim service <n> on|off``
     - in or out of service
   * - ``sim url [ws://host:port/path | none]``
     - show or save the backend URL
   * - ``sim id [identity]``
     - show or save the charge box identity
   * - ``sim idtag <n> [idTag]``
     - show or save the card of a connector
   * - ``sim restart``
     - restart with the saved backend
   * - ``sim page main|diag``
     - page on the display
   * - ``sim view <n>``
     - connector on the display
   * - ``sim tap <x> <y>``
     - touch the display (``native_sim``)
   * - ``sim shot <file.ppm>``
     - save a screenshot (``native_sim``)

Commands can be piped in, which is how a session is scripted. Without a
display, ``SDL_VIDEODRIVER=dummy`` keeps SDL from opening a window:

.. code-block:: sh

   (sleep 3; echo "sim plug 1"; echo "sim tag 1"; sleep 20; echo "sim stop 1"; sleep 3) |
       SDL_VIDEODRIVER=dummy ./build/native/zephyr/zephyr.exe

Command line
************

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Option
     - Effect
   * - ``--url=<ws://host:port/path>``
     - backend for this run, not saved
   * - ``--id=<identity>``
     - charge box identity for this run, not saved
   * - ``--page=main|diag``
     - page to open
   * - ``--shot=<file.ppm>``
     - write a screenshot and keep running
   * - ``--shot-delay=<ms>``
     - when to take it, 1500 ms by default
   * - ``--flash=<file>``
     - where the settings are kept, ``flash.bin`` in the current directory by
       default

Configuration
*************

Options of ``app/Kconfig``, set in ``app/prj.conf`` or on the ``west build``
command line:

.. list-table::
   :header-rows: 1
   :widths: 50 15 35

   * - Option
     - Default
     - Meaning
   * - ``CONFIG_OCPP_SIM_APP_CONNECTORS``
     - 2
     - number of connectors, 1 to 4
   * - ``CONFIG_OCPP_SIM_APP_MAX_POWER_W``
     - 11000
     - rating of a connector
   * - ``CONFIG_OCPP_SIM_APP_VOLTAGE_V``
     - 230
     - phase voltage
   * - ``CONFIG_OCPP_SIM_APP_PHASES``
     - 3
     - number of phases
   * - ``CONFIG_OCPP_SIM_APP_AUTH_TIMEOUT_S``
     - 30
     - how long an accepted card waits for the cable
   * - ``CONFIG_OCPP_SIM_APP_URL``
     - ``ws://127.0.0.1:8081``
     - backend URL until one is saved
   * - ``CONFIG_OCPP_SIM_APP_CHARGE_BOX_ID``
     - ``zephyr-sim-01``
     - identity until one is saved
   * - ``CONFIG_OCPP_SIM_APP_ID_TAG_PREFIX``
     - ``ZEPHYR-``
     - card of a connector until one is saved; the connector number is
       appended
   * - ``CONFIG_OCPP_SIM_APP_VENDOR``, ``_MODEL``, ``_SERIAL``
     - ``Zephyr``, ``ocpp-sim``, ``SIM-0001``
     - what the BootNotification reports
   * - ``CONFIG_OCPP_SIM_APP_REQUEST_TIMEOUT_MS``
     - 8000
     - time to wait for an answer of the central system
   * - ``CONFIG_OCPP_SIM_APP_PEEK``
     - y
     - read link state and transaction id from the library's internals, see
       :doc:`architecture`

The backend URL has the form ``ws://host[:port][/path]``; the charge box
identity is appended as the last path segment.

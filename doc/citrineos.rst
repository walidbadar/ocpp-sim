..
   Copyright (c) 2026 Muhammad Waleed Badar
   SPDX-License-Identifier: Apache-2.0

CitrineOS
#########

`CitrineOS <https://github.com/citrineos/citrineos-core>`_ is the central
system the simulator is set up for. Any other OCPP 1.6J central system works
the same way: enter its URL on the diagnostics page or with ``sim url``, and
restart.

The local stack
***************

``tools/citrineos/compose.yml`` starts the OCPP server of CitrineOS with
RabbitMQ and PostgreSQL from the published images. It leaves out the operator
UI, Hasura and the certificate authorities of the full stack.

.. code-block:: sh

   docker compose -f tools/citrineos/compose.yml up -d
   docker compose -f tools/citrineos/compose.yml down        # -v also drops the database

With Podman, ``podman compose`` takes the same arguments.

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Address
     - What
   * - ``ws://127.0.0.1:8081/<identity>``
     - OCPP 1.6 and 2.x without authentication; unknown stations are accepted
   * - ``http://127.0.0.1:8080/docs``
     - REST API with its Swagger page

Both listen on this machine only. ``CITRINEOS_BIND=0.0.0.0`` in the
environment of the ``up`` command opens them to the network, and
``CITRINEOS_TAG`` selects another image than ``v2.1.0``.

.. warning::

   The stack is a test bench. Its REST API runs without authentication
   (``CITRINEOS_AUTH_MODE=localBypass``) and its OCPP port takes any station.
   Do not expose it beyond a network you trust.

Setting up the station
**********************

.. code-block:: sh

   tools/citrine.py setup

creates the station ``zephyr-sim-01`` with two connectors and accepts the
cards ``ZEPHYR-1`` and ``ZEPHYR-2``. Other names and counts:

.. code-block:: sh

   tools/citrine.py --station bench-02 setup --connectors 4 --tags CARD-A CARD-B

Two things make this necessary:

- CitrineOS refuses a card it does not know.
- CitrineOS learns the connectors of a station from StatusNotification, and
  Zephyr's OCPP library does not send that message. Without the connectors
  it answers StartTransaction with a refusal and logs ``Unable to find
  connector``.

The records are written into the database of CitrineOS with ``psql`` in its
database container, because CitrineOS manages them through its operator UI.
The cards are allowed concurrent transactions: a simulator that is closed
while charging leaves its transaction open in CitrineOS, which would
otherwise refuse the card from then on.

tools/citrine.py
****************

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - Command
     - Effect
   * - ``setup [--connectors n] [--tags ...]``
     - station, connectors and cards
   * - ``tag <idTag> [--status Blocked]``
     - accept a card, or give it another status
   * - ``tags``, ``stations``, ``transactions``
     - what CitrineOS has
   * - ``start <connector> <idTag>``
     - RemoteStartTransaction
   * - ``stop [transaction]``
     - RemoteStopTransaction, of the running transaction if none is named
   * - ``unlock <connector>``
     - UnlockConnector
   * - ``getconf [key ...]``
     - GetConfiguration
   * - ``setconf <key> <value>``
     - ChangeConfiguration, for example ``MeterValueSampleInterval 2``

``--station`` names another charge box than ``zephyr-sim-01``, ``--api``
another REST address. The container tool is found on its own; ``--runtime``
forces ``podman`` or ``docker``.

The requests to the charge point go through the REST API of CitrineOS
(``POST /ocpp/1.6/<module>/<action>?identifier=<station>&tenantId=1``). Its
answer says that the request was sent; what the charge point did with it is
on the display and in the log of the simulator.

What CitrineOS records
**********************

.. code-block:: none

   $ tools/citrine.py transactions
   transaction  station        connector  active  meterStart  kWh    meter values  stopped  start
   12           zephyr-sim-01  1          f       0           0.023  4             Local    22:06:11

``kWh`` is the energy between StartTransaction and StopTransaction, ``meter
values`` the number of MeterValues messages stored for the transaction.
``stopped`` is always ``Local``: the library does not send a stop reason.

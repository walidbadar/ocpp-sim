..
   Copyright (c) 2026 Muhammad Waleed Badar
   SPDX-License-Identifier: Apache-2.0

Getting started
###############

Requirements
************

- Zephyr 4.5 with the ``lvgl``, ``mbedtls`` and ``tf-psa-crypto`` modules.
  The manifest pins ``v4.5.0``.
- The SDL2 development package for the ``native_sim`` display.
- For the VIEWE board, the ``hal_espressif`` module with its Wi-Fi blobs.
- Docker or Podman with Compose for CitrineOS, and Python 3 for
  ``tools/citrine.py``.

Setting up a workspace
**********************

The repository is a west manifest repository. Either build it in a Zephyr
workspace that is already there, or create one from it:

.. code-block:: sh

   mkdir ocpp-sim-workspace && cd ocpp-sim-workspace
   git clone https://github.com/walidbadar/ocpp-sim
   west init -l ocpp-sim
   west update

Building
********

.. code-block:: sh

   west build -b native_sim/native/64 -d build/native ocpp-sim/app

The window is 800x480. For a 480x320 display:

.. code-block:: sh

   west build -b native_sim/native/64 -d build/native-480 ocpp-sim/app -- \
       -DEXTRA_DTC_OVERLAY_FILE=overlays/native_sim_480x320.overlay

On a board
==========

The VIEWE UEDX32480035E-WB-A (ESP32-S3, 480x320 touch display, Wi-Fi) is
configured. Its Wi-Fi network is given to the build and joined at every
start:

.. code-block:: sh

   west blobs fetch hal_espressif
   west build -b uedx32480035e_wb_a/esp32s3/procpu -d build/viewe ocpp-sim/app -- \
       -DCONFIG_WIFI_CREDENTIALS_STATIC=y \
       -DCONFIG_WIFI_CREDENTIALS_STATIC_SSID=\"<SSID>\" \
       -DCONFIG_WIFI_CREDENTIALS_STATIC_PASSWORD=\"<password>\"
   west flash -d build/viewe

The board is built without the shell: the USB connector carries the log and
the display does the rest. The address of the central system is entered on the display: the round button
in the top bar opens the diagnostics, tap the backend URL, then **Restart**.
The central system has to listen on the network for this, see
:doc:`citrineos`.

Starting the central system
***************************

.. code-block:: sh

   docker compose -f ocpp-sim/tools/citrineos/compose.yml up -d     # or: podman compose
   ocpp-sim/tools/citrine.py setup

The first command starts CitrineOS, the second creates the station
``zephyr-sim-01`` with two connectors and accepts the cards ``ZEPHYR-1``
and ``ZEPHYR-2``. :doc:`citrineos` explains why that is needed.

A first charging session
************************

.. code-block:: sh

   ./build/native/zephyr/zephyr.exe

The simulator connects to ``ws://127.0.0.1:8081/zephyr-sim-01``; the status
pill in the top bar shows *Available* and the button next to it turns green
when the BootNotification was accepted.

1. Tap the socket to plug the cable in.
2. Tap **Start charging**. The card is authorised and the transaction starts.
3. The energy counts up. **EV** and **EVSE** pause and resume, **Stop
   charging** ends the transaction.
4. Tap **Unplug to finish**.

Then look at what the central system recorded:

.. code-block:: sh

   ocpp-sim/tools/citrine.py transactions

Repository layout
*****************

.. code-block:: none

   app/                 the application
     src/               sources, see the architecture page
     boards/            native_sim configuration, display of the VIEWE board
     socs/              ESP32-S3: Wi-Fi and memory
     overlays/          other display sizes
     fonts/             Montserrat Bold for the headlines
   tests/evse/          unit tests of the connector logic and the URL parser
   tools/citrine.py     station, cards and remote commands in CitrineOS
   tools/citrineos/     Compose file of the local CitrineOS
   doc/                 this documentation
   west.yml             manifest

Tests
*****

.. code-block:: sh

   west twister -p native_sim/native/64 -T ocpp-sim/tests -T ocpp-sim/app --integration

``tests/evse`` runs 36 cases on the connector logic and the URL parser; the
application test builds the simulator.

Building this documentation
***************************

.. code-block:: sh

   pip install -r doc/requirements.txt
   sphinx-build -b html doc doc/_build/html

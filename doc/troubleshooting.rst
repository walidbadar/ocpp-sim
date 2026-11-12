..
   Copyright (c) 2026 Muhammad Waleed Badar
   SPDX-License-Identifier: Apache-2.0

Troubleshooting
###############

The simulator logs to its console; the OCPP library logs there too, as
``ocpp``.

Connecting
**********

The link button stays amber, ``ocpp: tcp socket connect fail -1 111``
   Nothing listens at the backend address. Check that CitrineOS runs
   (``docker compose -f tools/citrineos/compose.yml ps``) and what the
   simulator connects to (``sim status``).

``needs a build with TLS sockets``
   The URL starts with ``wss://``. Only ``ws://`` is supported.

A new URL or identity has no effect
   It is saved, but used from the next start: ``sim restart`` or **Restart**
   on the diagnostics page.

The simulator connects to an old backend
   The URL is saved in ``flash.bin`` in the directory the simulator was
   started from. Delete the file to return to the defaults, or pass
   ``--flash=<file>``.

Charging
********

``Authorize ...: Invalid``
   The central system does not know the card:
   ``tools/citrine.py tag <idTag>``.

``StartTransaction: not authorised`` although the card was accepted
   CitrineOS has no connectors for the station; its log says ``Unable to find
   connector``. Run ``tools/citrine.py setup``, with ``--station`` if the
   identity is not ``zephyr-sim-01``.

   The same happens with a card that was not added by ``tools/citrine.py``
   after the simulator was closed while charging: the transaction is still
   open in CitrineOS. ``tools/citrine.py tag <idTag>`` lifts that.

``Authorize ...: no answer (-107)``
   The simulator is not on line; see above. ``-11`` means the central system
   did not answer in time.

``... is longer than the 19 characters the OCPP library of this Zephyr can send``
   The OCPP library of Zephyr 4.5 mangles a card of 20 characters, the
   longest OCPP allows. The simulator refuses it there; Zephyr after 4.5
   takes it.

A card is ignored
   Another card started the session, or a request is still on its way. The
   log says ``the connector is busy``.

The backend shows no connector status
   Zephyr's OCPP library does not send StatusNotification.

The local CitrineOS
*******************

``permission denied`` from ``docker compose``
   The user may not use Docker. ``podman compose`` runs the same file without
   root; it needs the Podman socket
   (``systemctl --user start podman.socket``).

A port is in use
   CitrineOS takes 8080 and 8081. Change the left side of the port mappings
   in ``tools/citrineos/compose.yml`` and use the new port in the backend
   URL and in ``tools/citrine.py --api``.

``database container ... is not running`` from ``tools/citrine.py``
   The stack is down, or was started under another project name. Pass
   ``--db-container``.

Running without a display
*************************

``SDL_VIDEODRIVER=dummy`` keeps SDL from opening a window. ``sim tap`` and
``sim shot`` still work.

Limits
******

Of Zephyr's OCPP library, which is marked experimental:

- No StatusNotification.
- No HTTP basic authentication; this application does not set up TLS. That
  leaves ``ws://`` to an unauthenticated endpoint (security profile 0).
- The backend cannot be changed while running.
- No stop reason in StopTransaction.
- Transactions started or stopped while off line are not queued.
- No API for the link state or the transaction id, see :doc:`architecture`.
- In Zephyr 4.5, cards of at most 19 characters.

Of the simulator:

- Landscape displays from 480x320.
- ``native_sim`` and the VIEWE UEDX32480035E-WB-A are configured. Another
  board needs its network set-up and enough memory for LVGL.
- The clock and the timestamps are UTC.

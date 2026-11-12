# Copyright (c) 2026 Muhammad Waleed Badar
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ocpp_sim_app LANGUAGES C)

target_sources(app PRIVATE
  src/main.c
  src/config.c
  src/cp.c
  src/evse.c
  src/net.c
  src/ui.c
  src/url.c
)

# Montserrat Bold for the headlines, as an array for LVGL's TinyTTF
generate_inc_file_for_target(app
  fonts/Montserrat-Bold-latin.ttf
  ${ZEPHYR_BINARY_DIR}/include/generated/montserrat_bold.ttf.inc)

# Link state and transaction id come from the OCPP library's own structures
# until it has an API for them: one file, behind one option.
if(CONFIG_OCPP_SIM_APP_PEEK)
  target_sources(app PRIVATE src/ocpp_peek.c)
  set_source_files_properties(src/ocpp_peek.c PROPERTIES
    INCLUDE_DIRECTORIES ${ZEPHYR_BASE}/subsys/net/lib/ocpp)
endif()

# native_sim: command line options (backend, screenshots) and the host clock
target_sources_ifdef(CONFIG_BOARD_NATIVE_SIM app PRIVATE src/host.c)

# The "sim" commands
target_sources_ifdef(CONFIG_SHELL app PRIVATE src/sim_shell.c)

# Boards with Wi-Fi: join the stored network
target_sources_ifdef(CONFIG_WIFI app PRIVATE src/wifi.c)

# Copyright (c) 2026 Muhammad Waleed Badar
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ocpp_sim_evse_test LANGUAGES C)

# The connector logic and the backend URL parser of the application: plain
# C without I/O, built here on their own.
set(APP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../app/src)

target_include_directories(app PRIVATE ${APP_SRC})
target_sources(app PRIVATE
  ${APP_SRC}/evse.c
  ${APP_SRC}/url.c
  src/test_evse.c
  src/test_url.c
)

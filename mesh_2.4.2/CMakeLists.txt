# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
set(QEMU_EXTRA_FLAGS -s)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mesh)

target_sources(app PRIVATE src/main.c)
target_sources(app PRIVATE src/spi.c)
target_sources(app PRIVATE src/mymodel.c)
target_include_directories(app PRIVATE src/include)

if (BOARD STREQUAL bbc_microbit)
  target_sources(app PRIVATE src/microbit.c)
else()
  target_sources(app PRIVATE src/board.c)
endif()

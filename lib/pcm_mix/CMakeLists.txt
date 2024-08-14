#
# Copyright (c) 2022 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#
# Test change to enable tests

zephyr_library()
zephyr_library_sources(
	pcm_mix.c
)

zephyr_include_directories(.)

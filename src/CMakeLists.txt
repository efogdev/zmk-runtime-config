# Copyright (c) 2024 The ZMK Contributors
# SPDX-License-Identifier: MIT

add_subdirectory(shell)
target_sources_ifdef(CONFIG_ZMK_RUNTIME_CONFIG app PRIVATE runtime_config.c)

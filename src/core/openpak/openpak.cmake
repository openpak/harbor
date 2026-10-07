# SPDX-FileCopyrightText: 2026 tobagin
# SPDX-License-Identifier: AGPL-3.0-only

# Harbor's OpenPak host layer, part of core. Included from src/core/CMakeLists.txt.

# Everything here but the async worker is free of kernel and service code, so the self-check
# below can build it without linking core.
set(HARBOR_OPENPAK_SOURCES
    openpak/network_ceiling.cpp
    openpak/network_profile.cpp
    openpak/network_profile.h
    openpak/platform.cpp
    openpak/platform.h
    openpak/secret_store.cpp
    openpak/secret_store.h
)

target_sources(core PRIVATE
    ${HARBOR_OPENPAK_SOURCES}
)

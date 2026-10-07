# SPDX-FileCopyrightText: 2026 tobagin
# SPDX-License-Identifier: AGPL-3.0-only

# Harbor's OpenPak host layer, part of core. Included from src/core/CMakeLists.txt.

# Everything here but the async worker is free of kernel and service code, so the self-check
# below can build it without linking core.
set(HARBOR_OPENPAK_SOURCES
    openpak/account.cpp
    openpak/account.h
    openpak/baas.cpp
    openpak/baas.h
    openpak/baas_blocks.cpp
    openpak/baas_detail.h
    openpak/baas_models.h
    openpak/baas_parsing.cpp
    openpak/baas_requests.cpp
    openpak/bcat.cpp
    openpak/bcat.h
    openpak/boot_connection.cpp
    openpak/boot_connection.h
    openpak/friend_structs.cpp
    openpak/friend_structs.h
    openpak/friends_cache.cpp
    openpak/friends_cache.h
    openpak/network_ceiling.cpp
    openpak/network_profile.cpp
    openpak/network_profile.h
    openpak/platform.cpp
    openpak/platform.h
    openpak/presence_state.cpp
    openpak/presence_state.h
    openpak/push.cpp
    openpak/push.h
    openpak/secret_store.cpp
    openpak/service_hooks.cpp
    openpak/service_hooks.h
    openpak/secret_store.h
    openpak/session.cpp
    openpak/session.h
    openpak/session_detail.h
)

target_sources(core PRIVATE
    ${HARBOR_OPENPAK_SOURCES}
    openpak/async_worker.cpp
    openpak/async_worker.h
)

# harbor-openpak-check: the self-check (ceiling signatures, redirect precedence, fail-closed
# families, the CA pin) and a boot connection against a data directory. Off by default.
option(HARBOR_OPENPAK_CHECK "Build harbor-openpak-check" OFF)
if (HARBOR_OPENPAK_CHECK)
    list(TRANSFORM HARBOR_OPENPAK_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
    add_executable(harbor-openpak-check openpak/check/openpak_check.cpp ${HARBOR_OPENPAK_SOURCES})
    target_include_directories(harbor-openpak-check PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(harbor-openpak-check PRIVATE common fmt::fmt nlohmann_json::nlohmann_json
                          httplib::httplib OpenSSL::SSL OpenSSL::Crypto)
endif()

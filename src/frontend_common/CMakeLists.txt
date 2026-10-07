# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2023 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

add_library(frontend_common STATIC
    config.cpp
    config.h
    content_manager.h
    firmware_manager.h
    firmware_manager.cpp
    data_manager.h data_manager.cpp
    play_time_manager.cpp
    play_time_manager.h
    settings_generator.h settings_generator.cpp
    mod_manager.h mod_manager.cpp
    joycon_rails.cpp
    joycon_rails.h)

if (ENABLE_UPDATE_CHECKER)
    target_link_libraries(frontend_common PRIVATE httplib::httplib)
    target_link_libraries(frontend_common PRIVATE nlohmann_json::nlohmann_json)
    target_sources(frontend_common PRIVATE
        update_checker.cpp
        update_checker.h)
    target_link_libraries(frontend_common PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

# OpenPak's credentials in the OS password store: one backend per platform (secret_store.h).
target_sources(frontend_common PRIVATE secret_store.cpp secret_store.h)
if (ANDROID)
    target_sources(frontend_common PRIVATE secret_store_android.cpp)
elseif (APPLE)
    target_sources(frontend_common PRIVATE secret_store_apple.cpp)
    target_link_libraries(frontend_common PRIVATE "-framework Security" "-framework CoreFoundation")
elseif (WIN32)
    target_sources(frontend_common PRIVATE secret_store_windows.cpp)
    target_link_libraries(frontend_common PRIVATE advapi32 crypt32)
else()
    # Linux and the BSDs: the Secret Service through libsecret, when it is installed.
    target_sources(frontend_common PRIVATE secret_store_libsecret.cpp)
    find_package(PkgConfig QUIET)
    if (PkgConfig_FOUND)
        pkg_check_modules(LIBSECRET QUIET IMPORTED_TARGET libsecret-1)
    endif()
    if (LIBSECRET_FOUND)
        target_link_libraries(frontend_common PRIVATE PkgConfig::LIBSECRET)
        target_compile_definitions(frontend_common PRIVATE HARBOR_HAS_LIBSECRET)
    else()
        message(STATUS "libsecret not found: OpenPak keeps its credentials in memory on this build")
    endif()
endif()

create_target_directory_groups(frontend_common)
target_link_libraries(frontend_common
    PUBLIC core SimpleIni::SimpleIni frozen::frozen-headers
    PRIVATE common hid_core Boost::headers ZLIB::ZLIB)

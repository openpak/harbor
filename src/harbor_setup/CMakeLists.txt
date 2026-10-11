# SPDX-FileCopyrightText: 2026 tobagin
# SPDX-License-Identifier: AGPL-3.0-only

# Harbor's first-run setup, drawn with Borealis (XITRIX fork, Apache-2.0). The fork is pinned in
# cpmfile.json and patched from .patch/borealis; only the parts the setup uses are built, against
# Eden's own SDL3, fmt, glad and nlohmann_json, so there is one copy of each in the binary.
# Notices: NOTICE in this directory.

AddJsonPackage(NAME borealis DOWNLOAD_ONLY)
set(BOREALIS "${borealis_SOURCE_DIR}/library")

file(GLOB BOREALIS_SOURCES
    "${BOREALIS}/lib/core/*.cpp"
    "${BOREALIS}/lib/core/touch/*.cpp"
    "${BOREALIS}/lib/views/*.cpp"
    "${BOREALIS}/lib/views/cells/*.cpp"
    "${BOREALIS}/lib/views/widgets/*.cpp"
    "${BOREALIS}/lib/platforms/sdl/*.cpp"
    "${BOREALIS}/lib/extern/yoga/yoga/*.cpp"
    "${BOREALIS}/lib/extern/yoga/yoga/event/*.cpp")
list(APPEND BOREALIS_SOURCES
    "${BOREALIS}/lib/platforms/desktop/desktop_font.cpp"
    "${BOREALIS}/lib/platforms/desktop/desktop_ime.cpp"
    "${BOREALIS}/lib/platforms/desktop/desktop_platform.cpp"
    "${BOREALIS}/lib/platforms/desktop/desktop_winrt.cpp"
    "${BOREALIS}/lib/platforms/desktop/steam_deck.cpp"
    "${BOREALIS}/lib/extern/libretro-common/compat/compat_strl.c"
    "${BOREALIS}/lib/extern/libretro-common/encodings/encoding_utf.c"
    "${BOREALIS}/lib/extern/libretro-common/features/features_cpu.c"
    "${BOREALIS}/lib/extern/nanovg/nanovg.c"
    "${BOREALIS}/lib/extern/tinyxml2/tinyxml2.cpp")
if (ANDROID)
    list(APPEND BOREALIS_SOURCES "${BOREALIS}/lib/platforms/android/android_platform.cpp")
elseif (APPLE)
    # The two hooks the desktop platform takes from the fork's Objective-C++ file, in plain C++.
    list(APPEND BOREALIS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/borealis_darwin.cpp")
endif()

add_library(borealis STATIC ${BOREALIS_SOURCES})
# SYSTEM: Borealis' headers are not held to Eden's warnings-as-errors where the setup includes them.
target_include_directories(borealis SYSTEM PUBLIC
    "${BOREALIS}/include"
    "${BOREALIS}/include/borealis/extern"
    "${BOREALIS}/include/borealis/extern/nanovg"
    "${BOREALIS}/include/borealis/extern/tinyxml2"
    "${BOREALIS}/lib/extern/tweeny/include"
    "${BOREALIS}/lib/extern/yoga")
# Eden's glad and nlohmann_json come before Borealis' bundled copies of the same headers.
target_include_directories(borealis SYSTEM BEFORE PUBLIC
    $<TARGET_PROPERTY:glad,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(borealis PUBLIC SDL3::SDL3 fmt::fmt glad nlohmann_json::nlohmann_json)
target_compile_definitions(borealis PUBLIC
    __SDL3__
    BOREALIS_USE_OPENGL
    SIMPLE_HIGHLIGHT # a static focus ring, as in the design
    YG_ENABLE_EVENTS
    [[BRLS_RESOURCES="@harbor-setup/"]]) # no resource files: the setup embeds its font
if (ANDROID)
    # The fork's sdl_input.cpp tests PLATFORM_ANDROID, which its own toolchain never defines.
    target_compile_definitions(borealis PUBLIC ANDROID PLATFORM_ANDROID USE_GLES2)
    target_link_libraries(borealis PUBLIC android log GLESv2 EGL)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(borealis PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()
if (WIN32)
    # The desktop platform's network and power queries.
    target_link_libraries(borealis PUBLIC wlanapi iphlpapi ws2_32)
elseif (APPLE)
    target_link_libraries(borealis PUBLIC "-framework IOKit" "-framework SystemConfiguration" "-framework CoreFoundation")
endif()
if (MSVC)
    # The fork is built with GCC-style toolchains (MinGW on Windows): unistd.h and strings.h come
    # from small stand-ins, and its task thread is std::thread instead of pthreads.
    target_include_directories(borealis SYSTEM BEFORE PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/msvc")
    target_compile_definitions(borealis PUBLIC BOREALIS_USE_STD_THREAD)
    set(HARBOR_SETUP_RTTI /GR)
else()
    set(HARBOR_SETUP_RTTI -frtti)
endif()
# Eden's common already carries stb_image; nanovg's copy stays private to nanovg.c.
set_source_files_properties("${BOREALIS}/lib/extern/nanovg/nanovg.c" PROPERTIES COMPILE_DEFINITIONS STB_IMAGE_STATIC)
# Third-party code: built with its own expectations (RTTI) and without Eden's warnings-as-errors
# (cl takes -w as /w).
target_compile_options(borealis PRIVATE -w $<$<COMPILE_LANGUAGE:CXX>:${HARBOR_SETUP_RTTI}>)
set_target_properties(borealis PROPERTIES CXX_STANDARD 20 POSITION_INDEPENDENT_CODE ON)

# The text font, Noto Sans (OFL-1.1), embedded so no resource files ship next to the binary.
set(HARBOR_SETUP_FONT "${CMAKE_CURRENT_SOURCE_DIR}/resources/NotoSans-Regular.ttf")
set(HARBOR_SETUP_FONT_CPP "${CMAKE_CURRENT_BINARY_DIR}/harbor_setup_font.cpp")
add_custom_command(
    OUTPUT "${HARBOR_SETUP_FONT_CPP}"
    COMMAND ${CMAKE_COMMAND} -DINPUT=${HARBOR_SETUP_FONT} -DOUTPUT=${HARBOR_SETUP_FONT_CPP}
            -DSYMBOL=harbor_setup_font -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake"
    DEPENDS "${HARBOR_SETUP_FONT}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake"
    VERBATIM)

# The flow, the record and the installs: no Borealis, so the self-check runs without a window.
add_library(harbor_setup_core STATIC
    console_preset.cpp
    device_check.cpp
    device_check.h
    device_probe.cpp
    driver_catalog.cpp
    driver_catalog.h
    driver_recovery.cpp
    driver_recovery.h
    harbor_setup.h
    host.h
    keys.cpp
    keys.h
    menu.h
    menu_input.cpp
    menu_input.h
    record.cpp
    record.h
    title_settings.cpp
    title_settings.h
    wizard.cpp
    wizard.h)
target_link_libraries(harbor_setup_core PUBLIC common core frontend_common hid_core input_common video_core nlohmann_json::nlohmann_json
                      PRIVATE Vulkan::Headers)
set_target_properties(harbor_setup_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The wizard's window, and the Harbor menu drawn offscreen onto the console's screen.
add_library(harbor_setup STATIC
    chime.cpp
    chime.h
    harbor_setup.cpp
    menu.cpp
    menu_view.cpp
    menu_view.h
    ui.cpp
    ui.h
    view.cpp
    view.h
    "${HARBOR_SETUP_FONT_CPP}")
if (NOT ANDROID)
    target_sources(harbor_setup PRIVATE desktop.cpp)
endif()
target_link_libraries(harbor_setup PUBLIC harbor_setup_core PRIVATE borealis SDL3::SDL3 ${CMAKE_DL_LIBS})
# Its classes derive from Borealis' views, which are built with RTTI.
target_compile_options(harbor_setup PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${HARBOR_SETUP_RTTI}>)
set_target_properties(harbor_setup PROPERTIES POSITION_INDEPENDENT_CODE ON)

if (NOT ANDROID)
    # harbor_setup_run: the desktop wizard on its own, against the current user directory (or ./user).
    # core calls back into frontend_common (play time): the static libraries are linked as one group
    # where the linker is single-pass (GNU and LLVM on ELF); MSVC's and Apple's linkers look again.
    if (CMAKE_CXX_LINK_GROUP_USING_RESCAN_SUPPORTED OR CMAKE_LINK_GROUP_USING_RESCAN_SUPPORTED)
        set(HARBOR_SETUP_CORE_GROUP "$<LINK_GROUP:RESCAN,core,frontend_common,video_core,audio_core,hid_core>")
    else()
        set(HARBOR_SETUP_CORE_GROUP core frontend_common video_core audio_core hid_core)
    endif()
    add_executable(harbor_setup_run run_main.cpp vma_impl.cpp)
    target_link_libraries(harbor_setup_run PRIVATE harbor_setup ${HARBOR_SETUP_CORE_GROUP})

    # The allocator's header is third-party code, not held to Eden's warnings-as-errors.
    set_source_files_properties(vma_impl.cpp PROPERTIES COMPILE_OPTIONS -w)

    # harbor_setup_selftest: the flow, record, checks and driver filter, without a window.
    add_executable(harbor_setup_selftest selftest.cpp vma_impl.cpp)
    target_link_libraries(harbor_setup_selftest PRIVATE harbor_setup ${HARBOR_SETUP_CORE_GROUP})
endif()

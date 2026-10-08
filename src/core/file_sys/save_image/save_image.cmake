# SPDX-FileCopyrightText: 2026 tobagin
# SPDX-License-Identifier: AGPL-3.0-only

# harbor-save-image-check: the save-image codec's self-check (round trips, the stream layout, a
# corrupt stream leaving the target alone) and export/import of host directories for scripted
# tests. Built with -DHARBOR_SAVE_IMAGE_CHECK=ON; links common and the VFS files only, not core.
option(HARBOR_SAVE_IMAGE_CHECK "Build harbor-save-image-check" OFF)
if (HARBOR_SAVE_IMAGE_CHECK)
    add_executable(harbor-save-image-check
        ${CMAKE_CURRENT_SOURCE_DIR}/file_sys/save_image/check/save_image_check.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/file_sys/save_image/save_image.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/file_sys/save_image/save_image_core.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/file_sys/vfs/vfs.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/file_sys/vfs/vfs_real.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/file_sys/vfs/vfs_vector.cpp)
    target_include_directories(harbor-save-image-check PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(harbor-save-image-check PRIVATE common fmt::fmt OpenSSL::Crypto)
endif()

# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2019 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# generate git/build information
include(GetSCMRev)

function(get_timestamp _var)
    string(TIMESTAMP timestamp UTC)
    set(${_var} "${timestamp}" PARENT_SCOPE)
endfunction()

get_timestamp(BUILD_DATE)

if (DEFINED GIT_RELEASE)
    set(BUILD_VERSION "${GIT_TAG}")
    set(GIT_REFSPEC "${GIT_RELEASE}")
    set(IS_DEV_BUILD false)
else()
    string(SUBSTRING ${GIT_COMMIT} 0 10 BUILD_VERSION)
    set(BUILD_VERSION "${BUILD_VERSION}-${GIT_REFSPEC}")
    # Local builds follow Harbor's release version and commit count. The rolling
    # nightly tag must not replace that release base; CI supplies GIT_RELEASE above.
    run_git_command(HARBOR_RELEASE_DESC describe --tags --match "v[0-9]*" --long)
    if (HARBOR_RELEASE_DESC MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+.*-g[0-9a-f]+$")
        string(REGEX REPLACE "(-0)?-g[0-9a-f]+$" "" BUILD_VERSION "${HARBOR_RELEASE_DESC}")
    endif()
    set(IS_DEV_BUILD true)
endif()

if (NIGHTLY_BUILD)
    set(IS_NIGHTLY_BUILD true)
else()
    set(IS_NIGHTLY_BUILD false)
endif()

message(STATUS "[Harbor] Build version: ${BUILD_VERSION}")
set(GIT_DESC ${BUILD_VERSION})

# Generate cpp with Git revision from template

# TODO(crueter): Stable releases feed.
set(BUILD_AUTO_UPDATE_STABLE_REPO "eden-emu/eden")
set(BUILD_AUTO_UPDATE_STABLE_API "git.eden-emu.dev")
set(BUILD_AUTO_UPDATE_STABLE_API_PATH "/api/v1/repos/")

set(BUILD_AUTO_UPDATE_API_PATH "/latest/release.json")
if (NIGHTLY_BUILD)
    set(BUILD_AUTO_UPDATE_WEBSITE "https://git.eden-emu.dev")
    set(BUILD_AUTO_UPDATE_API "nightly.eden-emu.dev")
    set(BUILD_AUTO_UPDATE_REPO "eden-ci/nightly")
    set(REPO_NAME "Eden Nightly")
else()
    set(BUILD_AUTO_UPDATE_WEBSITE "https://git.eden-emu.dev")
    set(BUILD_AUTO_UPDATE_API "stable.eden-emu.dev")
    set(BUILD_AUTO_UPDATE_REPO "eden-emu/eden")
    set(REPO_NAME "Eden")
endif()

set(BUILD_ID ${GIT_REFSPEC})
set(BUILD_FULLNAME "${REPO_NAME} ${BUILD_VERSION} ")
set(CXX_COMPILER "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

configure_file(scm_rev.cpp.in scm_rev.cpp @ONLY)

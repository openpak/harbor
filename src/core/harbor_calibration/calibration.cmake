# SPDX-FileCopyrightText: 2026 tobagin
# SPDX-License-Identifier: AGPL-3.0-only

# Harbor's per-install calibration (the PRODINFO a console has). Included from
# src/core/CMakeLists.txt. The self-check builds without CMake: see check/calibration_check.cpp.
target_sources(core PRIVATE
    harbor_calibration/cal0.cpp
    harbor_calibration/cal0.h
    harbor_calibration/calibration.cpp
    harbor_calibration/calibration.h
)

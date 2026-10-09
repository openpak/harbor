// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// macOS: Borealis' desktop platform takes two hooks from the fork's Objective-C++ file
// (desktop_darwin.mm). Harbor needs neither the Wi-Fi strength nor the Metal display-link run loop,
// so they are plain C++ here and the build needs no Objective-C++.

#include <functional>

namespace brls {

int darwin_wlan_quality() {
    return -1; // unknown
}

bool darwin_runloop(const std::function<bool()>& runLoopImpl) {
    return runLoopImpl();
}

} // namespace brls

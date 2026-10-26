// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Setup text entry uses Android's standard text field and installed keyboard on Android.
// Desktop uses Harbor's Borealis page, with controller, touch and physical keyboard input.

#pragma once

#include <functional>
#include <optional>
#include <string>

namespace HarborSetup {

struct TextEntryOptions {
    std::string title;
    /// A line of guidance above the field (the format, the limits); may be empty.
    std::string hint;
    std::string initial;
    /// In characters (code points); at most this many can be typed.
    int max_length = 64;
    /// A password: masked by the platform field, or bullets and a Show/Hide key on desktop.
    bool secret = false;
    /// Called once when the page has closed, before `done`, whichever way it closed.
    std::function<void()> closed;
};

#ifdef ANDROID
/// Android's standard text field and installed input method, owned by the setup activity.
void OpenAndroidTextEntry(const TextEntryOptions& options,
                         std::function<void(std::optional<std::string>)> finished);
void CancelAndroidTextEntry();
#endif

/// Opens the page on top of the current activity. `done` gets the text when it is confirmed and is
/// never called on cancel. False when a text page is already open.
bool OpenTextEntry(TextEntryOptions options, std::function<void(std::string)> done);

/// Closes the open page as a cancel (Android's Back reaches it this way); false when none is open.
bool CancelTextEntry();

bool TextEntryOpen();

} // namespace HarborSetup

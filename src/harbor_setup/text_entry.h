// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// The setup's text entry page: Harbor's own Borealis page in place of Borealis' small stock input
// dialog. Modelled on the Switch's software keyboard: a large field with a caret, a counter and an
// on-screen keyboard that works with the pad, with touch and with a desktop keyboard. Drawn on the
// same 1280x720 canvas, with the same palette and prompt bar as the wizard, on Android and desktop.

#pragma once

#include <functional>
#include <string>

namespace HarborSetup {

struct TextEntryOptions {
    std::string title;
    /// A line of guidance above the field (the format, the limits); may be empty.
    std::string hint;
    std::string initial;
    /// In characters (code points); at most this many can be typed.
    int max_length = 64;
    /// A password: the field shows bullets (with a Show/Hide key) and the text is only held here.
    bool secret = false;
    /// Called once when the page has closed, before `done`, whichever way it closed.
    std::function<void()> closed;
};

/// Opens the page on top of the current activity. `done` gets the text when it is confirmed and is
/// never called on cancel. False when a text page is already open.
bool OpenTextEntry(TextEntryOptions options, std::function<void(std::string)> done);

/// Closes the open page as a cancel (Android's Back reaches it this way); false when none is open.
bool CancelTextEntry();

bool TextEntryOpen();

} // namespace HarborSetup
